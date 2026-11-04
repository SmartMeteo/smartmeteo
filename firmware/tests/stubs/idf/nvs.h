#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
// НАСТОЯЩЕЕ хранилище ключ-значение в памяти, а не заглушка-пустышка. Над ним собирается
// настоящий main/nvs_manager.cpp, поэтому путь настроек исполняется как на приборе — включая
// хранение float битовым образом U32, из-за которого «поправка не читается» уже было дефектом.
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 1, NVS_READWRITE = 2 } nvs_open_mode_t;
#define ESP_ERR_NVS_BASE          0x1100
#define ESP_ERR_NVS_NOT_FOUND     0x1102
#define ESP_ERR_NVS_INVALID_HANDLE 0x1103
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110D
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
#define ESP_ERR_NVS_INVALID_LENGTH 0x110A
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_get_u8 (nvs_handle_t h, const char *key, uint8_t  *out);
esp_err_t nvs_set_u8 (nvs_handle_t h, const char *key, uint8_t   v);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t  v);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t  v);
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t  *out);
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t   v);
esp_err_t nvs_get_str (nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_set_str (nvs_handle_t h, const char *key, const char *v);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);

#ifdef __cplusplus
}
#endif
