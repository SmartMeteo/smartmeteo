#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int max_freq_mhz, min_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
typedef esp_pm_config_t esp_pm_config_esp32c6_t;
typedef void *esp_pm_lock_handle_t;
typedef enum { ESP_PM_CPU_FREQ_MAX, ESP_PM_APB_FREQ_MAX, ESP_PM_NO_LIGHT_SLEEP } esp_pm_lock_type_t;
// Записывается: тест проверяет, что лёгкий сон включён ровно там, где положено (внутри ожидания
// датчика), и выключен в остальное время.
esp_err_t esp_pm_configure(const void *cfg);
esp_err_t esp_pm_lock_create(esp_pm_lock_type_t t, int arg, const char *name, esp_pm_lock_handle_t *h);
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t h);
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t h);
static inline esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t h){ (void)h; return ESP_OK; }
typedef struct { esp_err_t (*enter_cb)(int64_t, void*); esp_err_t (*exit_cb)(int64_t, void*); void *arg; } esp_pm_sleep_cbs_register_config_t;
static inline esp_err_t esp_pm_light_sleep_register_cbs(const esp_pm_sleep_cbs_register_config_t *c){ (void)c; return ESP_OK; }

#ifdef __cplusplus
}
#endif
