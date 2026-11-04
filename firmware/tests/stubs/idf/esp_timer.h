#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
// Задаётся из теста: длительность такта — вход для расчёта опережения, и она должна быть
// повторяемой, иначе проверка опережения меряла бы скорость этой машины.
int64_t esp_timer_get_time(void);
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); void *arg; int dispatch_method; const char *name; bool skip_unhandled_events; } esp_timer_create_args_t;
static inline esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h){ (void)a; if(h)*h=nullptr; return ESP_OK; }
static inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t p){ (void)h; (void)p; return ESP_OK; }
static inline esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t p){ (void)h; (void)p; return ESP_OK; }
static inline esp_err_t esp_timer_stop(esp_timer_handle_t h){ (void)h; return ESP_OK; }
static inline esp_err_t esp_timer_delete(esp_timer_handle_t h){ (void)h; return ESP_OK; }

#ifdef __cplusplus
}
#endif
