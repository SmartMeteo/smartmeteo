#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
static inline esp_err_t mdns_init(void){ return ESP_OK; }
static inline void      mdns_free(void){}
static inline esp_err_t mdns_hostname_set(const char *h){ (void)h; return ESP_OK; }
static inline esp_err_t mdns_instance_name_set(const char *n){ (void)n; return ESP_OK; }
static inline esp_err_t mdns_service_add(const char *i, const char *s, const char *p, uint16_t port, void *t, size_t n){ (void)i;(void)s;(void)p;(void)port;(void)t;(void)n; return ESP_OK; }

#ifdef __cplusplus
}
#endif
