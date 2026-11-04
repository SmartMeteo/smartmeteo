#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_attr.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC,
               ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP,
               ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB, ESP_RST_JTAG } esp_reset_reason_t;
// Задаются из теста: причина сброса решает, холодная это загрузка или пробуждение, а от этого
// зависит и счётчик незавершённых загрузок, и что прибор нарисует.
esp_reset_reason_t esp_reset_reason(void);
[[noreturn]] void esp_restart(void);                       // не возвращается: бросает HostRestart
uint32_t esp_random(void);
void esp_fill_random(void *buf, size_t len);
static inline uint32_t esp_get_free_heap_size(void){ return 180 * 1024; }
static inline uint32_t esp_get_minimum_free_heap_size(void){ return 120 * 1024; }
static inline const char *esp_get_idf_version(void){ return "v5.5.2-host"; }
esp_err_t esp_register_shutdown_handler(void (*fn)(void));
static inline esp_err_t esp_unregister_shutdown_handler(void (*fn)(void)){ (void)fn; return ESP_OK; }

#ifdef __cplusplus
}
#endif
