#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
// Флеш в ОЗУ. Это не косметика: над ним собираются НАСТОЯЩИЕ hist_store.cpp и evt_log.cpp, значит
// кольцо истории и журнал событий в оснастке работают по-настоящему — восстановление головы
// сканированием, перенос через сектор, отметки времени. Правило записи в историю («только на
// газовом тике и не чаще h_step») — это логика цикла, и проверять её больше негде.
typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_TYPE_ANY = 0xFF } esp_partition_type_t;
typedef enum {
    ESP_PARTITION_SUBTYPE_APP_OTA_0   = 0x10,
    ESP_PARTITION_SUBTYPE_APP_OTA_1   = 0x11,
    ESP_PARTITION_SUBTYPE_DATA_OTA    = 0x00,
    ESP_PARTITION_SUBTYPE_DATA_NVS    = 0x02,
    ESP_PARTITION_SUBTYPE_DATA_COREDUMP = 0x03,
    ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82,
    ESP_PARTITION_SUBTYPE_ANY         = 0xFF,
} esp_partition_subtype_t;
typedef struct {
    esp_partition_type_t type; esp_partition_subtype_t subtype;
    uint32_t address, size, erase_size; char label[17]; bool encrypted;
} esp_partition_t;
// Размеры — из main/partitions.csv, чтобы ёмкость кольца в тесте была та же, что на приборе.
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                               esp_partition_subtype_t subtype, const char *label);
esp_err_t esp_partition_read (const esp_partition_t *p, size_t off, void *dst, size_t n);
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t n);
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n);

#ifdef __cplusplus
}
#endif
