// ============================================================================
// Battery bench: everything this board can learn about a cell, over two full cycles.
//
// A TEST is C0 charge → D1 discharge → C1 charge → D2 discharge → C2 charge, about three and a half
// days. C0 starts wherever the cell is and only makes it full; D1 and D2 are measured from a charge
// the bench watched end down to the cutoff; C1 and C2 are measured from the cutoff up to the end of
// charge. Then the cell is swapped and the test run again, and tools/curve_fit.py --compare puts the
// two side by side. The first bench (2026-09-30) measured one discharge for the voltage-to-percent
// table; this one keeps that discharge, byte for byte in its log format, and adds everything around it.
//
// THE CABLE MOVES BY HAND at every arrow, and the screen says when. On this board the charger cannot
// be switched off (TP4056's CE is tied to VBUS) and the cell is disconnected from the load whenever
// USB is in (Q3), so the bench can neither stop a charge nor start a discharge by itself.
//
// WHAT A DISCHARGE MEASURES. The cell feeds the board through a linear regulator (XC6220), which
// draws from its input the current its output delivers whatever the input voltage; the radio kept
// awake (own access point, plus the home network when configured, modem power save off) is a steady
// load of about 80 mA. Every 15 minutes under load (5 on the steep tail below 3.55 V): 5 minutes
// with the radio off in automatic light sleep, then one RESTED reading — the curve. Also:
//   - the loaded reading at the end of each load phase (the sag after 15 minutes: resistance plus
//     the slow polarisation of the cell);
//   - a PULSE reading 10 s after each load phase starts (the fast part: ohmic and charge transfer);
//   - the temperature, from the BME690 next to the cell, at each rest.
// Time under load is the capacity axis. Its unit is hours; it becomes mAh only through the assumed
// load current (CurveSummary::LOAD_MA), which the screen marks with a tilde. The RATIO between two
// cells needs no current at all: same board, same firmware, same load.
//
// WHAT A CHARGE MEASURES. The board runs from USB, the cell takes the charger's current, the radio
// stays off. Every 2 minutes: the cell voltage on the charger and the temperature. Derived: the
// charge time (to STDBY#), where constant current ended (the voltage reached its plateau), the step
// at plug-in — which against the 10-second pulse of the discharge just before gives the charge
// current at the start (CurveSummary explains the arithmetic) — and, after the end, the voltage
// relaxing on the idle charger to what the full cell really holds.
//
// NOTHING LIVES ONLY IN RAM. After every boot and every cable event the bench reads the log back,
// asks CurveSummary what it says (next()), and acts. A reset, a brownout or a reflash in the middle of
// four days costs a line in the log.
//
// PROCEDURE:
//   1. Calibrate the divider with the product firmware first (console, Settings, Battery voltage).
//   2. Flash this bench (CURVE_BENCH 1 in main.cpp). With an empty log, the cable in starts a test.
//      An old log is kept until the button is held for three seconds — read it first.
//   3. Follow the bottom bar. If the charger says "full" at once (the cell is above TP4056's 4.05 V
//      restart point) the bench asks for the cable out, runs the cell below 4.00 V, and asks back.
//   4. When it says TEST COMPLETE: tools/curve_fit.py --port --save cellA.bin, then hold the button 3 s,
//      swap the cell, plug in. Compare with tools/curve_fit.py --compare cellA.bin cellB.bin.
// ============================================================================
#include "curve_bench.h"
#include "curve_log.h"
#include "curve_summary.h"
#include "curve_screen.h"
#include "battery_source.h"
#include "battery_spec.h"
#include "bme690.h"
#include "i2c_manager.h"
#include "nvs_manager.h"
#include "scd41.h"
#include "epaper.h"
#include "wifi_manager.h"
#include "curve_page.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "CURVE";

using CurveSummary::Next;

#define PIN_BTN   GPIO_NUM_2      // low = pressed, R10 pull-up on the board
#define PIN_USB   GPIO_NUM_3      // low = cable in
#define PIN_LED   GPIO_NUM_11     // blinks while a reading of the charge current is due

static constexpr int      LOAD_S        = 15 * 60;
static constexpr int      LOAD_S_TAIL   = 5 * 60;    // below TAIL_V: the curve is steep there
static constexpr float    TAIL_V        = 3.55f;
static constexpr int      REST_S        = 5 * 60;
static constexpr int      PULSE_S       = 10;        // the fast reading into a load phase
// Where a discharge stops, rested, on the scale in use. The 0 % point is BATTERY_CUTOFF_V (3.30) on
// the TRUE scale, and the tool can only find it if the run went below it — so the margin here is a
// bound on how wrong the calibration may be. 3.25 V (the first bench) covered +1.5 %; re-reading the
// first run with a gain only +1 % higher already loses its 0 % point ("ended above 3.30 V"), and the
// meter behind the present calibration is a ±2 % instrument. 3.20 V covers +3 %: about 1.5 % more
// of the cell, still far above where lithium suffers, and the loaded floor below stays.
static constexpr float    END_V_CAL     = 3.20f;     // rested, calibrated
static constexpr float    END_V_RAW     = 3.15f;     // rested, never calibrated
static constexpr float    LOAD_FLOOR_V  = 3.10f;     // under load, with the gain in use
static constexpr float    RUN_DOWN_V    = 4.00f;     // rested, with the gain: safely below 4.05
static constexpr uint32_t CHARGED_MIN_S = 60;        // "finished" counts only after this much charging
static constexpr uint32_t CHG_SAMPLE_S  = 120;
static constexpr uint32_t CHG_SCREEN_S  = 300;       // Good Display: refresh no more often than ~3 min
static constexpr uint16_t UDP_PORT      = 47800;

static EPaper s_epd;
static esp_pm_lock_handle_t s_awake = nullptr;
static int s_udp = -1;
static bool s_sta = false, s_ap = false;
static BME690 s_bme;
static bool s_bme_ok = false;
static CurveSummary::State s_st;          // the log, summarised; kept current by put()
static CurveLog::Rec s_last = {};
static bool s_have_last = false;
static bool s_clock_fixed = false;
static float s_rundown_rest = 0;          // the last rested reading of a run-down, raw

// ---- pins, time ------------------------------------------------------------------------------

static bool usb_in()
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_USB;
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);
    return gpio_get_level(PIN_USB) == 0;
}

static bool button_down()
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_BTN;
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);
    return gpio_get_level(PIN_BTN) == 0;
}

static uint32_t now_s() { return (uint32_t)time(nullptr); }
static uint32_t rel(uint32_t t0) { const uint32_t n = now_s(); return n > t0 ? n - t0 : 0; }
static bool calibrated() { return BatterySource::calibration() != BatterySource::CAL_NONE; }
static float end_v() { return calibrated() ? END_V_CAL : END_V_RAW; }

// ---- the log ------------------------------------------------------------------------------------

static void rescan()
{
    s_st.reset();
    const int n = CurveLog::next_free();
    CurveLog::Rec r;
    for (int i = 0; i < n; i++)
        if (CurveLog::read(i, r) && CurveLog::valid(r)) s_st.feed(r);
    // Times in the log are relative to absolute starts. The RTC keeps the clock through deep sleep
    // and software resets, but a power-on or brownout reset starts it from zero — and then every
    // "seconds since the start" would wrap. Set it forward to the last record: the gap is lost, which
    // the Reset record says (detail | 0x80), and nothing else is.
    s_clock_fixed = false;
    if (s_st.t_abs_last && now_s() < s_st.t_abs_last) {
        struct timeval tv = {(time_t)s_st.t_abs_last + 1, 0};
        settimeofday(&tv, nullptr);
        s_clock_fixed = true;
        ESP_LOGW(TAG, "clock behind the log: set forward to %lu", (unsigned long)s_st.t_abs_last + 1);
    }
}

static void udp_record(const CurveLog::Rec &r);

static void put(CurveLog::Kind kind, uint32_t t, uint32_t load_s, float v_rest, float v_load,
                uint8_t spread, uint8_t detail, uint8_t flags)
{
    CurveLog::Rec r;
    memset(&r, 0, sizeof(r));
    r.kind = kind; r.flags = flags; r.t = t; r.load_s = load_s;
    r.v_rest = v_rest; r.v_load = v_load;
    r.gain = BatterySource::calibration();
    r.seq = (uint16_t)(s_st.seq + 1);
    r.spread = spread; r.detail = detail;
    if (!CurveLog::append(r)) ESP_LOGE(TAG, "log write failed (full?) — record %u lost", (unsigned)r.seq);
    s_st.feed(r);
    ESP_LOGW(TAG, "#%u kind %u  t %lu  load %lu  %.4f / %.4f V  sp %u  det %u  fl %u", (unsigned)r.seq,
             (unsigned)kind, (unsigned long)t, (unsigned long)load_s, v_rest, v_load, (unsigned)spread,
             (unsigned)detail, (unsigned)flags);
    s_last = r; s_have_last = true;
    udp_record(r);                         // goes out only if the radio is up now
}

static bool raw_cell(float &v, uint8_t &spread)
{
    i2c_master_bus_handle_t bus = I2CManager::getInstance().getBusHandle();
    if (!BatterySource::readRawVoltage(bus, v)) { v = 0; spread = 0; return false; }
    const BatterySource::BurstStats st = BatterySource::lastBurst();
    spread = (uint8_t)(st.spread_lsb > 255 ? 255 : st.spread_lsb);
    return true;
}

// The BME690 sits beside the cell; its own heater stays off here (no gas reading), so what it reads
// is the board and the air around the cell.
static uint8_t temp_now()
{
    if (!s_bme_ok) return 0;
    float t = NAN, h, p, g; bool gv;
    if (!s_bme.readDataWithHeater(0, 0, BME69X_OS_2X, BME69X_OS_NONE, BME69X_OS_1X, false, t, h, p, g, gv))
        return 0;
    return CurveLog::temp_code(t);
}

// ---- the screen -------------------------------------------------------------------------------

static void draw(Next n, float v_now = 0, int next_load_min = 15, const char *alert = nullptr)
{
    // No light sleep while a frame is drawn. Automatic light sleep isolates the pads, the panel's
    // six lines and its supply gate among them, and waitBusy() sleeps between polls — so without
    // this the chip dozed off mid-refresh, the gate floated up (R9) and the frame died. Found on the
    // bench on 2026-09-30; on the cable it cannot happen (USB-Serial-JTAG blocks light sleep).
    if (s_awake) esp_pm_lock_acquire(s_awake);
    struct Release { ~Release() { if (s_awake) esp_pm_lock_release(s_awake); } } release;
    if (!s_epd.init()) return;
    CurveScreen::View v;
    v.next = n;
    v.v_now = v_now > 2.0f ? v_now * BatterySource::calibration() : 0;
    v.end_v = end_v();
    v.gain = BatterySource::calibration();
    v.calibrated = calibrated();
    v.usb = usb_in();
    v.now_abs = now_s();
    v.next_load_min = next_load_min;
    v.alert = alert;
    v.page = CurvePage::up() ? CurvePage::url() : nullptr;
    v.pin = CurvePage::pin();
    CurveScreen::draw(s_epd, s_st, v);
    s_epd.refresh(EPaper::Update::Full);
    s_epd.hibernate();
}

// Waiting for the cable, the bench used to wake, redraw and sleep every five seconds — board e004
// after D1, 2026-10-05, the cell at 3.2 V. Something woke it at once, every time; the likeliest is
// the USB-detect pin, which the product firmware already guards against (USB_STORM in
// bench_screens.cpp: 187 wakes out of 207 from that pin on 2026-08-29). A full frame and a boot
// every five seconds is ~15-20 mA, below the cut-off, onto a cell the next charge then refills —
// so it spoils the charge/discharge pairing as well as the panel. Two rules now:
//   * a waiting screen is drawn once — a wake that changes nothing does not repaint the glass;
//   * after two wakes by the USB pin with no cable in it, the pin is not armed again: the cable is
//     looked for once a minute by the timer instead, until a press or a real cable resets it.
RTC_NOINIT_ATTR static uint32_t s_wait_magic, s_wait_sig, s_spurious;
static constexpr uint32_t WAIT_MAGIC = 0x57A17C0Du;
static constexpr uint32_t SPURIOUS_MAX = 2;

static bool woke_by(gpio_num_t pin)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_GPIO) return false;
    return (esp_sleep_get_gpio_wakeup_status() >> pin) & 1;
}

// Deep sleep until the cable changes or the button is pressed. The boot after it reads the log and
// decides again — the bench never has to remember why it slept.
static void sleep_for_cable_or_button(bool drew = true)
{
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (s_wait_magic != WAIT_MAGIC) { s_wait_magic = WAIT_MAGIC; s_wait_sig = 0; s_spurious = 0; }
    const bool usb = usb_in();
    const bool storm = s_spurious >= SPURIOUS_MAX;
    uint64_t mask = 0;
    if (!button_down())   mask |= 1ULL << PIN_BTN;
    if (!usb && !storm)   mask |= 1ULL << PIN_USB;
    if (mask) esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
    // Cable in: its removal is a rising edge, which a LOW wake cannot see; look again every minute.
    // The same timer looks for a cable going IN when the pin has been dropped for waking at random.
    if (usb || !mask || storm) esp_sleep_enable_timer_wakeup(60ULL * 1000 * 1000);
    ESP_LOGW(TAG, "sleeping until the cable or the button%s", storm ? " (USB pin noisy: polling it every minute)" : "");
    // The panel went to sleep at the end of draw(); Good Display's sequence gives its rails time to
    // settle before the supply goes (the gate floats high in deep sleep, R9 turns it off). Nothing
    // to settle when nothing was drawn — and that wake costs a boot, not a boot plus two seconds.
    if (drew) vTaskDelay(pdMS_TO_TICKS(2000));
    esp_deep_sleep_start();
}

// A waiting screen: drawn once, then left alone (see above). A press redraws it on purpose.
static void wait_for_cable(Next n)
{
    if (s_wait_magic != WAIT_MAGIC) { s_wait_magic = WAIT_MAGIC; s_wait_sig = 0; s_spurious = 0; }
    const uint32_t sig = ((uint32_t)n << 24) ^ (uint32_t)s_st.records;   // same state of the log, same screen
    const bool deep = esp_reset_reason() == ESP_RST_DEEPSLEEP;
    const bool pressed = woke_by(PIN_BTN);
    if (deep && woke_by(PIN_USB) && !usb_in()) {
        s_spurious++;
        ESP_LOGW(TAG, "woken by the USB pin with no cable (%u in a row)", (unsigned)s_spurious);
    } else if (pressed) {
        s_spurious = 0;
    }
    const bool redraw = !deep || pressed || sig != s_wait_sig;
    if (redraw) { draw(n); s_wait_sig = sig; }
    else ESP_LOGI(TAG, "waiting: the screen already shows this - not redrawn");
    sleep_for_cable_or_button(redraw);
}

// ---- the radio: the load, and the progress to the Mac -----------------------------------------

static void radio_up()
{
    WiFiManager &w = WiFiManager::getInstance();
    s_ap = w.startAP();
    std::string ssid, pass;
    NVSManager::getInstance().getString("ssid", ssid, "");
    NVSManager::getInstance().getString("pass", pass, "");
    s_sta = !ssid.empty() && w.connectSTAKeepAP(ssid, pass);
    // Receiver on all the time: that is the constant current the discharge is built on.
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (s_sta) {
        s_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int on = 1;
        if (s_udp >= 0) setsockopt(s_udp, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    }
    ESP_LOGW(TAG, "load on: access point %s, home network %s", s_ap ? "up" : "DOWN",
             s_sta ? "connected" : "not connected (no progress to the Mac)");
}

static void radio_down()
{
    if (s_udp >= 0) { close(s_udp); s_udp = -1; }
    WiFiManager::getInstance().shutdown();
    s_sta = s_ap = false;
}

// Directed broadcast on the home network: the listener does not need to know the device's address,
// and the device does not need to know the Mac's.
static void udp_line(const char *line)
{
    if (s_udp < 0) return;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0) return;
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(UDP_PORT);
    to.sin_addr.s_addr = ip.ip.addr | ~ip.netmask.addr;
    sendto(s_udp, line, strlen(line), 0, (sockaddr *)&to, sizeof(to));
}

static void udp_record(const CurveLog::Rec &r)
{
    char line[160];
    snprintf(line, sizeof(line), "CURVE,%u,%u,%lu,%lu,%.4f,%.4f,%.5f,%u,%u,%u", (unsigned)r.seq,
             (unsigned)r.kind, (unsigned long)r.t, (unsigned long)r.load_s, r.v_rest, r.v_load, r.gain,
             (unsigned)r.spread, (unsigned)r.flags, (unsigned)r.detail);
    udp_line(line);
}

// ---- a discharge ------------------------------------------------------------------------------

struct Dis {
    uint32_t t0 = 0;
    uint32_t load_s = 0;
    float    pending_load = 0;    // the loaded reading the next rested sample pairs with
    uint8_t  load_flags = 0;      // the radio state of the load phase before it
};

// Returns false if the cable went in.
static bool rest_phase()
{
    for (int s = 0; s < REST_S; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));   // the idle task takes the chip into light sleep
        if (usb_in()) return false;
    }
    return true;
}

// One load phase. `d` is null for a run-down, which is not logged.
static bool load_phase(Dis *d, float &v_load, int len_s, float v_rest_before)
{
    esp_pm_lock_acquire(s_awake);
    radio_up();
    if (s_have_last) udp_record(s_last);    // the sample was taken in the rest, without a radio
    const int64_t t_start = esp_timer_get_time();
    int64_t next_hb = t_start + 60LL * 1000000;
    bool pulsed = false, ok = true;
    const uint32_t load0 = d ? d->load_s : 0;
    while (esp_timer_get_time() - t_start < (int64_t)len_s * 1000000) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (usb_in()) { ok = false; break; }
        const int64_t el = esp_timer_get_time() - t_start;
        if (d && !pulsed && el >= (int64_t)PULSE_S * 1000000) {
            pulsed = true;
            float v = 0; uint8_t sp = 0;
            if (raw_cell(v, sp))
                put(CurveLog::KindPulse, rel(d->t0), load0 + (uint32_t)(el / 1000000), v_rest_before, v, sp,
                    temp_now(), (uint8_t)((s_sta ? CurveLog::FlagSta : 0) | (s_ap ? CurveLog::FlagAp : 0)));
        }
        if (d && esp_timer_get_time() >= next_hb) {
            next_hb += 60LL * 1000000;
            float v = 0; uint8_t sp = 0;
            if (raw_cell(v, sp)) {
                char line[96];
                snprintf(line, sizeof(line), "CURVE-HB,%lu,%lu,%.4f,%.5f", (unsigned long)rel(d->t0),
                         (unsigned long)(load0 + el / 1000000), v, BatterySource::calibration());
                udp_line(line);
            }
        }
    }
    const uint32_t ran = (uint32_t)((esp_timer_get_time() - t_start) / 1000000);
    uint8_t sp = 0;
    v_load = 0;
    if (ok) raw_cell(v_load, sp);            // at the end of the load, with the radio still on
    if (d) {
        d->load_s += ran;
        d->load_flags = (uint8_t)((s_sta ? CurveLog::FlagSta : 0) | (s_ap ? CurveLog::FlagAp : 0));
    }
    radio_down();
    esp_pm_lock_release(s_awake);
    return ok;
}

// Runs until the cutoff (then sleeps: the next boot charges) or the cable (returns: the caller
// voids it and recharges).
static void discharge_loop(Dis &d)
{
    for (;;) {
        if (!rest_phase()) break;
        float v = 0; uint8_t sp = 0;
        if (!raw_cell(v, sp)) ESP_LOGE(TAG, "rested reading failed");
        // The first rest follows the charger, not a load: there is no loaded reading to pair it with.
        put(CurveLog::KindSample, rel(d.t0), d.load_s, v, d.pending_load, sp, temp_now(), d.load_flags);
        const float g = BatterySource::calibration();
        if ((v > 2.0f && v * g <= end_v()) || (d.pending_load > 2.0f && d.pending_load * g <= LOAD_FLOOR_V)) {
            put(CurveLog::KindEnd, rel(d.t0), d.load_s, v, d.pending_load, sp, 0, 0);
            wait_for_cable(CurveSummary::next(s_st));
        }
        // Below 3.55 V the curve falls steeply, so the load between readings is cut to five minutes:
        // three times the points where the table needs them. The tool takes the spacing from load_s.
        const int len = (v > 2.0f && v * g < TAIL_V) ? LOAD_S_TAIL : LOAD_S;
        // The screen at every sample, before the radio: a refresh is two seconds at tens of mA,
        // a fraction of a per mille of the load phase it precedes.
        draw(Next::Discharging, 0, len / 60);
        float vl = 0;
        if (!load_phase(&d, vl, len, v)) break;
        d.pending_load = vl;
    }
    put(CurveLog::KindCharger, rel(d.t0), d.load_s, 0, 0, 0, 0, CurveLog::FlagUsb);
    ESP_LOGE(TAG, "cable in during D%u — charge went in, this discharge is void", (unsigned)s_st.cur_cycle);
}

static void start_discharge(uint8_t cycle)
{
    Dis d;
    d.t0 = now_s();
    put(CurveLog::KindHeader, d.t0, 0, 0, 0, cycle, CurveLog::VERSION, 0);
    ESP_LOGW(TAG, "D%u started", (unsigned)cycle);
    float v = 0; uint8_t sp = 0;
    raw_cell(v, sp);                       // the first rested reading is five minutes away: show this
    draw(Next::Discharging, v, LOAD_S / 60);
    discharge_loop(d);
}

static void resume_discharge()
{
    const CurveSummary::Discharge &x = s_st.discharge();
    Dis d;
    d.t0 = x.t0;
    d.load_s = x.load_s;
    put(CurveLog::KindReset, rel(d.t0), d.load_s, 0, 0, 0,
        (uint8_t)(esp_reset_reason() | (s_clock_fixed ? 0x80 : 0)), 0);
    ESP_LOGW(TAG, "D%u resumed after a reset", (unsigned)x.cycle);
    discharge_loop(d);
}

// Run the cell down until TP4056 will charge it again: above its restart point (4.05 V) it reports
// "finished" at once and charges nothing, and the bench cannot tell that from a cell that was full.
// Not logged — it is preparation, not measurement.
static void run_down()
{
    draw(Next::RunDown);
    const float g = BatterySource::calibration();
    for (;;) {
        if (!rest_phase()) return;                 // the cable went in: back to charging
        float v = 0; uint8_t sp = 0;
        if (raw_cell(v, sp) && v > 2.0f) s_rundown_rest = v;
        if (v > 2.0f && v * g <= RUN_DOWN_V) {
            char a[40];
            snprintf(a, sizeof(a), "%.3f V - PLUG IN NOW", v * g);
            draw(Next::RunDown, v, 15, a);
            sleep_for_cable_or_button();
        }
        float vl = 0;
        if (!load_phase(nullptr, vl, LOAD_S, v)) return;
    }
}

// ---- a charge ---------------------------------------------------------------------------------

static void erase_and_rearm();

// The cable is in and the charge (in s_st) is either running or finished. Logs and draws until the
// cable comes out. A finished charge keeps logging: the voltage relaxing on the idle charger is
// what the full cell really holds.
static void hm_str(char *out, size_t n, uint32_t s)
{
    snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
}

// Which board this is, as the last two bytes of its station MAC ("e004"). With two benches running
// side by side (two cells tested at once, 2026-10-03) a "measure R7 now" that does not say which
// board asks for it is a coin toss; the page link differs, but nobody reads an address to decide.
static const char *board_tag()
{
    static char tag[8] = "";
    if (!tag[0]) {
        uint8_t mac[6] = {};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) snprintf(tag, sizeof(tag), "%02x%02x", mac[4], mac[5]);
        else snprintf(tag, sizeof(tag), "????");
    }
    return tag;
}

// What the charge page shows, rebuilt by the loop each second (the page never reads s_st itself).
static void publish_page(bool charging, bool done, bool due, uint32_t t, uint32_t due_at, float v_now, bool tg)
{
    const CurveSummary::Charge &ch = s_st.charge();
    static char js[1600];
    int n = snprintf(js, sizeof(js),
                     "{\"board\":\"%s\",\"charging\":%d,\"done\":%d,\"cycle\":%u,\"t\":%lu,\"tdone\":%lu,\"due\":%d,\"n\":%d,"
                     "\"next\":%ld,\"vcell\":%.3f,\"mah\":%.1f,\"tg\":%d,\"r\":[",
                     board_tag(), charging ? 1 : 0, done ? 1 : 0, (unsigned)ch.cycle, (unsigned long)t, (unsigned long)ch.t_done,
                     due ? 1 : 0, ch.prog_n, (long)due_at - (long)t, v_now * BatterySource::calibration(),
                     ch.prog_covers() ? ch.prog_mah : 0.0f, tg ? 1 : 0);
    for (int i = 0; i < ch.prog_n && n > 0 && n < (int)sizeof(js) - 40; i++)
        n += snprintf(js + n, sizeof(js) - n, "%s[%lu,%.3f]", i ? "," : "", (unsigned long)ch.prog[i].t, ch.prog[i].a);
    if (n > 0 && n < (int)sizeof(js) - 3) snprintf(js + n, sizeof(js) - n, "]}");
    CurvePage::publish(js);
}

static void led(bool on) { gpio_set_level(PIN_LED, on ? 1 : 0); }

static void leave_charge()
{
    led(false);
    CurvePage::stop();
}

static void charge_loop()
{
    const uint32_t t0 = s_st.charge().t0;
    uint32_t charging_s = s_st.charge().charging_s;
    bool done = s_st.charge().done;
    bool idle_full = false;
    uint32_t t_prev = rel(t0);
    uint32_t next_sample = s_st.charge().samples ? t_prev + 5 : 10;
    // A fresh charge was drawn by start_charge(): the next frame after a minute shows the step at
    // plug-in. A resumed one draws at once.
    uint32_t next_draw = s_st.charge().samples ? 0 : t_prev + 60;
    uint32_t last_draw = s_st.charge().samples ? 0 : t_prev;
    float v_now = s_st.charge().v_last / BatterySource::calibration();
    // The page and its prompts: Wi-Fi for the charge (from USB, not from the cell).
    if (!CurvePage::up()) CurvePage::start();
    bool tg = CurvePage::telegram_configured();
    uint32_t announced = UINT32_MAX;          // the reading slot already announced
    bool blink = false, want_draw = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const uint32_t t = rel(t0);
        const uint32_t dt = t - t_prev;
        t_prev = t;
        if (!usb_in()) {
            if (!done) {
                put(CurveLog::KindUnplug, t, charging_s, v_now, 0, 0, idle_full ? 1 : 0, 0);
                ESP_LOGW(TAG, "cable out before the end of the charge%s", idle_full ? " (charger was idle)" : "");
            }
            leave_charge();
            return;
        }
        // The test is over and its log read: the button erases it here too, without unplugging.
        if (s_st.complete && button_down()) {
            int held = 0;
            while (button_down() && held < 3000) { vTaskDelay(pdMS_TO_TICKS(50)); held += 50; }
            if (held >= 3000) { leave_charge(); erase_and_rearm(); return; }
        }
        if (t % 10 == 0) { CurvePage::keep_alive(); tg = CurvePage::telegram_configured(); }

        // A reading typed on the page: into the log, with the moment it arrived.
        float r7 = 0;
        if (CurvePage::take_reading(r7) && !done && !idle_full) {
            uint8_t sp = 0;
            raw_cell(v_now, sp);
            put(CurveLog::KindProg, t, charging_s, v_now, r7, (uint8_t)(s_st.charge().prog_n + 1), 0,
                CurveLog::FlagUsb);
            want_draw = true;
        }
        const bool full = BatterySource::chargeComplete(true) == 1;
        bool redraw = false;
        if (!done) {
            if (!full) charging_s += dt;
            if (full && charging_s >= CHARGED_MIN_S) {
                done = true;
                uint8_t sp = 0;
                raw_cell(v_now, sp);
                const float g = BatterySource::calibration();
                put(CurveLog::KindChargeEnd, t, charging_s, v_now, s_st.charge().v_top / g, sp, temp_now(),
                    CurveLog::FlagUsb | CurveLog::FlagStdby);
                if (s_st.charge().cycle == 2 && !s_st.charge().recharge)
                    put(CurveLog::KindTestDone, t, 0, 0, 0, 0, 0, 0);
                next_sample = t + CHG_SAMPLE_S;
                redraw = true;
                if (tg) {
                    const CurveSummary::Charge &ch = s_st.charge();
                    char a[16], b[16], c[16], msg[600], q[128];
                    hm_str(a, sizeof(a), ch.t_done); hm_str(b, sizeof(b), ch.t_cv); hm_str(c, sizeof(c), ch.t_done - ch.t_cv);
                    if (ch.prog_covers()) snprintf(q, sizeof(q), "По замерам R7 зарядилось %.0f мА·ч.", ch.prog_mah);
                    else if (ch.prog_n >= 4 && ch.prog_gap() > CurveSummary::PROG_MAX_GAP_S) {
                        char g[16]; hm_str(g, sizeof(g), ch.prog_gap());
                        snprintf(q, sizeof(q), "Между замерами R7 дыра %s — мА·ч не считаю (можно не больше 0:30).", g);
                    }
                    else snprintf(q, sizeof(q), "Замеров R7 %d — мало, чтобы посчитать мА·ч.", ch.prog_n);
                    snprintf(msg, sizeof(msg), "Стенд АКБ %s: заряд C%u закончен\nЗа %s (постоянный ток %s, напряжение %s). %s\n%s",
                             board_tag(), (unsigned)ch.cycle, a, b, c, q,
                             s_st.complete ? "Тест окончен: подключи к Mac, сниму журнал."
                                           : "Вынимай кабель — начнётся разряд.");
                    CurvePage::telegram(msg);
                }
            } else if (full && !idle_full) {
                idle_full = true;            // "full" without having charged: see run_down()
                redraw = true;
            }
        }
        if (t >= next_sample) {
            uint8_t sp = 0;
            raw_cell(v_now, sp);
            put(CurveLog::KindChargeSample, t, charging_s, v_now, 0, sp, temp_now(),
                (uint8_t)(CurveLog::FlagUsb | (full ? CurveLog::FlagStdby : 0)));
            if (!done) next_sample = t < 60 ? 60 : t + CHG_SAMPLE_S;
            else {
                const uint32_t since = t - s_st.charge().t_done;
                next_sample = t + (since < 1800 ? CHG_SAMPLE_S : since < 6 * 3600 ? 600 : 3600);
            }
        }
        // Is a reading of the charge current due? Screen, LED, page and Telegram all say so.
        const uint32_t due_at = CurveSummary::prog_next_due(s_st.charge());
        const bool due = !done && !idle_full && t >= due_at;
        if (due && due_at != announced) {
            announced = due_at;
            want_draw = true;
            if (tg) {
                char el[16], msg[400];
                hm_str(el, sizeof(el), t);
                snprintf(msg, sizeof(msg), "Стенд АКБ %s: замерь R7 сейчас\nЗамер №%d · заряд C%u идёт %s · ячейка %.3f В\n%s%s",
                         board_tag(), s_st.charge().prog_n + 1, (unsigned)s_st.charge().cycle, el,
                         v_now * BatterySource::calibration(),
                         CurvePage::up() ? "Ввести: " : "Страницы нет — запиши вольты и время.",
                         CurvePage::up() ? CurvePage::url() : "");
                CurvePage::telegram(msg);
            }
        }
        blink = due ? !blink : false;
        led(blink);
        publish_page(true, done, due, t, due_at, v_now, tg);

        // The panel: every five minutes, at once on an event, but never twice within a minute
        // (Good Display wants three between refreshes; a minute is the compromise for prompts).
        if (redraw) want_draw = true;
        if ((want_draw && t >= last_draw + 60) || t >= next_draw) {
            char al[40];
            const char *alert = nullptr;
            if (!done && idle_full) alert = "FULL WITHOUT CHARGING - UNPLUG";
            else if (due) { snprintf(al, sizeof(al), "MEASURE R7 NOW - #%d", s_st.charge().prog_n + 1); alert = al; }
            draw(CurveSummary::next(s_st), v_now, 15, alert);
            next_draw = t + CHG_SCREEN_S;
            last_draw = t;
            want_draw = false;
        }
    }
}

static void start_charge(uint8_t cycle, bool recharge)
{
    // What the cell rested at before the cable: the last rested reading of the discharge just before,
    // or of the run-down. Without one (the start of a test) it is unknown, and says so.
    const float before = s_st.cur == CurveSummary::State::CurDischarge ? s_st.last_rest_raw : s_rundown_rest;
    float v0 = 0; uint8_t sp = 0;
    raw_cell(v0, sp);
    put(CurveLog::KindChargeStart, now_s(), 0, before, v0, cycle, temp_now(),
        (uint8_t)(CurveLog::FlagUsb | (recharge ? CurveLog::FlagRecharge : 0)));
    ESP_LOGW(TAG, "C%u%s started at %.4f V (rested before: %.4f)", (unsigned)cycle, recharge ? " (recharge)" : "",
             v0, before);
    draw(Next::Charging, v0);
    charge_loop();
}

static void resume_charge()
{
    put(CurveLog::KindReset, rel(s_st.charge().t0), s_st.charge().charging_s, 0, 0, 0,
        (uint8_t)(esp_reset_reason() | (s_clock_fixed ? 0x80 : 0)), 0);
    charge_loop();
}

// ---- the log as a whole -----------------------------------------------------------------------

static void erase_and_rearm()
{
    CurveLog::erase_all();
    s_st.reset();
    ESP_LOGW(TAG, "log erased by the button");
    // A new test starts on a fresh plug-in, never on the cable that is already there: the cell is
    // usually swapped between tests, and a test that started on the old one would be four days lost.
    if (usb_in()) {
        draw(Next::Empty, 0, 15, "ERASED - UNPLUG, SWAP, PLUG IN");
        while (usb_in()) vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// The log is kept until the button is held for three seconds: a finished test is never erased by
// accident. On the cable, the finished charge goes on logging its relaxation meanwhile.
static void hold_for_erase(Next n)
{
    draw(n);
    for (;;) {
        int held = 0;
        while (button_down() && held < 3000) { vTaskDelay(pdMS_TO_TICKS(50)); held += 50; }
        if (held >= 3000) { erase_and_rearm(); return; }
        if (!usb_in()) sleep_for_cable_or_button();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void curve_bench_run()
{
    ESP_LOGW(TAG, "battery bench");
    NVSManager::getInstance().init();
    WiFiManager::prepareApPassword();            // before any radio, see wifi_manager.h
    EPaper::releaseDeepSleepHold(true);
    I2CManager::getInstance().init();
    i2c_master_bus_handle_t bus = I2CManager::getInstance().getBusHandle();

    if (BatterySource::revision(bus) != BatterySource::RevDividerV2) {
        draw(Next::Empty, 0, 15, "WRONG BOARD - THIS IS FOR V2");
        sleep_for_cable_or_button();
    }
    // The CO2 sensor would sit in idle at a fraction of a milliamp for the whole run. Constant, so
    // harmless to the line — but off is cleaner for the rests.
    { SCD41 scd; if (scd.init(bus) && scd.wakeToIdle()) scd.powerDown(); }
    s_bme_ok = s_bme.init(bus);
    gpio_reset_pin(PIN_LED);
    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED, 0);

    esp_pm_config_t pm = {};
    pm.max_freq_mhz = 160;
    pm.min_freq_mhz = 40;
    pm.light_sleep_enable = true;
    esp_pm_configure(&pm);
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "curve_load", &s_awake);

    for (;;) {
        rescan();
        const bool usb = usb_in();
        if (usb && s_wait_magic == WAIT_MAGIC) s_spurious = 0;   // a real cable: the pin is honest again
        const Next n = CurveSummary::next(s_st);
        ESP_LOGW(TAG, "log: %s, %d records, cable %s", CurveSummary::name(n), s_st.records, usb ? "in" : "out");
        switch (n) {
        case Next::Empty:
            if (usb) {
                CurveLog::erase_all();           // empty already, but erased = all 0xFF, not merely invalid
                s_st.reset();
                put(CurveLog::KindTest, now_s(), 0, 0, 0, 0, CurveLog::VERSION, 0);
                start_charge(0, false);
                continue;
            }
            wait_for_cable(n);
            break;
        case Next::OldLog:
            hold_for_erase(n);
            break;
        case Next::Begin:
            if (usb) { start_charge(0, false); continue; }
            wait_for_cable(n);
            break;
        case Next::Charging:
            if (usb) { resume_charge(); continue; }
            put(CurveLog::KindUnplug, rel(s_st.charge().t0), s_st.charge().charging_s, 0, 0, 0, 0, 0);
            break;
        case Next::ChargeCut:
            if (usb) { start_charge(s_st.charge().cycle, s_st.charge().recharge); continue; }
            wait_for_cable(n);
            break;
        case Next::RunDown:
            if (usb) { start_charge(s_st.charge().cycle, s_st.charge().recharge); continue; }
            run_down();
            break;
        case Next::ChargedFull:
            if (usb) { resume_charge(); continue; }
            start_discharge((uint8_t)(s_st.charge().cycle + 1));
            break;
        case Next::Discharging:
            if (!usb) { resume_discharge(); continue; }
            put(CurveLog::KindCharger, rel(s_st.discharge().t0), s_st.discharge().load_s, 0, 0, 0, 0, CurveLog::FlagUsb);
            break;
        case Next::DischargeDone:
            if (usb) { start_charge(s_st.discharge().cycle, false); continue; }
            wait_for_cable(n);
            break;
        case Next::DischargeVoid:
            if (usb) { start_charge((uint8_t)(s_st.discharge().cycle - 1), true); continue; }
            wait_for_cable(n);
            break;
        case Next::Complete:
            if (usb && s_st.cur == CurveSummary::State::CurCharge) {
                // On the cable the relaxation goes on being logged (once an hour after six); the
                // button erases from inside the charge loop.
                resume_charge();
                continue;
            }
            hold_for_erase(n);
            break;
        }
    }
}
