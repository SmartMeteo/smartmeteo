#pragma once

// Bring-up bench for a freshly soldered board. Replaces the whole device cycle: nothing here
// schedules, sleeps or measures for the product, so a result can only be about the hardware.
// See board_bench.cpp for what it checks and what each verdict means.
void board_bench_run();   // never returns
