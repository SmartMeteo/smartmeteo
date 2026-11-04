// ============================================================================
// Bring-up bench: does this board have every node, and does each one answer?
//
// Written for a board that has just come off the soldering iron, so it assumes nothing. It probes
// what is there, prints one line per node and ends with a table of verdicts. It never writes to a
// sensor's EEPROM: no factory reset, no forced recalibration, no persisted offset — a bench that
// can destroy a calibration is a bench nobody dares to run twice.
//
// It also refuses to guess the board revision. v1 carries a MAX17048 gauge on I2C; v2 drops it and
// measures the cell through a 1 MOhm / 390 kOhm divider on IO4 with TP4056's STDBY on IO5. Both are
// checked, and the summary says which shape the evidence fits.
//
// Every check answers a specific soldering question:
//   flash size        - the module is the N8 (8 MB) the BOM calls for, not an N4
//   RTC slow clock    - the 32 kHz crystal and its two capacitors actually oscillate
//   button pull-up    - R10 is on the board (read with the internal pull-up off, then fought
//                       against the internal pull-down: 10k wins over ~45k if R10 is really there)
//   I2C scan          - every sensor acknowledges its address, i.e. SDA/SCL/power reach it
//   BME690 pressure   - the raw ADC word, because THIS is the joint that was dead on the last
//                       board: the chip answered, temperature and humidity were sane, and the
//                       pressure channel returned 0xFFFFC for weeks
//   panel             - MOSI/SCK/CS/DC/RST/BUSY, the P-FET supply gate and the panel's own DC-DC
// ============================================================================
#include "board_bench.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_chip_info.h"
#include "esp_clk_tree.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "soc/rtc.h"

#include "epaper.h"
#include "i2c_manager.h"
#include "scd41.h"
#include "bme690.h"
#include "max17048.h"

static const char *TAG = "BOARD";

// The same pins the product uses (bench_screens.cpp, epaper.cpp). Repeated rather than included:
// this file must keep working if the device cycle is not even built.
#define PIN_USER_BTN    GPIO_NUM_2
#define PIN_USB_DETECT  GPIO_NUM_3    // low = cable in
#define PIN_LED         GPIO_NUM_11
#define PIN_BAT_ADC     GPIO_NUM_4    // v2 only: 1 MOhm / 390 kOhm divider from the cell
#define PIN_STDBY       GPIO_NUM_5    // v2 only: TP4056 STDBY#, open drain
#define BAT_DIV_RATIO   0.2806f       // 390k / (1M + 390k)

// ---- the verdict table -----------------------------------------------------------------------
// Three states, not two. "skip" is for a node this board does not claim to have (the v1 gauge on a
// v2 board), and it must not read as a failure — the whole point of the run is to tell a missing
// part from a broken one.
enum class V { Pass, Fail, Skip };

struct Check {
    const char *name;
    V           verdict;
    char        detail[96];
};

static Check s_checks[24];
static int   s_n = 0;

static void note(const char *name, V v, const char *fmt, ...)
{
    if (s_n >= (int)(sizeof(s_checks) / sizeof(s_checks[0]))) return;
    Check &c = s_checks[s_n++];
    c.name = name;
    c.verdict = v;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c.detail, sizeof(c.detail), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%-22s %s  %s", name,
             v == V::Pass ? "OK  " : v == V::Fail ? "FAIL" : "skip", c.detail);
}

static void led(bool on) { gpio_set_level(PIN_LED, on ? 1 : 0); }

static void wait_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) + 1); }

// ---- 1. the module itself --------------------------------------------------------------------
static void check_chip(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash = 0;
    const bool flash_ok = esp_flash_get_size(nullptr, &flash) == ESP_OK;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGW(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    note("chip", V::Pass, "ESP32-C6 rev%d.%d, %d core",
         info.revision / 100, info.revision % 100, info.cores);

    // The BOM in the repository still says N4. If a board ever comes back with 4 MB, the partition
    // table does not fit and nothing else in this run matters.
    if (!flash_ok)
        note("flash", V::Fail, "size could not be read");
    else
        note("flash", flash >= 8u * 1024 * 1024 ? V::Pass : V::Fail, "%u MB (need 8 MB, module N8)",
             (unsigned)(flash / (1024 * 1024)));
}

// ---- 2. the 32 kHz crystal -------------------------------------------------------------------
static void check_slow_clock(void)
{
    const soc_rtc_slow_clk_src_t src = rtc_clk_slow_src_get();
    uint32_t hz = 0;
    // CACHED: the exact form re-runs the slow-clock calibration, which costs real time and
    // answers a question this bench is not asking (bench_screens.cpp does the same).
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_RTC_SLOW, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &hz);
    // The fallback is silent in normal operation: the device keeps time, just less accurately. On a
    // new board it is the one hint that Y1 or C9/C10 are cold joints.
    note("32 kHz crystal", src == SOC_RTC_SLOW_CLK_SRC_XTAL32K ? V::Pass : V::Fail,
         "%s, %u Hz",
         src == SOC_RTC_SLOW_CLK_SRC_XTAL32K ? "XTAL32K running"
       : src == SOC_RTC_SLOW_CLK_SRC_RC32K   ? "RC32K - crystal NOT running"
                                             : "internal RC (150 kHz)",
         (unsigned)hz);
}

// ---- 3. the two signal pins, and R10 ----------------------------------------------------------
static void check_static_pins(void)
{
    gpio_config_t in = {};
    in.mode = GPIO_MODE_INPUT;
    in.pin_bit_mask = (1ULL << PIN_USB_DETECT);
    in.pull_up_en = GPIO_PULLUP_DISABLE;
    in.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&in);
    const int usb = gpio_get_level(PIN_USB_DETECT);
    // This bench is run over USB, so the cable is in by definition: a HIGH here is the divider or
    // the joint, not the state of the world.
    note("USB detect (IO3)", usb == 0 ? V::Pass : V::Fail, "%s (cable is plugged in now)",
         usb == 0 ? "LOW = cable seen" : "HIGH = cable NOT seen");

    // R10 10k to 3V3. Floating input first: it must read HIGH on its own.
    in.pin_bit_mask = (1ULL << PIN_USER_BTN);
    gpio_config(&in);
    wait_ms(5);
    const int idle = gpio_get_level(PIN_USER_BTN);

    // Then make the chip pull the other way. The internal pull-down is ~45k, so an external 10k
    // still holds the line HIGH; with no R10 the pin follows the pull-down to 0.
    in.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&in);
    wait_ms(5);
    const int fought = gpio_get_level(PIN_USER_BTN);
    in.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&in);

    if (idle == 1 && fought == 1)      note("button pull-up R10", V::Pass, "HIGH against the internal pull-down");
    else if (idle == 1)                note("button pull-up R10", V::Fail, "HIGH floating, but lost to the internal pull-down");
    else                               note("button pull-up R10", V::Fail, "line already LOW - button stuck or R10 missing");
}

// ---- 4. I2C ------------------------------------------------------------------------------------
struct Bus {
    i2c_master_bus_handle_t handle = nullptr;
    bool scd = false, bme = false, gauge = false;
};

static Bus check_i2c(void)
{
    Bus bus;
    // Read SDA before the driver owns the pin: a device holding the line down is a different fault
    // from an empty bus, and after init() the question can no longer be asked.
    if (I2CManager::getInstance().busIsStuck())
        note("I2C bus", V::Fail, "SDA is held low before init - a device is hung");

    I2CManager::getInstance().init();
    bus.handle = I2CManager::getInstance().getBusHandle();
    if (!bus.handle) {
        note("I2C bus", V::Fail, "the bus would not initialise");
        return bus;
    }

    char found[80] = "";
    int  n = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(bus.handle, a, 40) != ESP_OK) continue;
        n++;
        char one[8];
        snprintf(one, sizeof(one), "0x%02x ", a);
        strncat(found, one, sizeof(found) - strlen(found) - 1);
        if (a == 0x62) bus.scd = true;
        if (a == 0x76) bus.bme = true;
        if (a == 0x36) bus.gauge = true;
    }
    note("I2C scan", n > 0 ? V::Pass : V::Fail, "%d device(s): %s", n, n ? found : "nothing answered");
    return bus;
}

// ---- 5. SCD41 ----------------------------------------------------------------------------------
static void check_scd41(Bus &bus)
{
    if (!bus.handle) { note("SCD41", V::Skip, "no bus"); return; }

    SCD41 scd;
    if (!scd.init(bus.handle) || !scd.wakeToIdle()) {
        note("SCD41", V::Fail, "no answer at 0x62");
        return;
    }
    uint64_t serial = 0;
    note("SCD41", scd.getSerial(serial) ? V::Pass : V::Fail, "serial %012llx", (unsigned long long)serial);

    // Sensirion's end-of-line test (§3.10.3): 0 means the sensor's own checks pass. It takes ten
    // seconds and touches no stored calibration, unlike the factory reset this bench never sends.
    uint16_t st = 0xFFFF;
    if (scd.selfTest(st)) note("SCD41 self-test", st == 0 ? V::Pass : V::Fail, "result 0x%04x (0 = healthy)", st);
    else                  note("SCD41 self-test", V::Fail, "no answer");

    float offset = -1.0f;
    if (scd.getTemperatureOffset(offset)) note("SCD41 offset", V::Pass, "%.2f C in the sensor's EEPROM", offset);

    uint16_t co2 = 0; float t = 0, rh = 0;
    if (scd.measureSingleShot()) {
        wait_ms(SCD41::SINGLE_SHOT_MS + 100);
        if (scd.readMeasurement(co2, t, rh)) {
            // Indoor air is 400-2000 ppm. A number outside that is not automatically a fault, but on
            // a bench next to a person it is worth the flag.
            const bool sane = co2 >= 300 && co2 <= 5000 && t > 0 && t < 50 && rh > 5 && rh < 95;
            note("SCD41 reading", sane ? V::Pass : V::Fail, "%u ppm, %.1f C, %.0f %%", co2, t, rh);
        } else note("SCD41 reading", V::Fail, "single shot returned nothing");
    } else note("SCD41 reading", V::Fail, "single shot refused");
    scd.powerDown();
}

// ---- 6. BME690, and the joint that failed last time -------------------------------------------
static void check_bme690(Bus &bus)
{
    if (!bus.handle) { note("BME690", V::Skip, "no bus"); return; }

    BME690 bme;
    if (!bme.init(bus.handle)) {
        note("BME690", V::Fail, "no answer at 0x76 (chip id not 0x61)");
        return;
    }
    note("BME690", V::Pass, "chip id read, OTP calibration loaded");

    // A forced measurement with a modest heater step: enough to prove the gas plate draws current
    // and the ADC converts, without the 300 s BSEC schedule.
    float t = 0, rh = 0, p = 0, gas = 0;
    bool gas_valid = false;
    if (!bme.readDataWithHeater(300, 100, 2, 5, 1, true, t, rh, p, gas, gas_valid)) {
        note("BME690 reading", V::Fail, "forced measurement failed");
        return;
    }
    note("BME690 T/RH", (t > 0 && t < 60 && rh > 5 && rh < 95) ? V::Pass : V::Fail, "%.1f C, %.0f %%", t, rh);

    // THE check of this bench. 300-1100 hPa is the sensor's range; the previous board sat at the
    // full-scale word with perfectly good temperature beside it, which is what an unsoldered
    // pressure pad looks like from software.
    const float hpa = p / 100.0f;
    note("BME690 pressure", (hpa > 300 && hpa < 1100) ? V::Pass : V::Fail,
         "%.1f hPa%s", hpa, (hpa > 300 && hpa < 1100) ? "" : " - suspect the solder joint");

    note("BME690 gas", gas_valid ? V::Pass : V::Fail, "%.0f Ohm, valid=%d", gas, (int)gas_valid);

    // Raw coefficients and the raw pressure word, so a bad reading can be read as a story rather
    // than a verdict: garbage par_p* means the OTP read failed, a stuck word means the channel.
    bme.logDiag();
}

// ---- 7. power: the v1 gauge, or the v2 divider -------------------------------------------------
static bool check_gauge(Bus &bus)
{
    if (!bus.handle || !bus.gauge) {
        note("MAX17048 (v1)", V::Skip, "nothing at 0x36 - expected on a v2 board");
        return false;
    }
    MAX17048 g;
    if (!g.init(bus.handle)) { note("MAX17048 (v1)", V::Fail, "address answered but init failed"); return false; }

    uint16_t ver = 0; float v = 0, soc = 0, crate = 0; uint8_t flags = 0;
    g.getVersion(ver);
    const bool ok = g.getVCELL(v) && g.getSOC(soc);
    g.getCRATE(crate);
    g.getStatusFlags(flags);
    note("MAX17048 (v1)", ok ? V::Pass : V::Fail, "IC 0x%04x, %.3f V, %.0f %%, %+.2f %%/h, status 0x%02x",
         ver, v, soc, crate, flags);
    // 3.0-4.3 V is a cell; anything else is either no battery or a wiring fault.
    if (ok) note("cell voltage", (v > 2.5f && v < 4.4f) ? V::Pass : V::Fail,
                 "%.3f V%s", v, (v > 2.5f && v < 4.4f) ? "" : " - no cell attached?");
    return true;
}

static void check_divider(bool gauge_present)
{
    // v1 leaves IO4 unconnected, so a reading there means nothing and a "fail" would be a lie.
    // The sweep still runs — a floating pin reads as noise, which is itself recognisable.
    adc_oneshot_unit_handle_t adc = nullptr;
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK) { note("battery ADC (v2)", V::Fail, "ADC1 would not start"); return; }

    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten = ADC_ATTEN_DB_12;         // full span for diagnosis; the product will use 2.5 dB
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(adc, ADC_CHANNEL_4, &ccfg);   // IO4 = ADC1 channel 4 on the C6

    adc_cali_handle_t cali = nullptr;
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id = ADC_UNIT_1;
    cal.chan = ADC_CHANNEL_4;
    cal.atten = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    const bool have_cali = adc_cali_create_scheme_curve_fitting(&cal, &cali) == ESP_OK;

    int sum = 0, n = 0;
    for (int i = 0; i < 16; i++) {
        int raw = 0;
        if (adc_oneshot_read(adc, ADC_CHANNEL_4, &raw) == ESP_OK) { sum += raw; n++; }
    }
    int mv = 0;
    if (n && have_cali) adc_cali_raw_to_voltage(cali, sum / n, &mv);

    const float cell = mv / BAT_DIV_RATIO / 1000.0f;
    const bool plausible = cell > 2.5f && cell < 4.5f;
    // On a v2 board the gauge is gone, so this path is the only battery reading and has to work.
    // On a v1 board it is a curiosity, and the verdict says so.
    note("battery ADC (v2)", gauge_present ? V::Skip : (plausible ? V::Pass : V::Fail),
         "IO4 raw %d, %d mV -> cell %.2f V%s", n ? sum / n : -1, mv, cell,
         gauge_present ? " (v1 board: IO4 not connected)" : "");

    if (cali) adc_cali_delete_scheme_curve_fitting(cali);
    adc_oneshot_del_unit(adc);
}

static void check_stdby(bool gauge_present)
{
    // TP4056 STDBY# is open drain: LOW = charge finished, HIGH (through the pull-up) = charging.
    // The internal pull-up is only enabled while the cable is in, exactly as the product must do it,
    // so this pin never feeds a dead charger.
    gpio_config_t in = {};
    in.mode = GPIO_MODE_INPUT;
    in.pin_bit_mask = (1ULL << PIN_STDBY);
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in);
    wait_ms(5);
    const int lvl = gpio_get_level(PIN_STDBY);
    in.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&in);
    note("TP4056 STDBY (v2)", gauge_present ? V::Skip : V::Pass, "IO5 reads %s%s",
         lvl ? "HIGH = charging (or no cell)" : "LOW = charge complete",
         gauge_present ? " (v1 board: IO5 not connected)" : "");
}

// ---- 8. the panel ------------------------------------------------------------------------------
static void check_panel(void)
{
    EPaper epd;
    EPaper::releaseDeepSleepHold(true);
    if (!epd.init()) { note("e-ink panel", V::Fail, "SPI would not start"); return; }

    epd.clear(0xFF);
    epd.drawString(20, 20, "SMART METEO - BOARD TEST", 0, 2);
    epd.drawString(20, 60, "If you can read this, the panel,", 0, 1);
    epd.drawString(20, 76, "its supply gate and all six lines", 0, 1);
    epd.drawString(20, 92, "are soldered correctly.", 0, 1);
    // Corner marks and a frame: a partially connected data line shows up as bands or a shifted
    // image, which text alone can hide.
    epd.drawRect(4, 4, EPD_WIDTH - 8, EPD_HEIGHT - 8, 0);
    for (int i = 0; i < 4; i++) epd.fillRect(4 + (i % 2) * (EPD_WIDTH - 44), 4 + (i / 2) * (EPD_HEIGHT - 44), 40, 40, 0);
    for (int g = 0; g <= 4; g++) epd.fillRectDither(60 + g * 56, 150, 50, 100, g, 0);
    epd.drawString(60, 262, "black  25%  50%  75%  solid", 0, 1);

    const int64_t t0 = esp_timer_get_time();
    epd.refresh(EPaper::Update::Full);
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);
    // sessionOk() is false when SPI errored or BUSY never moved — the two ways the panel can be
    // absent while the code carries on regardless.
    note("e-ink panel", epd.sessionOk() ? V::Pass : V::Fail, "full refresh %d ms, session %s",
         ms, epd.sessionOk() ? "clean" : "BUSY stuck or SPI error");
    epd.hibernate();
}

// ---- 9. the button, by hand --------------------------------------------------------------------
static void check_button_press(void)
{
    ESP_LOGW(TAG, "press the button now (20 s) - the LED follows the contact");
    const int64_t deadline = esp_timer_get_time() + 20LL * 1000000;
    int64_t down_at = 0;
    int     edges = 0;
    int     last = 1;
    int64_t held_ms = 0;

    while (esp_timer_get_time() < deadline) {
        const int lvl = gpio_get_level(PIN_USER_BTN);
        led(lvl == 0);
        if (lvl != last) {
            edges++;
            if (lvl == 0) down_at = esp_timer_get_time();
            else if (down_at) { held_ms = (esp_timer_get_time() - down_at) / 1000; break; }
            last = lvl;
        }
        wait_ms(1);
    }
    led(false);
    // Bounce is counted, not judged: the product debounces in software, and the number is here so a
    // suspiciously noisy contact can be recognised rather than discovered months later.
    if (held_ms > 0) note("button press", V::Pass, "held %lld ms, %d edge(s) seen", (long long)held_ms, edges);
    else             note("button press", V::Fail, "nothing pressed within 20 s");
}

// ---- the run -----------------------------------------------------------------------------------
void board_bench_run()
{
    gpio_config_t out = {};
    out.mode = GPIO_MODE_OUTPUT;
    out.pin_bit_mask = (1ULL << PIN_LED);
    gpio_config(&out);
    for (int i = 0; i < 6; i++) { led(i % 2 == 0); wait_ms(120); }   // the LED is its own test
    note("LED (IO11)", V::Pass, "blinked six times - confirm by eye");

    ESP_LOGW(TAG, "================ BOARD BRING-UP ================");
    check_chip();
    check_slow_clock();
    check_static_pins();
    Bus bus = check_i2c();
    check_scd41(bus);
    check_bme690(bus);
    const bool gauge = check_gauge(bus);
    check_divider(gauge);
    check_stdby(gauge);
    check_panel();
    check_button_press();

    int pass = 0, fail = 0, skip = 0;
    for (int i = 0; i < s_n; i++) {
        if (s_checks[i].verdict == V::Pass) pass++;
        else if (s_checks[i].verdict == V::Fail) fail++;
        else skip++;
    }

    while (true) {
        ESP_LOGW(TAG, "================ RESULT: %d ok, %d failed, %d skipped ================", pass, fail, skip);
        ESP_LOGW(TAG, "board looks like %s", bus.gauge ? "v1 (MAX17048 on the bus)" : "v2 (no gauge; cell via IO4)");
        for (int i = 0; i < s_n; i++)
            ESP_LOGW(TAG, "  %-22s %s  %s", s_checks[i].name,
                     s_checks[i].verdict == V::Pass ? "OK  " : s_checks[i].verdict == V::Fail ? "FAIL" : "skip",
                     s_checks[i].detail);
        // The report repeats every 30 s: a serial console opened after the run still gets the whole
        // table, and on a board where the port comes and goes that is the difference between a
        // result and another flash cycle.
        for (int i = 0; i < 30; i++) { led(fail && (i % 2 == 0)); wait_ms(1000); }
    }
}
