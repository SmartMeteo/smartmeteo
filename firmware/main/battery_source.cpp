#include "battery_source.h"
#include "battery_spec.h"
#include "max17048.h"
#include "nvs_manager.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "BATSRC";

#define PIN_BAT_ADC   GPIO_NUM_4          // v2: divider from the cell; ADC1 channel 4 on the C6
#define PIN_STDBY     GPIO_NUM_5          // v2: TP4056 STDBY#, open drain, no external pull-up

namespace BatterySource {

// Only ever holds a DEFINITE answer; see the header. The guarantee is structural rather than
// remembered by hand: RevUnknown is BOTH "not asked yet" and "could not tell", so there is no way
// to express "remember that I could not tell" — the next call simply asks again. Splitting the two
// apart (a separate `bool cached`) would quietly remove that, and a device whose bus was down for
// one probe would be locked out of its own cell for the rest of the boot.
static Rev s_rev_cached = RevUnknown;

Rev revision(i2c_master_bus_handle_t bus)
{
    if (s_rev_cached != RevUnknown) return s_rev_cached;
    if (!bus) return RevUnknown;
    // Said out loud, once per boot. The answer decides where the cell is read from and therefore
    // what the lockout compares against; a decision that important should not have to be inferred
    // from the absence of an error line somewhere else in the log.
    if (i2c_master_probe(bus, 0x36, 50) == ESP_OK) {
        ESP_LOGW(TAG, "board v1: fuel gauge answered at 0x36");
        return s_rev_cached = RevGaugeV1;
    }

    // The gauge did not answer. That is what a v2 board looks like — and ALSO what a v1 board
    // with a dead gauge looks like, and confusing the two is not harmless: on v1 nothing is
    // connected to IO4, so reading the divider there measures a floating pin and hands back a
    // plausible number instead of an honest "cannot tell". A low one would lock a working device
    // out of its own cell.
    //
    // A second opinion settles it. If anything else on the bus still answers, the bus is healthy
    // and the gauge is genuinely absent — a v2 board. If nobody answers, the bus itself is sick
    // and this function has no business guessing which board it is on.
    if (i2c_master_probe(bus, 0x62, 50) == ESP_OK || i2c_master_probe(bus, 0x76, 50) == ESP_OK) {
        ESP_LOGW(TAG, "board v2: no gauge, cell on the divider (gain %.4f)", calibration());
        return s_rev_cached = RevDividerV2;
    }
    ESP_LOGE(TAG, "the I2C bus is silent — cannot tell which board this is, so the cell is not read");
    return RevUnknown;                   // deliberately NOT remembered
}

// ---- v2: the divider ---------------------------------------------------------------------------
//
// How one reading is taken, and what each step is for. Figures are from the ESP32-C6 datasheet v1.5
// §5.5 and the ESP-IDF v5.5.2 sources unless said otherwise.
//
//  - ONE SESSION AT A TIME. The console's calibration runs in the web server's task while the window
//    loop refreshes the cell every five seconds from the cycle task, and each used to open ADC1 on
//    its own. The driver gives a unit to one owner — adc_oneshot_new_unit() answers the second with
//    ESP_ERR_NOT_FOUND — so a calibration that collided with the refresh was refused as "unreadable"
//    for no reason the person holding the meter could see.
//
//  - OPENED AND CLOSED AROUND EACH BURST, not held for the boot. Opening the unit takes the SAR's
//    power and, on the C6, keeps the modem power domain up through light sleep until the unit is
//    deleted (ADC_LL_ADC_FE_ON_MODEM_DOMAIN in adc_oneshot.c). Matter mode lives in light sleep and
//    would pay for both between every tick.
//
//  - THE FIRST CONVERSIONS ARE THROWN AWAY. Configuring the channel is what turns the pad from a
//    digital input into an analogue one (gpio_config_as_analog), so the first conversions are the
//    ones taken nearest to a change. They cost microseconds; not trusting them costs nothing.
//
//  - SIXTY-FOUR KEPT, SORTED, AN EIGHTH DROPPED FROM EACH END, THE MIDDLE AVERAGED. The datasheet
//    gives DNL of -8..+12 LSB and itself advises sampling several times and filtering. A trimmed
//    mean keeps what averaging does for noise and adds what a plain mean cannot do: a conversion
//    caught by a Wi-Fi burst on the rail while the console is open lands in a tail and is gone,
//    instead of dragging the answer.
//
//  - THE FRACTIONAL MEAN IS CONVERTED THROUGH A LINE, not rounded to a code. adc_cali_raw_to_voltage()
//    answers in whole millivolts, and one millivolt at the node is 3.6 mV at the cell — a whole
//    percent in the flat middle of the discharge curve, and a staircase in every slope computed from
//    it. A least-squares line through nine points of the calibration curve around the mean,
//    evaluated at the mean, takes the rounding out: the curve is smooth over 40 mV, the rounding is
//    not.
//
// No settling wait is needed for the node itself. C10 100 nF holds it, and at a 280 kOhm source the
// SAR's sampling charge moves it by microvolts; the datasheet's own figures are taken with the same
// 100 nF on the pin. What the node DOES filter is the cell: its 28 ms time constant averages a load
// pulse rather than catching it at an instant.
static constexpr int   BURST_DISCARD = 4;
static constexpr int   BURST_KEEP    = 64;
static constexpr int   ADC_FULL      = 4095;       // 12-bit: a code this high is the rail, not a reading

// Near the cutoff one reading is not enough. The lockout it feeds is sticky — it lifts only at the
// cutoff plus 0.15 V, which in practice means a charger — so a single low reading would stop a
// working device until somebody plugged it in. A load pulse on the cell (the CO2 sensor's lamp, the
// panel's charge pump) is real voltage, but it is not the cell's charge. Below this line a second
// burst is taken after the node has re-settled several times over, and the higher of the two wins:
// a cell that is really flat reads low twice.
static constexpr float SECOND_LOOK_V  = 0.05f;
static constexpr int   SECOND_LOOK_MS = 150;       // > 5 x 28 ms, the node's time constant

static BurstStats s_last = {};

static SemaphoreHandle_t adc_mutex()
{
    // A function-local static: initialisation is thread-safe in ESP-IDF's C++ runtime, so the first
    // two callers cannot both create one.
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

// The mean code, converted through a line fitted to the calibration curve around it. See above.
static bool code_to_mv(adc_cali_handle_t cali, float code, float &mv)
{
    const int c0 = (int)lroundf(code);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int k = 0;
    for (int d = -64; d <= 64; d += 16) {
        const int c = c0 + d;
        if (c < 0 || c > ADC_FULL) continue;
        int v = 0;
        if (adc_cali_raw_to_voltage(cali, c, &v) != ESP_OK) return false;
        sx += c; sy += v; sxx += (double)c * c; sxy += (double)c * v; k++;
    }
    const double den = k * sxx - sx * sx;
    if (k < 2 || den <= 0.0) return false;
    const double b = (k * sxy - sx * sy) / den;
    const double a = (sy - b * sx) / k;
    mv = (float)(a + b * code);
    return true;
}

// One burst, raw: the cell as the bare divider reports it, no gain. Takes the ADC lock itself.
static bool burst_cell(float &volts)
{
    if (xSemaphoreTake(adc_mutex(), pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "ADC busy for a second — cell not read");
        return false;
    }

    adc_oneshot_unit_handle_t adc = nullptr;
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK) {
        xSemaphoreGive(adc_mutex());
        ESP_LOGE(TAG, "ADC1 would not start");
        return false;
    }

    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    ccfg.atten    = ADC_ATTEN_DB_2_5;          // 0..1300 mV; see DIV_MAX_CELL_V in the header
    adc_oneshot_config_channel(adc, ADC_CHANNEL_4, &ccfg);

    adc_cali_handle_t cali = nullptr;
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id  = ADC_UNIT_1;
    cal.chan     = ADC_CHANNEL_4;
    cal.atten    = ADC_ATTEN_DB_2_5;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    const bool have_cali = adc_cali_create_scheme_curve_fitting(&cal, &cali) == ESP_OK;

    int raw = 0;
    for (int i = 0; i < BURST_DISCARD; i++) adc_oneshot_read(adc, ADC_CHANNEL_4, &raw);

    // A failed conversion is retried rather than counted, within a bound.
    int s[BURST_KEEP];
    int n = 0;
    for (int i = 0; i < 2 * BURST_KEEP && n < BURST_KEEP; i++)
        if (adc_oneshot_read(adc, ADC_CHANNEL_4, &raw) == ESP_OK) s[n++] = raw;

    BurstStats st = {};
    float node_mv = 0.0f;
    bool ok = have_cali && n >= BURST_KEEP / 2;
    if (ok) {
        for (int i = 1; i < n; i++) {              // insertion sort: 64 values, already nearly sorted
            const int x = s[i];
            int j = i - 1;
            while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; }
            s[j + 1] = x;
        }
        const int trim = n / 8;
        long sum = 0;
        for (int i = trim; i < n - trim; i++) sum += s[i];
        st.raw_mean   = (float)sum / (float)(n - 2 * trim);
        st.spread_lsb = s[n - trim - 1] - s[trim];
        st.samples    = n;
        if (s[n - trim - 1] >= ADC_FULL) {
            // The node is above what this attenuation can see. Nothing on a healthy board puts it
            // there (4.26 V at the cell is 1.20 V at the node), so it is a fault, and a clipped
            // number would be a plausible lie.
            ESP_LOGE(TAG, "divider node at the top of the converter's range — cell not read");
            ok = false;
        } else {
            ok = code_to_mv(cali, st.raw_mean, node_mv);
        }
    }

    if (cali) adc_cali_delete_scheme_curve_fitting(cali);
    adc_oneshot_del_unit(adc);

    if (ok) {
        st.node_mv = node_mv;
        st.valid   = true;
        s_last     = st;
    }
    xSemaphoreGive(adc_mutex());

    if (!ok) {
        // Refusing beats guessing. An uncalibrated reading would feed the only software guard this
        // cell has, and callers already know how to handle "cannot tell" — they keep the cached
        // voltage and release the lockout if a charger is attached.
        if (!have_cali || n < BURST_KEEP / 2)
            ESP_LOGE(TAG, "cell unreadable: %d samples, calibration %s", n, have_cali ? "ok" : "missing");
        return false;
    }
    volts = node_mv / 1000.0f / DIV_RATIO;
    ESP_LOGI(TAG, "divider: code %.1f (spread %d LSB over %d) -> node %.2f mV -> cell %.4f V raw",
             st.raw_mean, st.spread_lsb, st.samples, node_mv, volts);
    return true;
}

static bool read_divider(float &volts, bool apply_cal)
{
    float v = 0.0f;
    if (!burst_cell(v)) return false;
    if (!apply_cal) { volts = v; return true; }

    const float gain = calibration();
    v *= gain;
    if (v < BATTERY_CUTOFF_V + SECOND_LOOK_V) {
        vTaskDelay(pdMS_TO_TICKS(SECOND_LOOK_MS));
        float v2 = 0.0f;
        if (burst_cell(v2)) {
            v2 *= gain;
            ESP_LOGW(TAG, "cell %.3f V is near the cutoff — second look %.3f V, keeping %.3f V",
                     v, v2, v2 > v ? v2 : v);
            if (v2 > v) v = v2;
        }
    }
    volts = v;
    return true;
}

BurstStats lastBurst()
{
    BurstStats st = {};
    if (xSemaphoreTake(adc_mutex(), pdMS_TO_TICKS(100)) == pdTRUE) {
        st = s_last;
        xSemaphoreGive(adc_mutex());
    }
    return st;
}

// ---- the gain ----------------------------------------------------------------------------------

// Cached for the boot. Every wake is a fresh boot in normal running, so this is not a stale-value
// hazard; the one place the number can change while the firmware is up is calibrate(), which is
// reached from the console window and sets this itself. Zero means "not read yet".
static float s_cal_cached = 0.0f;

float calibration()
{
    if (s_cal_cached != 0.0f) return s_cal_cached;

    float f = CAL_NONE;
    if (!NVSManager::getInstance().getFloat("bat_cal", f, CAL_NONE)) f = CAL_NONE;
    if (!isfinite(f) || f < CAL_MIN || f > CAL_MAX) {
        // Out of range means somebody wrote it by hand or the key is corrupt. Refusing to apply it
        // leaves the device merely uncalibrated, which is where it started; honouring it could move
        // the cutoff onto a full cell.
        if (f != CAL_NONE) ESP_LOGE(TAG, "stored cell gain %.4f is outside %.2f..%.2f — ignored",
                                    f, CAL_MIN, CAL_MAX);
        f = CAL_NONE;
    }
    s_cal_cached = f;
    return s_cal_cached;
}

void forgetBoard()
{
    s_cal_cached = 0.0f;
    s_rev_cached = RevUnknown;
}

// ---- the common face ---------------------------------------------------------------------------

bool readVoltage(i2c_master_bus_handle_t bus, float &volts)
{
    switch (revision(bus)) {
    case RevGaugeV1: {
        MAX17048 g;
        float v = 0.0f;
        if (!g.init(bus) || !g.getVCELL(v) || v < 2.0f) return false;
        volts = v;
        return true;
    }
    case RevDividerV2:
        return read_divider(volts, true);
    default:
        // Which board this is could not be established, so there is no right pin to read.
        // Callers treat false as "keep what you decided before", which is the safe answer.
        return false;
    }
}

bool readRawVoltage(i2c_master_bus_handle_t bus, float &volts)
{
    if (revision(bus) != RevDividerV2) return false;
    return read_divider(volts, false);
}

// A stored gain is worth more care than a reading: it moves every reading after it, for the life of
// the board. So it is the MEAN of 32 bursts across about a second and a half, and it is refused only
// if the cell is DRIFTING — not if it is noisy.
//
// The difference is the whole point, and the first version got it wrong. Calibration happens with
// the console open, which means on battery with the radio up, and every Wi-Fi transmission sags the
// cell for a moment. A burst lasts ~3 ms, so it catches the node at some point of that pattern:
// burst to burst the readings scatter by tens of millivolts. The first version refused when any two
// of eight bursts were 20 mV apart, and it refused on the bench on 2026-09-30 every time — the
// scatter of the radio, read as "the voltage is moving". A meter averages that same scatter over its
// own gate time, so the mean is the right thing to compare with it, and the scatter averages out.
//
// Drift is something else: a charger still in, a cell relaxing just after it came off one. That
// moves the MEAN, so the two halves of the run are compared. It is refused when the halves are both
// further apart than CAL_DRIFT_V and further apart than four standard errors — so a noisy run needs
// a correspondingly clearer drift before it is called one, and a quiet run cannot hide one.
static constexpr int   CAL_BURSTS   = 32;
static constexpr int   CAL_GAP_MS   = 40;
static constexpr float CAL_DRIFT_V  = 0.010f;   // between the means of the two halves, at the cell

bool calibrate(i2c_master_bus_handle_t bus, float reference_v, float &factor, float &raw,
               CalError *why, bool on_usb, CalStats *stats)
{
    CalError dummy;
    CalError &err = why ? *why : dummy;
    err = CalOk;
    CalStats st_dummy;
    CalStats &st = stats ? *stats : st_dummy;
    st = CalStats{};

    if (!isfinite(reference_v) || reference_v < 3.0f || reference_v > 4.5f) { err = CalRange; return false; }
    if (revision(bus) != RevDividerV2) { err = CalNoDivider; return false; }

    // With the charger in, a full ampere flows through the connector and the traces between the
    // cell and the divider, so the meter on the cell and the divider on the board read different
    // voltages — by tens of millivolts, which is the size of the error being corrected. The page
    // says "unplug the charger" in words; this makes it true.
    if (on_usb) { err = CalOnCharger; return false; }

    float v[CAL_BURSTS];
    int n = 0;
    for (int i = 0; i < CAL_BURSTS; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(CAL_GAP_MS));
        float x = 0.0f;
        if (burst_cell(x) && x >= 2.0f) v[n++] = x;
    }
    st.n = n;
    if (n < CAL_BURSTS * 3 / 4) { err = CalUnreadable; return false; }

    // Two halves in time order: their means, and each half's variance for the standard error.
    const int h = n / 2;
    double s1 = 0, s2 = 0;
    for (int i = 0; i < h; i++) s1 += v[i];
    for (int i = h; i < n; i++) s2 += v[i];
    const double m1 = s1 / h, m2 = s2 / (n - h), mean = (s1 + s2) / n;
    double q1 = 0, q2 = 0, q = 0;
    for (int i = 0; i < h; i++) q1 += (v[i] - m1) * (v[i] - m1);
    for (int i = h; i < n; i++) q2 += (v[i] - m2) * (v[i] - m2);
    for (int i = 0; i < n; i++) q += (v[i] - mean) * (v[i] - mean);
    const double se = sqrt(q1 / (h - 1) / h + q2 / (n - h - 1) / (n - h));
    const double drift = m2 - m1;
    st.sd_mv    = (float)(1000.0 * sqrt(q / (n - 1)));
    st.drift_mv = (float)(1000.0 * drift);
    if (fabs(drift) > CAL_DRIFT_V && fabs(drift) > 4.0 * se) {
        ESP_LOGW(TAG, "cell calibration refused: the mean moved %+.1f mV between the two halves of the "
                      "run (noise %.1f mV over %d bursts) — something is charging or loading the cell",
                 st.drift_mv, st.sd_mv, n);
        err = CalUnstable;
        return false;
    }
    raw = (float)mean;

    factor = reference_v / raw;
    if (factor < CAL_MIN || factor > CAL_MAX) {
        ESP_LOGW(TAG, "cell gain %.4f refused: reference %.3f V against a raw %.3f V is further out "
                      "than a divider and a converter can be", factor, reference_v, raw);
        err = CalRange;
        return false;
    }
    if (!NVSManager::getInstance().setFloat("bat_cal", factor)) { err = CalUnreadable; return false; }

    // Written straight into the cache as well, so the next reading in this same console window
    // already carries it; without this the page would keep showing the old number until the device
    // slept and re-read NVS.
    s_cal_cached = factor;
    // The smoothed voltage was built in the old units; carrying it over would drag the percentage
    // back towards the uncorrected value for the next half hour.
    forgetSmoothing();
    ESP_LOGW(TAG, "cell voltage calibrated: raw %.4f V (%d bursts, noise %.1f mV, drift %+.1f mV), "
                  "reference %.3f V -> gain %.4f", raw, n, st.sd_mv, st.drift_mv, reference_v, factor);
    return true;
}

// ---- across readings: the voltage behind the percentage ----------------------------------------
//
// A burst is steady to about a millivolt, but the cell is not: what it shows depends on what the
// device was doing a moment ago — the radio window, the panel, the CO2 lamp. On v1 the fuel gauge
// hid that behind its own model. Here it reaches the percentage directly, and in the flat middle of
// the curve a few millivolts are a whole percent — enough to re-anchor the discharge slope
// (BatteryMonitor re-anchors on a rise of more than one point) and so to keep "days left" blank for
// ever. The percentage is therefore computed from an exponential average with a half-hour time
// constant, weighted by the real time between readings, so a burst of button presses does not
// count for more than the quiet hour it interrupts.
//
// It starts over — takes the new reading as it is — whenever the old average stops describing the
// same thing: the cable went in or out, the gap was longer than three hours (a clock step, a device
// left in a drawer), or the reading jumped by more than 0.1 V (a different cell, a charger). On the
// cable it is not used at all: the charger holds the terminals, and the percentage there is
// governed by the charging ceiling, not by the voltage.
//
// The lockout never sees this. It compares a fresh reading with the cutoff, with its own second look
// (read_divider); an average that lags half an hour has no business deciding when to protect a cell.
//
// RTC_DATA: survives deep sleep, which is every tick; re-initialised on a restart, which costs one
// reading of history and nothing else.
static constexpr double EMA_TAU_S  = 1800.0;
static constexpr double EMA_GAP_S  = 3.0 * 3600.0;
static constexpr float  EMA_STEP_V = 0.10f;

RTC_DATA_ATTR static float  rtc_vf     = -1.0f;
RTC_DATA_ATTR static double rtc_vf_t   = 0.0;

void forgetSmoothing()
{
    rtc_vf = -1.0f;
    rtc_vf_t = 0.0;
}

float smoothed(float volts, double now_s, bool on_usb)
{
    if (on_usb || !isfinite(volts)) { forgetSmoothing(); return volts; }

    const double dt = now_s - rtc_vf_t;
    const bool fresh = !(isfinite(rtc_vf) && rtc_vf > 2.0f && rtc_vf < 5.0f)
                       || !(dt >= 0.0 && dt <= EMA_GAP_S)
                       || fabsf(volts - rtc_vf) > EMA_STEP_V;
    if (fresh) {
        rtc_vf = volts;
    } else {
        const float a = (float)(1.0 - exp(-dt / EMA_TAU_S));
        rtc_vf += a * (volts - rtc_vf);
    }
    rtc_vf_t = now_s;
    return rtc_vf;
}

// ---- the percentage ----------------------------------------------------------------------------
//
// One fact that a voltage alone does not know, applied on top of the curve.
//
// A FINISHED CHARGE IS 100 %. STDBY# going low is the charger saying it terminated — a fact from the
// hardware, where the voltage is an estimate. The screen already followed it; Telegram, the console
// and MQTT took the voltage and could say 96 % on a cell the charger had just called full.
//
// WHAT THIS FUNCTION NO LONGER DOES IS HOLD THE NUMBER DOWN. Until 2026-10-13 the percentage was
// pinned here to the lowest value seen since the cable came out, so that the figure could not rise on
// the battery. THE RULE IS RIGHT AND STILL IN FORCE — it is simply enforced one layer up, in
// battery_display.cpp, which owns the figure the owner reads and promises it in its own header: "on
// the battery the number never JUMPS" — a rate limit since 2026-10-19, after the one-way version of
// it was measured costing the glass seven points. When that layer was written on 2026-10-05
// this copy became a duplicate of it, and nobody took the duplicate out.
//
// A duplicate would be harmless if this were a display. It is not: what this function returns is the
// MEASUREMENT (BatteryInfo::soc_raw), and that is what goes into the history ring, the CSV export and
// the consumption estimate. A floor tracking the low edge of the noise turns the series into a
// staircase that cannot climb: it latches onto the lowest wobble it has seen and then stands still
// until the real decline catches up — and on the 103450 plateau, where five percent live inside six
// millivolts, that takes days. Reconstructed from the field board's own gauge readings 2026-10-13:
// across the newer half of the estimator's window, sixty hours, the held percentage never moved at
// all, and a straight line through a constant is flat — so the device published a runtime of 2^31
// days and a draw of 0.000 mA to Home Assistant (journal 2026-10-13). The glass keeps the rule; the
// measurement is free to wobble, which is the only way a slope can be read out of it.
float percent(float volts, double now_s, bool on_usb, bool charge_done, Cell cell)
{
    if (on_usb) {
        if (charge_done) return 100.0f;
        // A CHARGE STILL RUNNING IS NOT A FULL CELL. The curve is measured on RESTING voltage — the
        // 804050 rests at 4.058 V when full — while a charger holds the cell at about 4.2 V from
        // early in the constant-voltage phase. So the top of the curve is reached long before
        // TP4056 terminates, and without this the device said 100 % with the charge LED still lit
        // (seen on a v2 board, 2026-10-08). "Finished" is a fact the hardware reports on STDBY#;
        // the voltage cannot stand in for it, so until that fact arrives the answer stops short.
        const float soc = socFromVoltage(smoothed(volts, now_s, true), cell);
        return soc > CHARGING_MAX_PCT ? CHARGING_MAX_PCT : soc;
    }
    return socFromVoltage(smoothed(volts, now_s, false), cell);
}

int chargeComplete(bool on_usb)
{
    // No cable, no answer. STDBY# floats when the charger has no supply, and pulling it up then
    // would push current into an unpowered chip through its own output.
    if (!on_usb) return -1;

    gpio_config_t in = {};
    in.mode = GPIO_MODE_INPUT;
    in.pin_bit_mask = (1ULL << PIN_STDBY);
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in);

    // The pin is a 45 kOhm internal pull-up against an open drain; a few microseconds would do, but
    // this runs once per measurement tick and the cost is invisible.
    esp_rom_delay_us(200);
    const int lvl = gpio_get_level(PIN_STDBY);

    in.pull_up_en = GPIO_PULLUP_DISABLE;          // do not hold the pull-up through sleep
    gpio_config(&in);

    return lvl ? 0 : 1;                           // LOW = terminated (STDBY#, pin 6)
}

Cell cellFor(Rev rev)
{
    return rev == RevGaugeV1 ? Cell103450 : Cell804050;
}

struct CurvePoint { float v; float pct; };

static float interpolate(const CurvePoint *curve, int n, float volts)
{
    if (!isfinite(volts))          return 0.0f;
    if (volts >= curve[0].v)       return 100.0f;
    if (volts <= curve[n - 1].v)   return 0.0f;

    for (int i = 1; i < n; i++) {
        if (volts >= curve[i].v) {
            const float span = curve[i-1].v - curve[i].v;
            const float f    = span > 0.0f ? (volts - curve[i].v) / span : 0.0f;
            return curve[i].pct + f * (curve[i-1].pct - curve[i].pct);
        }
    }
    return 0.0f;
}

// The v1 board's cell, 103450 (2026-10-06): measured by the discharge bench on the
// second v2 bench board, second discharge (D2) of soak_2026-10-06_f708/run.bin — the first discharge
// agrees with it to 1-2 mV along the whole plateau.
//
// SCALE. That board's divider was never calibrated (gain 1.0), and that is the right scale here,
// because this table is read through a different instrument: the v1 board's fuel gauge, whose own
// reference is +-7.5 mV. Both were seen in the same state — a full cell on an idle charger after
// TP4056 terminated — and agreed to 3 mV (gauge 4.173 V, bench raw 4.176 V). The multimeter-scaled
// 804050 table below would be ~60 mV off for this instrument.
//
// THE PLATEAU. Between 4.026 and 4.060 V this cell holds 70..90 % of its charge: one percent is
// 1.4-2.8 mV. A few millivolts of reading noise are therefore several percent, which is why the
// percentage is computed from smoothed() and held from rising (percent()), and why a gauge model
// that tracks voltage sat almost still across this stretch — 1 % in a week on the field device,
// against ~5 % by its volts, 2026-10-01..08.
static const CurvePoint CURVE_103450[] = {
    // CURVE-TABLE-103450 BEGIN
    // measured by curve_bench: 93 samples, 29.7 h, rested full 4.164 V
    {4.164f, 100.0f}, {4.082f,  95.0f}, {4.060f,  90.0f}, {4.046f,  85.0f},
    {4.039f,  80.0f}, {4.033f,  75.0f}, {4.026f,  70.0f}, {4.012f,  65.0f},
    {3.983f,  60.0f}, {3.955f,  55.0f}, {3.922f,  50.0f}, {3.896f,  45.0f},
    {3.883f,  40.0f}, {3.872f,  35.0f}, {3.859f,  30.0f}, {3.842f,  25.0f},
    {3.811f,  20.0f}, {3.774f,  15.0f}, {3.737f,  10.0f}, {3.720f,   7.0f},
    {3.685f,   5.0f}, {3.589f,   3.0f}, {3.520f,   2.0f}, {3.429f,   1.0f},
    {BATTERY_CUTOFF_V, 0.0f},
    // CURVE-TABLE-103450 END
};

float socFromVoltage(float volts, Cell cell)
{
    if (cell == Cell103450)
        return interpolate(CURVE_103450, sizeof(CURVE_103450) / sizeof(CURVE_103450[0]), volts);

    // The v2 cell, 804050, measured by the discharge bench; zero is the cutoff. See the header.
    // Between the markers: written by tools/curve_fit.py --write from a discharge-bench run
    // (curve_bench.cpp). Descending voltage, last entry the cutoff at 0 %.
    static const CurvePoint CURVE[] = {
        // CURVE-TABLE BEGIN
        // measured by curve_bench: 143 samples, 36.9 h, rested full 4.058 V
        {4.058f, 100.0f}, {4.029f,  95.0f}, {4.008f,  90.0f}, {3.993f,  85.0f},
        {3.981f,  80.0f}, {3.942f,  75.0f}, {3.896f,  70.0f}, {3.864f,  65.0f},
        {3.843f,  60.0f}, {3.810f,  55.0f}, {3.777f,  50.0f}, {3.745f,  45.0f},
        {3.697f,  40.0f}, {3.651f,  35.0f}, {3.612f,  30.0f}, {3.579f,  25.0f},
        {3.542f,  20.0f}, {3.498f,  15.0f}, {3.442f,  10.0f}, {3.406f,   7.0f},
        {3.387f,   5.0f}, {3.367f,   3.0f}, {3.353f,   2.0f}, {3.333f,   1.0f},
        {BATTERY_CUTOFF_V, 0.0f},
        // CURVE-TABLE END
    };
    return interpolate(CURVE, sizeof(CURVE) / sizeof(CURVE[0]), volts);
}

void park(i2c_master_bus_handle_t bus)
{
    // Ask which board this is rather than poking the gauge blind. MAX17048::init() only adds the
    // device to the bus — it asks the chip nothing — so on a board without one this used to print
    // "initialized successfully" and then an ERROR about a failed register write: two lines that
    // together read as a fault, for a chip that is absent by design.
    //
    // It also settles the revision here, in the loop task, on a quiet bus — before the console's
    // status poll can ask from the web task and lose the bus lock to a sensor transaction.
    if (revision(bus) == RevGaugeV1) {
        MAX17048 g;
        if (g.init(bus) && g.enableHibernate()) ESP_LOGW(TAG, "fuel gauge parked (hibernate)");
        else                                    ESP_LOGW(TAG, "fuel gauge did not acknowledge hibernate");
        return;
    }
    // v2: the divider is passive and cannot be switched off from here, and STDBY# is left as an
    // input with the pull-up already disabled by chargeComplete(). Nothing to do, and saying so
    // beats a silent return that reads like a missing branch.
    ESP_LOGW(TAG, "no fuel gauge on this board — nothing to park");
}

} // namespace BatterySource
