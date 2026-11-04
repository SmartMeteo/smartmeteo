#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef enum { SOC_RTC_SLOW_CLK_SRC_RC_SLOW, SOC_RTC_SLOW_CLK_SRC_XTAL32K,
               SOC_RTC_SLOW_CLK_SRC_RC32K, SOC_RTC_SLOW_CLK_SRC_OSC_SLOW } soc_rtc_slow_clk_src_t;
typedef soc_rtc_slow_clk_src_t rtc_slow_freq_t;
// По умолчанию кварц: это то, что на плате, и это не то, на что прибор откатывается при отказе.
static inline soc_rtc_slow_clk_src_t rtc_clk_slow_src_get(void){ return SOC_RTC_SLOW_CLK_SRC_XTAL32K; }
static inline uint32_t rtc_clk_slow_freq_get_hz(void){ return 32768; }

#ifdef __cplusplus
}
#endif
