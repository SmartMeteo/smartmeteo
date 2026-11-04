#pragma once
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct adc_cali_ctx *adc_cali_handle_t;
// Оснастка отдаёт сырое значение уже в милливольтах (см. host.cpp), поэтому пересчёт здесь —
// тождественный. Настоящая калибровка живёт в eFuse и на компьютере её нет.
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv);
#ifdef __cplusplus
}
#endif
