#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef void *TaskHandle_t_wdt;
typedef struct { uint32_t timeout_ms; uint32_t idle_core_mask; bool trigger_panic; } esp_task_wdt_config_t;
static inline esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t *c){ (void)c; return ESP_OK; }
static inline esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t *c){ (void)c; return ESP_OK; }
static inline esp_err_t esp_task_wdt_add(void *h){ (void)h; return ESP_OK; }
static inline esp_err_t esp_task_wdt_delete(void *h){ (void)h; return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void){ return ESP_OK; }
static inline esp_err_t esp_task_wdt_status(void *h){ (void)h; return ESP_OK; }

#ifdef __cplusplus
}
#endif
