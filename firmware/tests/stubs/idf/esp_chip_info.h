#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef enum { CHIP_ESP32 = 1, CHIP_ESP32S2 = 2, CHIP_ESP32S3 = 9, CHIP_ESP32C3 = 5, CHIP_ESP32C6 = 13 } esp_chip_model_t;
typedef struct { esp_chip_model_t model; uint32_t features; uint16_t revision; uint8_t cores; } esp_chip_info_t;
#define CHIP_FEATURE_WIFI_BGN (1<<0)
#define CHIP_FEATURE_BLE      (1<<2)
#define CHIP_FEATURE_BT       (1<<1)
#define CHIP_FEATURE_IEEE802154 (1<<6)
static inline void esp_chip_info(esp_chip_info_t *i){
    if(i){ i->model=CHIP_ESP32C6; i->features=CHIP_FEATURE_WIFI_BGN|CHIP_FEATURE_BLE|CHIP_FEATURE_IEEE802154;
           i->revision=2; i->cores=1; } }

#ifdef __cplusplus
}
#endif
