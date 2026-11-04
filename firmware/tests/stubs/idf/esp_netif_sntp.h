#pragma once
#include <stdint.h>
#include <sys/time.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif
// NTP не работает на этой машине и не должен: часами оснастки распоряжается тест. Успех или отказ
// синхронизации задаётся из host.h — это вход расписания (неудача откладывает попытку на 6 часов,
// а до первой установки часов — на 30 минут), и обе ветки надо уметь пройти.
typedef enum { ESP_SNTP_OPMODE_POLL, ESP_SNTP_OPMODE_LISTENONLY } esp_sntp_operatingmode_t;
typedef enum { SNTP_SYNC_MODE_IMMED, SNTP_SYNC_MODE_SMOOTH } sntp_sync_mode_t;
typedef enum { SNTP_SYNC_STATUS_RESET, SNTP_SYNC_STATUS_COMPLETED, SNTP_SYNC_STATUS_IN_PROGRESS } sntp_sync_status_t;
typedef struct { size_t num_of_servers; const char *servers[4]; bool start; bool server_from_dhcp;
                 bool renew_servers_after_new_IP; int ip_event_to_renew; bool index_of_first_server;
                 void (*sync_cb)(struct timeval *); bool wait_for_sync; sntp_sync_mode_t smooth_sync; } esp_sntp_config_t;
#define ESP_SNTP_DEFAULT_SERVERS(...) (esp_sntp_config_t){0}
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(s) (esp_sntp_config_t){0}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *cfg);
void      esp_netif_sntp_deinit(void);
esp_err_t esp_netif_sntp_sync_wait(uint32_t ticks);
static inline esp_err_t esp_netif_sntp_start(void){ return ESP_OK; }
static inline void esp_sntp_setoperatingmode(esp_sntp_operatingmode_t m){ (void)m; }
static inline void esp_sntp_setservername(int i, const char *n){ (void)i;(void)n; }
static inline void esp_sntp_init(void){}
static inline void esp_sntp_stop(void){}
static inline sntp_sync_status_t sntp_get_sync_status(void){ return SNTP_SYNC_STATUS_RESET; }

#ifdef __cplusplus
}
#endif
