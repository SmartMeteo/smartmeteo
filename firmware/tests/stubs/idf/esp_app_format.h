#pragma once
#include <stdint.h>
#include "esp_app_desc.h"

#ifdef __cplusplus
extern "C" {
#endif
#define ESP_APP_DESC_MAGIC_WORD 0xABCD5432
typedef struct { uint8_t magic_word, segment_count, spi_mode, spi_speed_size; uint32_t entry_addr;
                 uint8_t wp_pin, spi_pin_drv[3]; uint16_t chip_id; uint8_t min_chip_rev; } esp_image_header_t;
typedef struct { uint32_t load_addr, data_len; } esp_image_segment_header_t;

#ifdef __cplusplus
}
#endif
