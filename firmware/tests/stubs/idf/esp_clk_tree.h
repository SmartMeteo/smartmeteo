#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef int soc_module_clk_t;
#define SOC_MOD_CLK_RTC_SLOW 1
#define SOC_MOD_CLK_XTAL32K 2
#define SOC_MOD_CLK_RC_FAST 3
typedef enum { ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX,
               ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT } esp_clk_tree_src_freq_precision_t;
static inline esp_err_t esp_clk_tree_src_get_freq_hz(soc_module_clk_t c, esp_clk_tree_src_freq_precision_t p, uint32_t *hz){
    (void)c;(void)p; if(hz)*hz=32768; return ESP_OK; }

#ifdef __cplusplus
}
#endif
