#pragma once
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_core_dump_image_erase(void);
static inline esp_err_t esp_core_dump_image_check(void){ return ESP_FAIL; }
static inline esp_err_t esp_core_dump_image_get(size_t *a, size_t *s){ (void)a;(void)s; return ESP_FAIL; }

#ifdef __cplusplus
}
#endif
