#pragma once
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef enum { WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
typedef enum { WIFI_IF_STA, WIFI_IF_AP } wifi_interface_t;
#define ESP_IF_WIFI_STA WIFI_IF_STA
#define ESP_IF_WIFI_AP  WIFI_IF_AP
// Порядок и значения — как в esp_wifi_types_generic.h, включая алиас WPA2_ENTERPRISE на тот же
// номер, что ENTERPRISE. Заглушка, повторяющая раскладку ESP-IDF неточно, проверяет саму себя, а
// не прошивку (урок 2026-09-29, когда выдуманный путь заголовка прошёл тесты и уронил сборку).
typedef enum {
    WIFI_AUTH_OPEN = 0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_ENTERPRISE,
    WIFI_AUTH_WPA2_ENTERPRISE = WIFI_AUTH_ENTERPRISE,
    WIFI_AUTH_WPA3_PSK, WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WAPI_PSK, WIFI_AUTH_OWE,
    WIFI_AUTH_WPA3_ENT_192, WIFI_AUTH_WPA3_EXT_PSK, WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE,
    WIFI_AUTH_DPP, WIFI_AUTH_WPA3_ENTERPRISE, WIFI_AUTH_WPA2_WPA3_ENTERPRISE,
    WIFI_AUTH_WPA_ENTERPRISE, WIFI_AUTH_MAX
} wifi_auth_mode_t;
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
typedef enum { WIFI_FAST_SCAN, WIFI_ALL_CHANNEL_SCAN } wifi_scan_method_t;
typedef enum { WIFI_CONNECT_AP_BY_SIGNAL, WIFI_CONNECT_AP_BY_SECURITY } wifi_sort_method_t;
typedef enum { WPA3_SAE_PWE_UNSPECIFIED, WPA3_SAE_PWE_HUNT_AND_PECK } wifi_sae_pwe_method_t;
typedef enum { WIFI_STORAGE_FLASH, WIFI_STORAGE_RAM } wifi_storage_t;
typedef struct { bool capable, required; } wifi_pmf_config_t;
typedef struct { int8_t rssi; wifi_auth_mode_t authmode; } wifi_scan_threshold_t;
typedef struct {
    uint8_t ssid[32], password[64], bssid[6]; bool bssid_set; uint8_t channel;
    uint16_t listen_interval; wifi_scan_method_t scan_method; wifi_sort_method_t sort_method;
    wifi_scan_threshold_t threshold; wifi_pmf_config_t pmf_cfg;
} wifi_sta_config_t;
typedef struct {
    uint8_t ssid[32], password[64], ssid_len, channel; wifi_auth_mode_t authmode;
    uint8_t ssid_hidden, max_connection; uint16_t beacon_interval; wifi_pmf_config_t pmf_cfg;
} wifi_ap_config_t;
typedef union { wifi_sta_config_t sta; wifi_ap_config_t ap; } wifi_config_t;
typedef struct { uint8_t ssid[33], bssid[6], primary; int8_t rssi; wifi_auth_mode_t authmode; } wifi_ap_record_t;
typedef enum { WIFI_SCAN_TYPE_ACTIVE, WIFI_SCAN_TYPE_PASSIVE } wifi_scan_type_t;
typedef struct { uint32_t min, max; } wifi_active_scan_time_t;
typedef struct { wifi_active_scan_time_t active; uint32_t passive; } wifi_scan_time_t;
typedef struct { uint8_t *ssid; uint8_t *bssid; uint8_t channel; bool show_hidden;
                 wifi_scan_type_t scan_type; wifi_scan_time_t scan_time; uint8_t home_chan_dwell_time; } wifi_scan_config_t;
typedef struct { uint8_t ssid[32], ssid_len, bssid[6], channel; int8_t rssi; wifi_auth_mode_t authmode; } wifi_event_sta_connected_t;
typedef struct { uint8_t ssid[32], ssid_len, bssid[6], reason; int8_t rssi; } wifi_event_sta_disconnected_t;
typedef struct { int dummy; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() (wifi_init_config_t){0}
typedef enum { WIFI_EVENT_WIFI_READY, WIFI_EVENT_SCAN_DONE, WIFI_EVENT_STA_START,
               WIFI_EVENT_STA_STOP, WIFI_EVENT_STA_CONNECTED, WIFI_EVENT_STA_DISCONNECTED,
               WIFI_EVENT_AP_START, WIFI_EVENT_AP_STOP, WIFI_EVENT_AP_STACONNECTED,
               WIFI_EVENT_AP_STADISCONNECTED } wifi_event_t;
extern esp_event_base_t WIFI_EVENT;
// Радио в оснастке не поднимается. Здесь оно и не может ничего проверить: окно связи меряется на
// приборе, а на компьютере проверяется РАСПИСАНИЕ — подошёл ли срок окна, не наступила ли ночь.
static inline esp_err_t esp_wifi_init(const wifi_init_config_t *c){ (void)c; return ESP_OK; }
static inline esp_err_t esp_wifi_deinit(void){ return ESP_OK; }
static inline esp_err_t esp_wifi_set_mode(wifi_mode_t m){ (void)m; return ESP_OK; }
static inline esp_err_t esp_wifi_get_mode(wifi_mode_t *m){ if(m)*m=WIFI_MODE_NULL; return ESP_OK; }
static inline esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c){ (void)i;(void)c; return ESP_OK; }
static inline esp_err_t esp_wifi_get_config(wifi_interface_t i, wifi_config_t *c){ (void)i; if(c) memset(c,0,sizeof(*c)); return ESP_OK; }
static inline esp_err_t esp_wifi_set_storage(wifi_storage_t s){ (void)s; return ESP_OK; }
static inline esp_err_t esp_wifi_start(void){ return ESP_OK; }
static inline esp_err_t esp_wifi_stop(void){ return ESP_OK; }
static inline esp_err_t esp_wifi_connect(void){ return ESP_OK; }
static inline esp_err_t esp_wifi_disconnect(void){ return ESP_OK; }
static inline esp_err_t esp_wifi_set_ps(wifi_ps_type_t p){ (void)p; return ESP_OK; }
static inline esp_err_t esp_wifi_set_max_tx_power(int8_t p){ (void)p; return ESP_OK; }
static inline esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *c, bool block){ (void)c;(void)block; return ESP_OK; }
static inline esp_err_t esp_wifi_scan_stop(void){ return ESP_OK; }
static inline esp_err_t esp_wifi_scan_get_ap_num(uint16_t *n){ if(n)*n=0; return ESP_OK; }
static inline esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *r){ (void)r; if(n)*n=0; return ESP_OK; }
static inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *r){ (void)r; return ESP_FAIL; }
static inline esp_err_t esp_wifi_clear_ap_list(void){ return ESP_OK; }
static inline esp_err_t esp_wifi_get_mac(wifi_interface_t i, uint8_t *m){ (void)i;
    if(m){ m[0]=0x24;m[1]=0x6F;m[2]=0x28;m[3]=0xAA;m[4]=0xBB;m[5]=0x0C; } return ESP_OK; }
static inline esp_err_t esp_wifi_set_mac(wifi_interface_t i, const uint8_t *m){ (void)i;(void)m; return ESP_OK; }
static inline esp_err_t esp_wifi_restore(void){ return ESP_OK; }

#ifdef __cplusplus
}
#endif
