#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base, int32_t id, void *data);
#define ESP_EVENT_ANY_ID (-1)
static inline esp_err_t esp_event_loop_create_default(void){ return ESP_OK; }
static inline esp_err_t esp_event_loop_delete_default(void){ return ESP_OK; }
static inline esp_err_t esp_event_handler_instance_register(esp_event_base_t b, int32_t id, esp_event_handler_t h, void *a, esp_event_handler_instance_t *i){
    (void)b;(void)id;(void)h;(void)a; if(i)*i=nullptr; return ESP_OK; }
static inline esp_err_t esp_event_handler_instance_unregister(esp_event_base_t b, int32_t id, esp_event_handler_instance_t i){ (void)b;(void)id;(void)i; return ESP_OK; }
static inline esp_err_t esp_event_handler_register(esp_event_base_t b, int32_t id, esp_event_handler_t h, void *a){ (void)b;(void)id;(void)h;(void)a; return ESP_OK; }
static inline esp_err_t esp_event_handler_unregister(esp_event_base_t b, int32_t id, esp_event_handler_t h){ (void)b;(void)id;(void)h; return ESP_OK; }

#ifdef __cplusplus
}
#endif
