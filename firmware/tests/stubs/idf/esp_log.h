#pragma once
// Лог здесь ЗАХВАТЫВАЕТСЯ, а не выбрасывается. Часть решений прошивки видна только в логе —
// например отказ снять питание панели над частичным кадром, — и захват превращает такую строку в
// проверяемое утверждение вместо строки, которую надо разглядеть глазами.
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif
void host_log(char level, const char *tag, const char *fmt, ...);
#define ESP_LOGE(tag, fmt, ...) host_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) host_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) host_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) host_log('D', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) host_log('V', tag, fmt, ##__VA_ARGS__)
#define ESP_LOG_LEVEL_LOCAL(l, tag, fmt, ...) host_log('I', tag, fmt, ##__VA_ARGS__)
static inline unsigned esp_log_timestamp(void){ return 0; }
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
static inline void esp_log_level_set(const char *t, esp_log_level_t l){ (void)t; (void)l; }

#ifdef __cplusplus
}
#endif
