#pragma once
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { adc_unit_t unit_id; adc_channel_t chan; adc_atten_t atten; adc_bitwidth_t bitwidth; }
        adc_cali_curve_fitting_config_t;
// Управляется из теста: Host::adc_cali_ok. Отказ калибровки — не выдумка, это состояние чипа без
// данных в eFuse, и прошивка обязана на нём отказаться читать, а не выдать неоткалиброванное число.
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *c, adc_cali_handle_t *h);
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t h);
#ifdef __cplusplus
}
#endif
