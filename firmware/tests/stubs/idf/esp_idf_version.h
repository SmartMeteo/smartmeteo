#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#define ESP_IDF_VERSION_MAJOR 5
#define ESP_IDF_VERSION_MINOR 5
#define ESP_IDF_VERSION_PATCH 2
#define ESP_IDF_VERSION_VAL(ma,mi,pa) ((ma)*10000+(mi)*100+(pa))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,5,2)

#ifdef __cplusplus
}
#endif
