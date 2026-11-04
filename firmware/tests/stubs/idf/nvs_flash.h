#pragma once
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
static inline esp_err_t nvs_flash_deinit(void){ return ESP_OK; }
static inline esp_err_t nvs_flash_init_partition(const char *p){ (void)p; return nvs_flash_init(); }
static inline esp_err_t nvs_flash_erase_partition(const char *p){ (void)p; return nvs_flash_erase(); }

#ifdef __cplusplus
}
#endif
