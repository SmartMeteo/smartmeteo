#pragma once

// Isolated bench: does the image survive a sleep with the panel unpowered? Full refresh, power
// off, deep sleep, repeat — and nothing else. Never returns. See panel_bench.cpp.
void panel_bench_run(void);
