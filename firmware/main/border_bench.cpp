// ============================================================================
// BORDER BENCH — the black ring around the splash screens.
// ONE STATE PER FLASH, AND EVERY STATE ENDS PARKED.
//
// THE COMPLAINT. The power-off screen, the logo after a factory reset and the LOW BATTERY lock all
// come out with a black ring around the picture. The glass is glued by hand and sits a little off
// inside the case, so the ring shows along whichever edges the bezel fails to cover. The dashboard,
// drawn every minute for months, has no such ring.
//
// ---------------------------------------------------------------------------------------------
// WHAT IS ALREADY SETTLED, so no flash is spent on it again
// ---------------------------------------------------------------------------------------------
//
//  1. THE RING IS NOT IN THE ARTWORK. Both planes of logo4g.h and lock4g.h were expanded with the
//     firmware's own rle_expand on the host and counted: ZERO non-white pixels in the outermost
//     4 px ring; the logo's ink lives in y 18..279, x 92..309. No change to the pictures can remove
//     the ring, and nothing the drawing code does to the frame buffer can reach it.
//
//  2. SO IT IS THE BORDER ELECTRODE — VBD, one output driving the area outside the 400x300 matrix
//     (SSD1683 Rev 1.0: 400 source, 300 gate, 1 VCOM, 1 VBD for border). Command 0x3C addresses it
//     and nothing else does. Image data cannot reach it.
//
//  3. IT IS THE GRAYSCALE PATH — measured, not reasoned. S7 (the shipping logo, vendor border,
//     parked by the production sequence, board f708, 2026-10-15) shows the ring, and the picture
//     changed from the previous frame, so the panel did take it. The owner named the grey gradient
//     as his suspect in his first sentence and he was right.
//
//     What is still open: whether the cure is the register value or the grayscale sequence itself.
//     The border follows a LUT of whichever waveform is loaded, and the grayscale waveform comes
//     from the panel's OTP (0x1A = 0x5A, 0x22 = 0x91) — we do not upload it and cannot inspect it,
//     so its border LUT may simply end dark. If no register value cures it, that is the answer and
//     the remedy has to be elsewhere (a white 1-bit frame after the planes, with the border driven
//     white, before the park).
//
//     The history of this paragraph is the reason to distrust anything here that was not
//     photographed.
//
//     It first asserted grayscale was innocent: the power-off screen is an ordinary 1-bit full
//     refresh (clear, drawOffScreen, Update::Full), the owner reports the ring, so parking must be
//     the variable. The first half is code and still true; the second half was an INFERENCE from
//     how the complaint was worded, not an observation. Then S0 was read as refuting it — a 1-bit
//     power-off screen, parked, no ring. That reading is also void: the bench had no
//     releaseDeepSleepHold(), so the panel was unpowered and deaf, nothing was ever written, and
//     the photograph was of the picture already on the glass. That S0 reading stays void: it has
//     to be taken again now that the bench can actually write to the panel.
//
//     Keep the park in every state regardless: the dashboard is the same 1-bit full refresh with
//     the same border byte and is also clean, so parking is still the other half of the condition,
//     and it costs nothing to hold it constant. The difference it makes:
//
//         dashboard        -> panel_to_sleep(Powered):  sleep command, latch, rail STAYS UP
//         all three splash -> panel_to_sleep(Parked):   sleep, 2 s, CUT THE RAIL, latch, 3 s
//
//     The old bench never saw this because eight of its ten states left the panel powered, and its
//     powered grayscale state came out GREY like a plain full refresh. It was measuring a condition
//     that does not reproduce the complaint, so a clean photograph from it meant nothing.
//
//  4. AND THERE IS PRECEDENT FOR THE RAIL DOING EXACTLY THIS. This project already measured the
//     whole GLASS greying overnight when the rail was cut too soon after the sleep command, and
//     fixed it with the vendor's mandatory sleep plus a settle: the controller runs boosted rails
//     (VGH near +20 V, VGL near -20 V, VCOM) and needs seconds to bring them down in order; cut
//     sooner and the charge is still on the electrodes. VBD is driven from those same rails, and
//     the settle was tuned by watching the IMAGE, never the border.
//
//  5. 0x3C, FROM THE CONTROLLER'S DATASHEET (SSD1683 Rev 1.0, command table, p. 34). Default 0xC0,
//     which sets VBD to HiZ. The byte is laid out A7 A6 A5 A4 0 0 A1 A0 — bits 3 and 2 are PRINTED
//     AS FIXED ZEROS.
//         A[7:6] VBD option:   00 GS transition (per A[1:0]) | 01 fix level (per A[5:4])
//                              10 VCOM | 11 HiZ (default)
//         A[5:4] fix level:    00 VSS | 01 VSH1 | 10 VSL | 11 VSH2
//         A[1:0] GS transition: which LUT the border follows — 00 LUT0 | 01 LUT1 | 10 LUT2 | 11 LUT3
//     Our two values against that table:
//         0x05 = 0000 0101  GS transition, LUT1, AND BIT 2 SET — a bit the table prints as zero.
//         0x01 = 0000 0001  the same intent in the table's own encoding. GxEPD2 writes exactly this
//                           on its SSD168x panels and labels it the value for a white border.
//     We write what the vendor writes (0x05 in every init, 0x80 in every partial), so the ring is
//     not a transcription slip — following them produces it. Their demo runs on a dev board that is
//     never unpowered, so the state this device spends months in was never theirs to see.
//
// ---------------------------------------------------------------------------------------------
// WHAT THIS BENCH VARIES
// ---------------------------------------------------------------------------------------------
// Three things, because three mechanisms are live and they are not the same fix:
//
//   the border byte   — what VBD is driven to during the last update (0x3C)
//   the settle        — how long after the sleep command before the rail is cut
//   the picture       — 1-bit, like the power-off screen, or 4-gray, like the logo
//
// ORDER OF THE STATES. The grayscale logo goes first, because that is the screen the complaint is
// about and the owner named the gradient as his suspect in the first sentence. S7 is the
// reproduction and S8 the first candidate value. S1..S6 stay in the table for two reasons: if S8
// cures the gradient, the same value still has to be checked against the 1-bit screen before it
// ships to both; and if S7 turns out clean, the 1-bit path has to be re-measured honestly, because
// its one reading so far was taken through a deaf panel.
//
// AND THE FIRST THING EVERY PHOTOGRAPH MUST ESTABLISH IS THAT THE GLASS CHANGED. E-ink holds the
// last frame for months with no power, so "unchanged" and "no ring" look identical. 4-gray states
// carry no on-glass label (the planes cover the whole area), so the proof is that the picture is
// now the logo and not whatever was there before.
//
// WHY NOT ONE FLASH THAT WALKS THE STATES. Because HiZ is a candidate. A floating border holds
// whatever charge it was left with, so a state following another state would show the PREVIOUS
// state's ring and be read as its own. One state, one flash, one photograph, and nothing touches
// the panel afterwards.
//
// HOW TO READ THE PHOTOGRAPHS. S0 FIRST, and nothing else means anything until its ring is the ring
// on the device: S0 is the production arrangement, so if it comes out clean then this bench does not
// reproduce the complaint and the fault is elsewhere. The 1-bit states print their own name on the
// glass, well away from the edge. The LED blinks STATE+1 times in a group every three seconds, on
// both boots, so a photograph taken minutes later still says which state it is.
// ============================================================================
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_attr.h"

#include "border_bench.h"
#include "epaper.h"
#include "screen.h"
#include "logo4g.h"

static const char *TAG = "BORDER_BENCH";

// ---------------------------------------------------------------------------
//  #   border  settle  picture   what it asks
//  0   0x05     2 s    1-bit     the production power-off screen. The 2026-10-15 run is VOID: no
//                                   latch release, so the panel was never written to. Re-run it.
//  1   0x01     2 s    1-bit     the datasheet's encoding; GxEPD2's value for a white border
//  2   0xC0     2 s    1-bit     HiZ, the controller's own default — undriven
//  3   0x50     2 s    1-bit     fix level VSH1 — hold the border hard at a source high level
//  4   0x60     2 s    1-bit     fix level VSL  — hold it low; one of S3/S4 is white, one is black
//  5   0x40     2 s    1-bit     fix level VSS  — hold it at zero
//  6   0x05    15 s    1-bit     NOT the register at all: is the settle simply too short for VBD?
//  7   0x05     2 s    4-gray    the factory-reset logo as it ships. MEASURED 2026-10-15 on f708:
//                                   THE RING IS THERE, and the picture visibly changed, so the
//                                   panel took the frame. This is the reproduction.
//  8   0x01     2 s    4-gray    the logo, documented encoding on the grayscale write only.
//                                MEASURED 2026-10-15 on f708: THE RING IS STILL THERE. So the
//                                vendor's extra bit is not the cause, and GS-transition-on-LUT1
//                                does not mean white under the grayscale waveform the way it
//                                does under the 1-bit one. The LUT is OTP and unreadable, so
//                                stop guessing LUT indices: the remaining states take the border
//                                off the waveform entirely (HiZ) or pin it to a rail.
//  9   0xC0     2 s    4-gray    the logo, grayscale write leaves the border alone (HiZ): the
//                                white pre-clear has just driven it, so it should stay white.
//                                MEASURED 2026-10-15 on f708: NO RING. This is the cure and it
//                                shipped — see the 0x3C write in EPD_HW_Init_4G. S10 and S11 were
//                                never needed; they stay in the table as the fallback if some
//                                panel lot ever turns out to need a driven level instead.
// 10   0x50     2 s    4-gray    the logo, grayscale write pins the border to VSH1
// 11   0x40     2 s    4-gray    the logo, grayscale write pins the border to VSS
// ---------------------------------------------------------------------------
#define BORDER_STATE 9

#ifndef PIN_LED
#define PIN_LED  GPIO_NUM_11
#endif

// Production's own park timings (bench_screens.cpp: PANEL_SETTLE_MS, RAIL_QUIET_MS). Kept here as
// literals with their source named: an instrument that parks differently from the thing it measures
// gives answers about itself. S6 is the one state that deliberately changes the first of them.
#define PROD_SETTLE_MS 2000
#define PROD_QUIET_MS  3000

// HOW THE SECOND BOOT KNOWS IT IS THE SECOND BOOT. By the wake-up cause, and not by a flag in
// RTC memory. The first draft kept a flag, and that draft had a defect that would have cost a
// cable session: RTC_NOINIT is uninitialised on a fresh flash, so a byte that happened to hold the
// "already parked" value would send the very first boot down the do-not-touch-the-panel path — the
// board would blink contentedly and never draw anything, and the fault would look like a dead
// panel. The timer is the only thing that put this state to sleep, so asking who woke us is exact:
// a flash, a power-on or a button cannot be mistaken for it, and nothing has to be remembered.

// Nothing was measured: 80/80 ms without pause, unmistakable against any group of state blinks.
static void blink_error()
{
    for (;;) {
        gpio_set_level(PIN_LED, 1); vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(PIN_LED, 0); vTaskDelay(pdMS_TO_TICKS(80));
    }
}

static void blink_forever(int state)
{
    for (;;) {
        for (int i = 0; i <= state; ++i) {
            gpio_set_level(PIN_LED, 1); vTaskDelay(pdMS_TO_TICKS(120));
            gpio_set_level(PIN_LED, 0); vTaskDelay(pdMS_TO_TICKS(200));
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void border_bench_run()
{
    // TWO BORDER VALUES, NOT ONE, and the split is the whole instrument on the grayscale path.
    // display4GrayRle writes 0x3C twice: EPD_HW_Init for the white pre-clear, then EPD_HW_Init_4G
    // for the planes. S7 set both to the vendor's 0x05 and reproduced the ring; if a cure were
    // applied to both at once, a clean result would not say which write mattered. So every
    // grayscale state from here leaves the white frame exactly as it ships and changes only the
    // grayscale write. gray_b = -1 means "follow init_b", which is what the 1-bit states want.
    struct { const char *name; int init_b; int gray_b; int settle_ms; bool gray4; } S[] = {
        { "S0  1-bit, 0x05 vendor (re-run)",   0x05,   -1, PROD_SETTLE_MS, false },
        { "S1  1-bit, 0x01 documented",        0x01,   -1, PROD_SETTLE_MS, false },
        { "S2  1-bit, 0xC0 HiZ",               0xC0,   -1, PROD_SETTLE_MS, false },
        { "S3  1-bit, 0x50 fix VSH1",          0x50,   -1, PROD_SETTLE_MS, false },
        { "S4  1-bit, 0x60 fix VSL",           0x60,   -1, PROD_SETTLE_MS, false },
        { "S5  1-bit, 0x40 fix VSS",           0x40,   -1, PROD_SETTLE_MS, false },
        { "S6  1-bit, 0x05, settle 15 s",      0x05,   -1,          15000, false },
        { "S7  4-gray, 0x05 + 0x05  RING",     0x05, 0x05, PROD_SETTLE_MS, true  },
        { "S8  4-gray, 0x05 + 0x01 documented",0x05, 0x01, PROD_SETTLE_MS, true  },
        { "S9  4-gray, 0x05 + 0xC0 HiZ",       0x05, 0xC0, PROD_SETTLE_MS, true  },
        { "S10 4-gray, 0x05 + 0x50 fix VSH1",  0x05, 0x50, PROD_SETTLE_MS, true  },
        { "S11 4-gray, 0x05 + 0x40 fix VSS",   0x05, 0x40, PROD_SETTLE_MS, true  },
    };
    const auto &st = S[BORDER_STATE];

    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED, 0);

    // The second boot must not touch the panel at all: no init, no draw, nothing that could repair
    // the very thing being photographed. The rail has been off since the park and stays off.
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGW(TAG, "STATE %d (%s) — second boot, panel deliberately untouched.",
                 BORDER_STATE, st.name);
        ESP_LOGW(TAG, "The rail has been off since the park. PHOTOGRAPH THE RING NOW.");
        blink_forever(BORDER_STATE);
    }

    static EPaper epd;

    // RELEASE THE LATCHES BEFORE TOUCHING THE PANEL. Without this the bench measures nothing at
    // all, which is what it did on 2026-10-15 until the owner said the picture had not changed.
    // The park at the end of the previous run latches the supply gate HIGH and the six signal
    // pads (holdForDeepSleep / holdOffForDeepSleep), and gpio_hold_en survives a reset and a
    // re-flash — so every boot of this bench started with the panel unpowered and its pads deaf.
    // A held pad silently ignores writes (driver/gpio.h), so init() reports success, the frame
    // goes nowhere, and the glass keeps whatever it held: the previous picture, photographed as
    // if it were this state's answer. Production does this at step 11 of bench_screens_run();
    // safe_mode() once had exactly this defect and it was fixed in 0.9.45.
    // Only on this path: the second boot above must keep the latches, that is what it measures.
    EPaper::releaseDeepSleepHold(true);

    // Set BEFORE init(): the values have to be in place when the sequences write 0x3C.
    EPaper::init_border = st.init_b;
    EPaper::gray_border = st.gray_b;

    // Its own fast signal, not blink_forever: that blinks the state number, and a failure that
    // blinks the same count as a finished run is a failure read as a result.
    if (!epd.init()) { ESP_LOGE(TAG, "panel init failed"); blink_error(); }

    ESP_LOGW(TAG, "STATE %d — %s", BORDER_STATE, st.name);
    ESP_LOGW(TAG, "0x3C init=0x%02X gray=0x%02X | settle %d ms | %s", st.init_b,
             st.gray_b >= 0 ? st.gray_b : st.init_b, st.settle_ms,
             st.gray4 ? "4-gray logo" : "1-bit, like the power-off screen");

    const int64_t t0 = esp_timer_get_time();
    if (st.gray4) {
        // Exactly what draw_rle() does for the logo: the white pre-clear, the sleep command and the
        // vendor's two seconds between frames, then the grayscale planes. Same call, same assets.
        epd.display4GrayRle(logo4g_plane24_rle, LOGO4G_PLANE24_RLE_LEN,
                            logo4g_plane26_rle, LOGO4G_PLANE26_RLE_LEN);
    } else {
        // Exactly what the power-off path does: clear, the real OFF screen, one full refresh. The
        // state's name goes in the free space below it so the photographs cannot be confused — the
        // logo there is 100x120 and the text sits in the middle, so the perimeter stays empty and
        // the ring is the only thing at the edge, which is the whole point.
        char l[48];
        epd.clear(0xFF);
        drawOffScreen(epd, "BORDER BENCH");
        snprintf(l, sizeof(l), "%s", st.name);
        // y=274: the OFF screen's ink ends at y=271 (measured on tests/golden/off.pbm), so this
        // is the top of the only empty band, leaving 25 clean rows between the label and the
        // bottom edge. A label nearer the edge could be mistaken for the ring itself.
        epd.drawString(8, 274, l, 0, 1);
        epd.refresh(EPaper::Update::Full);
    }
    ESP_LOGW(TAG, "drawn in %lld ms, session %s",
             (long long)((esp_timer_get_time() - t0) / 1000), epd.sessionOk() ? "ok" : "FAULTED");

    // And now park it the way the device parks it — the half the old bench left out.
    ESP_LOGW(TAG, "parking as production does: sleep, %d ms settle, cut the rail, latch, %d ms quiet",
             st.settle_ms, PROD_QUIET_MS);
    epd.hibernate();                              // mandatory before power removal (vendor)
    vTaskDelay(pdMS_TO_TICKS(st.settle_ms));
    EPaper::holdOffForDeepSleep();                // isolate the lines, then open the switch
    vTaskDelay(pdMS_TO_TICKS(PROD_QUIET_MS));     // let the rail finish falling

    ESP_LOGW(TAG, "parked. Sleeping 60 s; the timer wake will touch nothing.");
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
    esp_deep_sleep_start();                       // never returns
}
