// ============================================================================
// ISOLATED PANEL BENCH — does the image survive a sleep with the panel unpowered?
//
// Nothing else runs: no sensors, no radio, no console, no schedule, no dashboard. One question
// only, so there is only one thing that can be at fault.
//
// The cycle is deliberately tiny:
//     full refresh (the picture)  ->  hibernate  ->  power off  ->  latch  ->  deep sleep 20 s
//
// Two counters are drawn ON THE GLASS, because a cable is not always attached and the serial port
// barely exists between sleeps: the wake number and how many refreshes have run. If the picture
// greys, it greys with those numbers visible, so it is obvious at a glance whether the device is
// still cycling or has stopped.
//
// How to read the result:
//   * numbers advance, picture stays black on white  -> the sequence is fine on this hardware,
//     and the fault is somewhere in the real cycle. Add its pieces back one at a time.
//   * picture greys                                  -> the sequence itself is the problem, and
//     nothing in the dashboard code matters. Then vary ONE thing here (see PANEL_BENCH_CUT).
//
// PANEL_BENCH_CUT is the one variable: 1 cuts the panel's supply for the sleep, 0 leaves it on.
// Run it at 1 first. If that greys, set it to 0 — same code, same picture, same sleep, only the
// switch differs, which makes the answer unambiguous.
// ============================================================================
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include "panel_bench.h"
#include "epaper.h"
#include "screen.h"

static const char *TAG = "PANEL_BENCH";

// What happens between the refresh and the sleep. One number, one variable at a time; the mode is
// printed on the glass so there is never any doubt which run is on screen.
//
//   0  supply KEPT                  reference: known clean
//   1  cut immediately              what the firmware did: hibernate, isolate, clamp, latch.
//                                   KNOWN TO GREY.
//   2  cut AFTER the frame lands    wait for BUSY to go idle, then a generous settle margin, and
//                                   only then hibernate and remove the supply. The panel reports
//                                   BUSY while it is driving pixels; a driver that returned early
//                                   would have us cutting power mid-waveform, and e-ink holds a
//                                   COMPLETED frame with no power but cannot hold an unfinished
//                                   one. This also MEASURES the wait, so the log says outright
//                                   whether the frame was still landing when the driver returned.
//   3  cut, lines left alone       supply off, but the signal lines keep the levels the last
//                                   transfer left them at and RESET stays idle-high. The clamping
//                                   in mode 1 was written from a guess about protection diodes,
//                                   never from the datasheet; pulling every line of an unpowered
//                                   controller to ground is not obviously the safe thing to do.
//
//   4  cut, only RESET pulled low  exactly mode 3, except RST goes to ground. Reset low means
//                                   "hold the controller in reset", and doing that to a chip whose
//                                   supply is collapsing is the crudest thing the clamp did. If
//                                   this greys, RST alone is the culprit and the rest is innocent.
//
//   5  cut, only the BUS pulled low  MOSI/SCK/CS/DC to ground; reset high, BUSY floating.
//   6  cut, only BUSY pulled down    everything at rest, and the one remaining difference applied.
//                                   By elimination this is the culprit — but elimination is not
//                                   proof, so it is applied on its own and made to show itself.
//                                   BUSY is the only line the PANEL drives, not us. Pulling an
//                                   output to ground drains whatever rail is driving it, and with
//                                   the supply cut that rail is the trickle the panel is living on.
//
// Result so far:
//   0 supply kept ............... CLEAN
//   1 cut + full clamp .......... greys      <- the original firmware
//   2 cut + wait for the frame .. greys      (log proved the frame was complete: 1799 ms, BUSY idle)
//   3 cut, lines left alone ..... CLEAN      <- so the power cut is innocent, the clamp is guilty
//   4 cut, only RESET low ....... CLEAN      <- reset is innocent
//   5 cut, only the bus low ..... CLEAN      <- the bus lines are innocent
//   6 cut, only BUSY pulled down  CLEAN      <- BUSY is innocent too
//
// Every ingredient on its own is harmless, yet all of them together (mode 1) greys. That leaves
// one thing untested: powerDownAndIsolate(), which modes 3-6 never call. It does two things
// nothing else does — it hibernates the controller a SECOND time, and it sets all six lines to
// INPUT, briefly floating them at an already-unpowered panel before they are re-driven and
// latched. Mode 7 is mode 3 with exactly that block put back.
//
//   7 cut via powerDownAndIsolate CLEAN     <- that block is innocent too
//
// Which leaves the one thing every clean run had in common and the grey one did not: in modes
// 3-7 at least ONE line stayed HIGH. Mode 4 grounded reset but left CS and DC high; mode 5
// grounded the bus but left reset high; mode 6 left everything high. Only mode 1 grounded the
// lot. An unpowered panel lives on the trickle its protection diodes draw from whatever input is
// still high — one is enough to hold its internal rails and the pixels with them. Ground every
// line and the last source is gone.
//
//   8  the production candidate     everything clamped low as before, EXCEPT reset stays idle
//                                   high — one line left to feed it. CLEAN, and mode 1 rerun as a
//                                   control greyed again: confirmed in both directions.
//   9  reset held by a PULL-UP       as 8, but reset is an input with the internal pull-up rather
//                                   than a driven output. Two things at once. It caps the trickle
//                                   at what a ~45 kOhm resistor can pass instead of what a driver
//                                   can, and it MEASURES: if the picture survives, the panel needs
//                                   only microamps in sleep, so cutting the FET saves almost
//                                   nothing over leaving it on. If the rail sags and the image
//                                   greys, the draw is real and the switch is worth having.
//                                   RESULT: the pin reads 1 before the sleep, but the picture
//                                   drifts grey SLOWLY over it — a ~45 kOhm path cannot hold the
//                                   rail. So the panel draws tens of microamps while fed through
//                                   the diode, not units. Which raises a question worth measuring:
//                                   a properly powered controller in its own deep sleep draws about
//                                   a microamp, so cutting the FET may COST current rather than
//                                   save it. Settle it with days on battery, not with argument.
//  10  every line HIGH-Z            what the controller datasheet actually prescribes, found in
//                                   the field reports of this exact fault: put CS/RST/DC (and the
//                                   rest) into INPUT before sleeping, so no signal can feed the
//                                   unpowered panel at all. We have never tried it — mode 1 drove
//                                   them to ground, modes 3-9 drove them to levels.
//                                   The reasoning it tests: e-ink demonstrably holds an image at
//                                   zero power (that is what electronic shelf labels do for years),
//                                   so greying means the panel is not properly off but lingering
//                                   part-powered. With nothing feeding it the rail collapses fast
//                                   and the controller passes through its brown-out region quickly
//                                   instead of sitting in it misbehaving. Mode 9 fits that: a weak
//                                   pull-up made the picture drift SLOWLY, like a slowly sagging
//                                   rail.
#define PANEL_BENCH_MODE  10
#define SETTLE_MS       2000       // mode 2: margin after BUSY clears, before the supply goes

#define BENCH_SLEEP_SEC  20
#define PIN_USER_BTN     GPIO_NUM_2
#define PIN_LED          GPIO_NUM_11

// Run A2. Run A cut the supply after a FULL refresh and stayed clean, while production greyed
// doing the same thing — and production almost never draws a full frame: thirty partials sit
// between them. So the variable under test is not the power cut, it is what the panel was left in
// when the rail went away. A full refresh drives every pixel through the whole waveform; a partial
// drives only what changed, differentially, and leaves the rest holding charge from an earlier
// frame. Harmless while powered, possibly not once the supply is gone.
//
// It has to replicate production's baseline dance or it tests the wrong thing: a hardware reset
// zeroes 0x26, so a naive partial after a power cut produces the OVERLAY of 4.7 — old and new
// stacked — which is a different fault with a different look. Repaint the previous frame, hand it
// over as the reference, then draw the new one.
#define PANEL_BENCH_PARTIAL  1     // 0 = always full (run A), 1 = partial series (run A2)

#define PB_MAGIC 0x9B37C0DE
RTC_NOINIT_ATTR static uint32_t pb_magic;
RTC_NOINIT_ATTR static uint32_t pb_wake;      // how many times we have woken
RTC_NOINIT_ATTR static uint32_t pb_drawn;     // how many refreshes have actually run
RTC_NOINIT_ATTR static uint32_t pb_prev_ok;   // is there a frame on the glass we can reproduce?
RTC_NOINIT_ATTR static uint32_t pb_prev_wake, pb_prev_drawn;   // exactly what it showed

void panel_bench_run(void)
{
    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED, 1);

    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (pb_magic != PB_MAGIC) { pb_magic = PB_MAGIC; pb_wake = 0; pb_drawn = 0; pb_prev_ok = 0; }
    pb_wake++;

    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, "wake %u | cause %s | supply during sleep: %s",
             (unsigned)pb_wake,
             cause == ESP_SLEEP_WAKEUP_TIMER ? "timer" :
             cause == ESP_SLEEP_WAKEUP_GPIO  ? "button" : "power-on",
             PANEL_BENCH_MODE == 0 ? "kept" : "CUT");

    // A latched pad ignores every later write while the driver still reports success, so the
    // holds go first — before anything tries to talk to the panel.
    EPaper::releaseDeepSleepHold();

    static EPaper epd;
    if (!epd.init()) {
        ESP_LOGE(TAG, "EPaper init FAILED");
    } else {
        // A plain, unambiguous picture: big counters and a frame. Nothing dithered, nothing grey
        // by design — so any grey that appears came from the sleep, not from the drawing.
        // Renders one frame into the buffer for the given numbers. Called twice on a partial wake:
        // once to reproduce what is on the glass, once for the new picture.
        auto paint = [&](unsigned wake, unsigned drawn) {
            static char l1[24], l2[24], l3[24], mode_txt[12];
            ScreenData d = {};
            snprintf(l1, sizeof(l1), "%u", wake);
            snprintf(l2, sizeof(l2), "%u", drawn);
            snprintf(l3, sizeof(l3), "%s", PANEL_BENCH_MODE == 0 ? "KEPT" : "CUT");
            snprintf(mode_txt, sizeof(mode_txt), "MODE %d", PANEL_BENCH_MODE);
            d.date = "PANEL BENCH"; d.time = l1; d.batt = 100;
            d.co2 = l2; d.fresh = l3; d.temp = "--"; d.hum = "--";
            d.aq = "Good"; d.aqLevel = -1; d.cal = nullptr;
            d.pres = "--"; d.presU = "hPa";
            d.tStart = "--:--"; d.tEnd = "--:--"; d.tMid = "--:--"; d.graphSpan = "--";
            d.city = nullptr; d.desc = mode_txt; d.wxIcon = "01d"; d.wxFresh = "now";
            d.mmLo = "--"; d.mmHi = "--"; d.outHum = "--"; d.wind = "--"; d.feels = "--";
            d.hist = nullptr; d.histN = 0;
            drawMainScreen(epd, d);
        };

        const int64_t t0 = esp_timer_get_time();
#if PANEL_BENCH_PARTIAL
        // Partial only once a frame has been put on the glass; the first wake after a cold start
        // draws full and becomes the reference.
        //
        // The repaint-the-previous-frame step that used to sit here is gone, because refresh() now
        // writes the displayed image into the reference buffer itself and production no longer does
        // it either. A bench that prepares the panel differently from the cycle it is meant to
        // stand in for answers questions about itself — 4.35 caught exactly that once already,
        // where powerDownAndIsolate() left pull-ups on that the real sleep path did not.
        const bool partial = (pb_prev_ok == PB_MAGIC);
        paint(pb_wake, pb_drawn + 1);
        epd.refresh(partial ? EPaper::Update::Partial : EPaper::Update::Full);
#else
        const bool partial = false;
        paint(pb_wake, pb_drawn + 1);
        epd.refresh(EPaper::Update::Full);     // ALWAYS a full refresh: one variable at a time
#endif
        const long long ms = (esp_timer_get_time() - t0) / 1000;
        pb_drawn++;
        pb_prev_wake = pb_wake; pb_prev_drawn = pb_drawn; pb_prev_ok = PB_MAGIC;
        ESP_LOGW(TAG, "%s refresh #%u took %lld ms | BUSY=%d",
                 partial ? "PARTIAL" : "full", (unsigned)pb_drawn, ms,
                 gpio_get_level(GPIO_NUM_18));
        // A partial is about half a second; a full one about 1.7 s. Either way, a few hundred
        // milliseconds means the transfer silently did nothing (4.19).
        if (ms < (partial ? 250 : 1200)) ESP_LOGE(TAG, "TOO FAST — the refresh did not really run");
    }

    gpio_set_level(PIN_LED, 0);

#if   PANEL_BENCH_MODE == 0
    epd.hibernate();
    EPaper::holdForDeepSleep();
    ESP_LOGW(TAG, "mode 0: supply KEPT, lines latched");

#elif PANEL_BENCH_MODE == 1
    // NEGATIVE CONTROL — the state that greyed, written out in full rather than by calling
    // holdOffForDeepSleep(). That function has since been fixed to leave reset high, so calling
    // it would no longer reproduce the fault, and the control would prove nothing. Every line to
    // the panel goes to ground here, exactly as the original did.
    {
        const gpio_num_t PWR_EN = GPIO_NUM_10, RST = GPIO_NUM_19, BUSY = GPIO_NUM_18;
        const gpio_num_t MOSI = GPIO_NUM_23, SCK = GPIO_NUM_22, CS = GPIO_NUM_21, DC = GPIO_NUM_20;

        epd.hibernate();
        epd.powerDownAndIsolate();

        gpio_set_direction(PWR_EN, GPIO_MODE_OUTPUT);
        gpio_set_level(PWR_EN, 1);
        gpio_hold_en(PWR_EN);

        const gpio_num_t low[] = { MOSI, SCK, CS, DC, RST };
        for (int i = 0; i < 5; i++) {
            gpio_set_direction(low[i], GPIO_MODE_OUTPUT);
            gpio_set_level(low[i], 0);
            gpio_hold_en(low[i]);
        }
        gpio_set_direction(BUSY, GPIO_MODE_INPUT);
        gpio_set_pull_mode(BUSY, GPIO_PULLDOWN_ONLY);
        gpio_hold_en(BUSY);

        ESP_LOGW(TAG, "mode 1 (control): supply CUT, EVERY line grounded — expected to grey");
    }

#elif PANEL_BENCH_MODE == 2
    // Wait for the panel to say it has finished, and measure the wait. BUSY high here means the
    // driver came back while pixels were still moving — which would make every earlier "the frame
    // was drawn" claim wrong, and would explain everything at once.
    {
        const int64_t tw = esp_timer_get_time();
        int waited = 0;
        while (gpio_get_level(GPIO_NUM_18) == 1 && waited < 10000) {
            vTaskDelay(pdMS_TO_TICKS(10)); waited += 10;
        }
        const long long busy_ms = (esp_timer_get_time() - tw) / 1000;
        if (busy_ms > 20) ESP_LOGE(TAG, "BUSY was STILL HIGH for %lld ms after the refresh returned",
                                   busy_ms);
        else              ESP_LOGW(TAG, "BUSY already idle when the refresh returned (%lld ms)",
                                   busy_ms);
        vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));      // generous margin: let the frame truly settle
    }
    epd.hibernate();
    epd.powerDownAndIsolate();
    EPaper::holdOffForDeepSleep();
    ESP_LOGW(TAG, "mode 2: supply CUT after BUSY idle + %d ms settle", SETTLE_MS);

#elif PANEL_BENCH_MODE == 3
    // Mirrors the pin numbers in epaper.cpp. Kept local on purpose: this is an experiment, and it
    // has no business changing the driver until it has proved something.
    {
        const gpio_num_t PWR_EN = GPIO_NUM_10, RST = GPIO_NUM_19, BUSY = GPIO_NUM_18;
        const gpio_num_t MOSI = GPIO_NUM_23, SCK = GPIO_NUM_22, CS = GPIO_NUM_21, DC = GPIO_NUM_20;

        epd.hibernate();                       // controller told to sleep, as in every other mode

        gpio_set_direction(PWR_EN, GPIO_MODE_OUTPUT);
        gpio_set_level(PWR_EN, 1);             // P-FET gate high: panel unpowered
        gpio_hold_en(PWR_EN);

        // Everything else keeps whatever level it already has — CS idle high, clock low, reset
        // high — and is merely latched so deep sleep cannot let it float. No pulling to ground.
        gpio_set_level(RST, 1);
        const gpio_num_t keep[] = { RST, MOSI, SCK, CS, DC };
        for (int i = 0; i < 5; i++) gpio_hold_en(keep[i]);
        gpio_set_direction(BUSY, GPIO_MODE_INPUT);
        gpio_set_pull_mode(BUSY, GPIO_FLOATING);

        ESP_LOGW(TAG, "mode 3: supply CUT, lines left at their idle levels (reset high)");
    }

#elif PANEL_BENCH_MODE == 4
    {
        const gpio_num_t PWR_EN = GPIO_NUM_10, RST = GPIO_NUM_19, BUSY = GPIO_NUM_18;
        const gpio_num_t MOSI = GPIO_NUM_23, SCK = GPIO_NUM_22, CS = GPIO_NUM_21, DC = GPIO_NUM_20;

        epd.hibernate();

        gpio_set_direction(PWR_EN, GPIO_MODE_OUTPUT);
        gpio_set_level(PWR_EN, 1);
        gpio_hold_en(PWR_EN);

        // The ONE difference from mode 3.
        gpio_set_direction(RST, GPIO_MODE_OUTPUT);
        gpio_set_level(RST, 0);
        gpio_hold_en(RST);

        const gpio_num_t keep[] = { MOSI, SCK, CS, DC };
        for (int i = 0; i < 4; i++) gpio_hold_en(keep[i]);
        gpio_set_direction(BUSY, GPIO_MODE_INPUT);
        gpio_set_pull_mode(BUSY, GPIO_FLOATING);

        ESP_LOGW(TAG, "mode 4: supply CUT, only RESET pulled low");
    }

#elif PANEL_BENCH_MODE == 5
    {
        const gpio_num_t PWR_EN = GPIO_NUM_10, RST = GPIO_NUM_19, BUSY = GPIO_NUM_18;
        const gpio_num_t MOSI = GPIO_NUM_23, SCK = GPIO_NUM_22, CS = GPIO_NUM_21, DC = GPIO_NUM_20;

        epd.hibernate();

        gpio_set_direction(PWR_EN, GPIO_MODE_OUTPUT);
        gpio_set_level(PWR_EN, 1);
        gpio_hold_en(PWR_EN);

        gpio_set_level(RST, 1);                  // innocent, per mode 4 — left idle high
        gpio_hold_en(RST);

        // The four bus lines to ground: the remaining half of the original clamp.
        const gpio_num_t bus[] = { MOSI, SCK, CS, DC };
        for (int i = 0; i < 4; i++) {
            gpio_set_direction(bus[i], GPIO_MODE_OUTPUT);
            gpio_set_level(bus[i], 0);
            gpio_hold_en(bus[i]);
        }
        gpio_set_direction(BUSY, GPIO_MODE_INPUT);
        gpio_set_pull_mode(BUSY, GPIO_FLOATING);

        ESP_LOGW(TAG, "mode 5: supply CUT, only MOSI/SCK/CS/DC pulled low");
    }

#elif PANEL_BENCH_MODE == 6
    {
        const gpio_num_t PWR_EN = GPIO_NUM_10, RST = GPIO_NUM_19, BUSY = GPIO_NUM_18;
        const gpio_num_t MOSI = GPIO_NUM_23, SCK = GPIO_NUM_22, CS = GPIO_NUM_21, DC = GPIO_NUM_20;

        epd.hibernate();

        gpio_set_direction(PWR_EN, GPIO_MODE_OUTPUT);
        gpio_set_level(PWR_EN, 1);
        gpio_hold_en(PWR_EN);

        gpio_set_level(RST, 1);
        const gpio_num_t keep[] = { RST, MOSI, SCK, CS, DC };
        for (int i = 0; i < 5; i++) gpio_hold_en(keep[i]);

        // The ONE difference from mode 3.
        gpio_set_direction(BUSY, GPIO_MODE_INPUT);
        gpio_set_pull_mode(BUSY, GPIO_PULLDOWN_ONLY);
        gpio_hold_en(BUSY);

        ESP_LOGW(TAG, "mode 6: supply CUT, only BUSY pulled down");
    }

#elif PANEL_BENCH_MODE == 7
    {
        const gpio_num_t PWR_EN = GPIO_NUM_10, RST = GPIO_NUM_19, BUSY = GPIO_NUM_18;
        const gpio_num_t MOSI = GPIO_NUM_23, SCK = GPIO_NUM_22, CS = GPIO_NUM_21, DC = GPIO_NUM_20;

        epd.hibernate();
        epd.powerDownAndIsolate();     // THE untested block: hibernate again, cut, float every line

        // Then settle into exactly the state mode 3 proved clean: supply off, lines at rest.
        gpio_set_direction(PWR_EN, GPIO_MODE_OUTPUT);
        gpio_set_level(PWR_EN, 1);
        gpio_hold_en(PWR_EN);

        const gpio_num_t outs[] = { RST, CS, DC, SCK, MOSI };
        const int        lvl[]  = {   1,  1,  1,   0,    0 };   // idle: reset high, CS high, clock low
        for (int i = 0; i < 5; i++) {
            gpio_set_direction(outs[i], GPIO_MODE_OUTPUT);
            gpio_set_level(outs[i], lvl[i]);
            gpio_hold_en(outs[i]);
        }
        gpio_set_direction(BUSY, GPIO_MODE_INPUT);
        gpio_set_pull_mode(BUSY, GPIO_FLOATING);

        ESP_LOGW(TAG, "mode 7: supply CUT via powerDownAndIsolate(), then lines set to rest");
    }

#elif PANEL_BENCH_MODE == 8
    {
        const gpio_num_t PWR_EN = GPIO_NUM_10, RST = GPIO_NUM_19, BUSY = GPIO_NUM_18;
        const gpio_num_t MOSI = GPIO_NUM_23, SCK = GPIO_NUM_22, CS = GPIO_NUM_21, DC = GPIO_NUM_20;

        epd.hibernate();
        epd.powerDownAndIsolate();

        gpio_set_direction(PWR_EN, GPIO_MODE_OUTPUT);
        gpio_set_level(PWR_EN, 1);
        gpio_hold_en(PWR_EN);

        // Reset stays HIGH — the one line left able to feed the panel. It is also its correct
        // idle level, so nothing is being abused to achieve this.
        gpio_set_direction(RST, GPIO_MODE_OUTPUT);
        gpio_set_level(RST, 1);
        gpio_hold_en(RST);

        // Everything else clamped low and latched, exactly as the original did.
        const gpio_num_t low[] = { MOSI, SCK, CS, DC };
        for (int i = 0; i < 4; i++) {
            gpio_set_direction(low[i], GPIO_MODE_OUTPUT);
            gpio_set_level(low[i], 0);
            gpio_hold_en(low[i]);
        }
        gpio_set_direction(BUSY, GPIO_MODE_INPUT);
        gpio_set_pull_mode(BUSY, GPIO_PULLDOWN_ONLY);
        gpio_hold_en(BUSY);

        ESP_LOGW(TAG, "mode 8: supply CUT, all clamped low EXCEPT reset (left high)");
    }

#elif PANEL_BENCH_MODE == 9
    {
        const gpio_num_t PWR_EN = GPIO_NUM_10, RST = GPIO_NUM_19, BUSY = GPIO_NUM_18;
        const gpio_num_t MOSI = GPIO_NUM_23, SCK = GPIO_NUM_22, CS = GPIO_NUM_21, DC = GPIO_NUM_20;

        epd.hibernate();
        epd.powerDownAndIsolate();

        gpio_set_direction(PWR_EN, GPIO_MODE_OUTPUT);
        gpio_set_level(PWR_EN, 1);
        gpio_hold_en(PWR_EN);

        // Reset as an INPUT with the internal pull-up: a ~45 kOhm path to 3.3 V instead of a
        // driver. Whatever the panel draws now has to fit through that resistor.
        gpio_set_direction(RST, GPIO_MODE_INPUT);
        gpio_set_pull_mode(RST, GPIO_PULLUP_ONLY);
        gpio_hold_en(RST);

        const gpio_num_t low[] = { MOSI, SCK, CS, DC };
        for (int i = 0; i < 4; i++) {
            gpio_set_direction(low[i], GPIO_MODE_OUTPUT);
            gpio_set_level(low[i], 0);
            gpio_hold_en(low[i]);
        }
        gpio_set_direction(BUSY, GPIO_MODE_INPUT);
        gpio_set_pull_mode(BUSY, GPIO_PULLDOWN_ONLY);
        gpio_hold_en(BUSY);

        // What the pin reads back right now, before the sleep: 1 means the pull-up is winning, 0
        // means the panel is already pulling harder than 45 kOhm can hold.
        ESP_LOGW(TAG, "mode 9: supply CUT, reset held by the internal pull-up (reads %d)",
                 gpio_get_level(RST));
    }

#elif PANEL_BENCH_MODE == 10
    {
        const gpio_num_t PWR_EN = GPIO_NUM_10, RST = GPIO_NUM_19, BUSY = GPIO_NUM_18;
        const gpio_num_t MOSI = GPIO_NUM_23, SCK = GPIO_NUM_22, CS = GPIO_NUM_21, DC = GPIO_NUM_20;

        epd.hibernate();

        // The supply first, and nothing else driven afterwards.
        gpio_set_direction(PWR_EN, GPIO_MODE_OUTPUT);
        gpio_set_level(PWR_EN, 1);
        gpio_hold_en(PWR_EN);

        // Every signal to high impedance, no pulls in either direction. Nothing can feed the panel
        // and nothing can drain it; its rail is left to collapse on its own.
        const gpio_num_t all[] = { RST, MOSI, SCK, CS, DC, BUSY };
        for (int i = 0; i < 6; i++) {
            gpio_hold_dis(all[i]);                       // a previous latch would override this
            gpio_set_direction(all[i], GPIO_MODE_INPUT);
            gpio_set_pull_mode(all[i], GPIO_FLOATING);
        }
        // Deliberately NOT latched: a hold on an input is about the pull, and we want none.
        // In deep sleep an undriven digital pad is high-Z anyway, which is exactly the goal.

        vTaskDelay(pdMS_TO_TICKS(50));
        ESP_LOGW(TAG, "mode 10: supply CUT, every line high-Z (RST reads %d, BUSY reads %d)",
                 gpio_get_level(RST), gpio_get_level(BUSY));
    }
#endif

    rtc_gpio_init(PIN_USER_BTN);
    rtc_gpio_set_direction(PIN_USER_BTN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(PIN_USER_BTN);
    rtc_gpio_pulldown_dis(PIN_USER_BTN);
    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_USER_BTN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_sleep_enable_timer_wakeup((uint64_t)BENCH_SLEEP_SEC * 1000000ULL);

    ESP_LOGW(TAG, "sleeping %d s", BENCH_SLEEP_SEC);
    ESP_LOGW(TAG, "==================================================");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_deep_sleep_start();
}
