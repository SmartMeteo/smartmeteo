#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_app_desc.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif
#define ESP_ERR_OTA_BASE                 0x1500
#define ESP_ERR_OTA_PARTITION_CONFLICT   0x1501
#define ESP_ERR_OTA_SELECT_INFO_INVALID  0x1502
#define ESP_ERR_OTA_VALIDATE_FAILED      0x1503
#define ESP_ERR_OTA_SMALL_SEC_VER        0x1504
#define ESP_ERR_OTA_ROLLBACK_FAILED      0x1505
#define ESP_ERR_OTA_ROLLBACK_INVALID_STATE 0x1506
typedef void *esp_ota_handle_t;
typedef enum { ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID,
               ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFF } esp_ota_img_states_t;
// Помечается и спрашивается тестом: откат — самое необратимое, что делает прошивка без участия
// человека, и проверять его надо там, где это ничего не стоит.
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p);
const esp_partition_t *esp_ota_get_last_invalid_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
bool      esp_ota_check_rollback_is_possible(void);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *d);
static inline esp_err_t esp_ota_begin(const esp_partition_t *p, size_t sz, esp_ota_handle_t *h){ (void)p;(void)sz; if(h)*h=(esp_ota_handle_t)1; return ESP_OK; }
static inline esp_err_t esp_ota_write(esp_ota_handle_t h, const void *d, size_t n){ (void)h;(void)d;(void)n; return ESP_OK; }
static inline esp_err_t esp_ota_end(esp_ota_handle_t h){ (void)h; return ESP_OK; }
static inline esp_err_t esp_ota_abort(esp_ota_handle_t h){ (void)h; return ESP_OK; }

#ifdef __cplusplus
}
#endif
