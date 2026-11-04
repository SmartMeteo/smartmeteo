#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t size; } esp_flash_t;
extern esp_flash_t *esp_flash_default_chip;
static inline esp_err_t esp_flash_get_size(esp_flash_t *c, uint32_t *sz){ (void)c; if(sz)*sz=8*1024*1024; return ESP_OK; }
static inline esp_err_t esp_flash_get_physical_size(esp_flash_t *c, uint32_t *sz){ return esp_flash_get_size(c, sz); }

#ifdef __cplusplus
}
#endif
