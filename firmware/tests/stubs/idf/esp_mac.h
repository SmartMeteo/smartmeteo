#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH } esp_mac_type_t;
static inline esp_err_t esp_read_mac(uint8_t *m, esp_mac_type_t t){ (void)t;
    if(m){ m[0]=0x24;m[1]=0x6F;m[2]=0x28;m[3]=0xAA;m[4]=0xBB;m[5]=0x0C; } return ESP_OK; }
static inline esp_err_t esp_efuse_mac_get_default(uint8_t *m){ return esp_read_mac(m, ESP_MAC_WIFI_STA); }

#ifdef __cplusplus
}
#endif
