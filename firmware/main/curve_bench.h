#pragma once

// Discharge bench for a v2 board: measures THIS cell's voltage-to-percent curve from a full charge
// to the cutoff, so battery_source.cpp can replace its generic table with a measured one. Replaces
// the whole device cycle while it runs. See curve_bench.cpp for the procedure and the reasoning.
void curve_bench_run();   // never returns
