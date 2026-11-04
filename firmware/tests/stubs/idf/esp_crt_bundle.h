#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
static inline esp_err_t esp_crt_bundle_attach(void *conf){ (void)conf; return ESP_OK; }
static inline void      esp_crt_bundle_detach(void *conf){ (void)conf; }

#ifdef __cplusplus
}
#endif
