#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
static inline int esp_clk_cpu_freq(void){ return 160 * 1000 * 1000; }
static inline int esp_clk_apb_freq(void){ return 80 * 1000 * 1000; }
static inline uint64_t esp_clk_rtc_time(void){ return 0; }

#ifdef __cplusplus
}
#endif
