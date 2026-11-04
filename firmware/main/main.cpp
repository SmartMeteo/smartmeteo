// ============================================================================
// Smart Meteo - entry point.
//
// The device cycle lives in bench_screens.cpp: wake, measure, go online only if something needs
// the network, draw once, sleep. This file only starts it.
//
// It used to hold that cycle itself, alongside an earlier one that was already unreachable - two
// schedules sharing one set of globals, one of them dead. That is how you end up editing the path
// that is not running and hunting a ghost, so the dead one is gone from the build. It is kept
// verbatim and uncompiled in ../legacy/ for reference; the git history has it as well. What it did
// beyond the cycle — alert tiers and notifications, Telegram, battery cycle counting, discharge slope
// and the low-battery lockout — has since been ported (analytics.cpp, telegram.cpp,
// battery_monitor.cpp, battery_too_low() in bench_screens.cpp).
//
// The readings the web console serves now live in telemetry.cpp.
// ============================================================================
#include "bench_screens.h"
#include "panel_bench.h"
#include "border_bench.h"
#include "scd_bench.h"
#include "hist_bench.h"
#include "board_bench.h"
#include "bme_probe.h"
#include "curve_bench.h"

// Isolated bench, off by default. Set to 1 to answer one question and nothing else: does the
// picture survive a sleep with the panel's supply cut? It replaces the whole cycle, so no sensor,
// schedule or drawing decision can be blamed for the result.
#define PANEL_BENCH 0
// Isolated bench for the dark border ring. Set to 1, build, flash: the device stops being a
// weather station and does nothing but four labelled phases. See border_bench.cpp.
#define BORDER_BENCH 0
#define SCD_BENCH   0
#define HIST_BENCH  0
// Bring-up bench for a freshly soldered board: every node probed once, one verdict per node, and
// nothing written to any sensor's EEPROM. Set to 1, flash, read the table, set it back to 0.
#define BOARD_BENCH 0
// One question only: the BME690 pressure channel. Sweeps every oversampling setting with the gas
// heater on and off, before and after a soft reset, and prints the raw ADC word each time.
#define BME_PROBE   0
// Discharge bench for a v2 board: charge to full, pull the cable, and it measures this cell's
// voltage-to-percent curve down to the cutoff over about a day and a half. See curve_bench.cpp.
#define CURVE_BENCH 0

extern "C" void app_main(void)
{
#if CURVE_BENCH
    curve_bench_run();     // never returns
#endif
#if BME_PROBE
    bme_probe_run();       // never returns
#endif
#if BOARD_BENCH
    board_bench_run();     // never returns
#endif
#if HIST_BENCH
    hist_bench_run();      // never returns
#endif
#if SCD_BENCH
    scd_bench_run();       // never returns
#endif
#if BORDER_BENCH
    border_bench_run();    // never returns
#endif
#if PANEL_BENCH
    panel_bench_run();     // never returns
#endif
    bench_screens_run();   // never returns: it ends in deep sleep
}
