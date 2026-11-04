#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef void *temperature_sensor_handle_t;
typedef struct { int range_min, range_max; int clk_src; } temperature_sensor_config_t;
#define TEMPERATURE_SENSOR_CONFIG_DEFAULT(lo,hi) (temperature_sensor_config_t){ (lo), (hi), 0 }
static inline esp_err_t temperature_sensor_install(const temperature_sensor_config_t *c, temperature_sensor_handle_t *h){ (void)c; if(h)*h=nullptr; return ESP_OK; }
static inline esp_err_t temperature_sensor_enable(temperature_sensor_handle_t h){ (void)h; return ESP_OK; }
static inline esp_err_t temperature_sensor_disable(temperature_sensor_handle_t h){ (void)h; return ESP_OK; }
static inline esp_err_t temperature_sensor_uninstall(temperature_sensor_handle_t h){ (void)h; return ESP_OK; }
static inline esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t h, float *t){ (void)h; if(t)*t=30.0f; return ESP_OK; }

#ifdef __cplusplus
}
#endif
