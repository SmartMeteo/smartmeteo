#include "bme690.h"
#include "pm_gate.h"
#include "esp_log.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char* TAG = "BME690";

// =============================================================================
// BME690 DRIVER
// -----------------------------------------------------------------------------
// Driven through the dedicated Bosch BME69x Sensor API v1.1.0 (bme69x.c), which
// natively handles the BME690's 24-bit pressure ADC and its compensation model.
// Because of this, NO pressure scale factor is needed: the old BME680-API-on-
// BME690 workaround (divide pressure by ~3.076) is gone — pressure, temperature
// and gas now come out physically correct straight from the API.
// =============================================================================

// Static I2C callbacks for Bosch BME69x API
static int8_t bme69x_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr) {
    i2c_master_dev_handle_t dev_handle = (i2c_master_dev_handle_t)intf_ptr;
    esp_err_t err = i2c_master_transmit_receive(dev_handle, &reg_addr, 1, reg_data, len, 100);
    return (err == ESP_OK) ? BME69X_OK : BME69X_E_COM_FAIL;
}

static int8_t bme69x_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr) {
    i2c_master_dev_handle_t dev_handle = (i2c_master_dev_handle_t)intf_ptr;
    uint8_t* write_buf = (uint8_t*)malloc(len + 1);
    if (!write_buf) {
        return BME69X_E_COM_FAIL;
    }
    write_buf[0] = reg_addr;
    memcpy(&write_buf[1], reg_data, len);
    esp_err_t err = i2c_master_transmit(dev_handle, write_buf, len + 1, 100);
    free(write_buf);
    return (err == ESP_OK) ? BME69X_OK : BME69X_E_COM_FAIL;
}

// The SensorAPI asks for AT LEAST `period` microseconds (bme69x_defs.h: "The time period in microseconds").
// vTaskDelay(pdMS_TO_TICKS(period / 1000)) rounded down twice at this 100 Hz tick and could return up to a
// tick early — reading before the measurement finished. Below one tick, busy-wait; above, round up.
static void bme69x_delay_us(uint32_t period, void *intf_ptr) {
    (void)intf_ptr;
    if (period < 1000u * portTICK_PERIOD_MS) {
        ets_delay_us(period);
    } else {
        // The heater profile is the other long wait in a measuring wake — about three seconds of
        // the six. Same gate as the SCD41 conversion: sleep is permitted here and nowhere else.
        PmGate::Nap nap((period + 999u) / 1000u);
        vTaskDelay(pdMS_TO_TICKS((period + 999u) / 1000u + portTICK_PERIOD_MS - 1) + 1);
    }
}

// ESP-IDF I2C master: a device added to the bus is removed with i2c_master_bus_rm_device().
BME690::~BME690() {
    if (dev_handle) i2c_master_bus_rm_device(dev_handle);
}

bool BME690::init(i2c_master_bus_handle_t bus_handle) {
    if (initialized) {
        return true;
    }

    // Add device to I2C bus
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BME69X_I2C_ADDR_LOW, // 0x76
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        }
    };

    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add BME690 device to I2C bus: %s", esp_err_to_name(err));
        return false;
    }

    // Setup Bosch API structure
    bme_dev.read = bme69x_i2c_read;
    bme_dev.write = bme69x_i2c_write;
    bme_dev.delay_us = bme69x_delay_us;
    bme_dev.intf = BME69X_I2C_INTF;
    bme_dev.intf_ptr = dev_handle;
    bme_dev.amb_temp = 25; // 25 deg C ambient

    int8_t rslt = bme69x_init(&bme_dev);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "Bosch bme69x_init failed: %d", rslt);
        return false;
    }
    ESP_LOGI(TAG, "BME690 detected. chip_id=0x%02x, variant_id=%u",
             (unsigned)bme_dev.chip_id, (unsigned)bme_dev.variant_id);

    // Configure default sensor settings
    conf.filter = BME69X_FILTER_OFF;
    conf.odr = BME69X_ODR_NONE;
    conf.os_hum = BME69X_OS_1X;
    conf.os_pres = BME69X_OS_16X;
    conf.os_temp = BME69X_OS_2X;

    rslt = bme69x_set_conf(&conf, &bme_dev);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "Bosch bme69x_set_conf failed: %d", rslt);
        return false;
    }

    // Configure default heater settings
    heatr_conf.enable = BME69X_ENABLE;
    heatr_conf.heatr_temp = 320; // 320 degrees C
    heatr_conf.heatr_dur = 150;  // 150 ms

    rslt = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &heatr_conf, &bme_dev);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "Bosch bme69x_set_heatr_conf failed: %d", rslt);
        return false;
    }

    initialized = true;
    ESP_LOGI(TAG, "Bosch BME690 Sensor API initialized successfully!");
    return true;
}

bool BME690::readDataWithHeater(uint16_t heatr_temp, uint16_t heatr_dur,
                                uint8_t os_temp, uint8_t os_pres, uint8_t os_hum, bool run_gas,
                                float &temperature, float &humidity, float &pressure, float &gas_resistance, bool &gas_valid) {
    gas_valid = false;
    if (!initialized) return false;

    // 1. Apply the oversampling BSEC asked for
    conf.os_temp = os_temp;
    conf.os_pres = os_pres;
    conf.os_hum  = os_hum;
    int8_t rslt = bme69x_set_conf(&conf, &bme_dev);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "readDataWithHeater: set_conf failed: %d", rslt);
        return false;
    }

    // 2. Apply the heater profile BSEC asked for
    heatr_conf.enable     = run_gas ? BME69X_ENABLE : BME69X_DISABLE;
    heatr_conf.heatr_temp = heatr_temp;
    heatr_conf.heatr_dur  = heatr_dur;
    rslt = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &heatr_conf, &bme_dev);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "readDataWithHeater: set_heatr_conf failed: %d", rslt);
        return false;
    }

    // 3. Trigger the forced measurement
    rslt = bme69x_set_op_mode(BME69X_FORCED_MODE, &bme_dev);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "readDataWithHeater: set_op_mode failed: %d", rslt);
        return false;
    }

    // 4. Wait for TPH conversion + the heater duration (both required before read)
    uint32_t meas_dur = bme69x_get_meas_dur(BME69X_FORCED_MODE, &conf, &bme_dev);
    bme_dev.delay_us(meas_dur + (uint32_t)heatr_dur * 1000u, bme_dev.intf_ptr);

    // 5. Fetch the compensated data
    struct bme69x_data data;
    uint8_t n_fields = 0;
    rslt = bme69x_get_data(BME69X_FORCED_MODE, &data, &n_fields, &bme_dev);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "readDataWithHeater: get_data failed: %d", rslt);
        return false;
    }

    if (n_fields > 0) {
        temperature = data.temperature;
        humidity = data.humidity;
        pressure = data.pressure;
        gas_resistance = data.gas_resistance;
        // Bosch's example gives BSEC a sample only when this bit is set (bsec_integration.c).
        gas_valid = (data.status & BME69X_GASM_VALID_MSK) != 0;
        ESP_LOGI(TAG, "[BSEC-ctl] T=%.2fC H=%.2f%% P=%.1fPa Gas=%.0fOhm (heater %dC/%dms gas=%d valid=%d)",
                 temperature, humidity, pressure, gas_resistance,
                 heatr_temp, heatr_dur, run_gas, (data.status & BME69X_GASM_VALID_MSK) ? 1 : 0);
        return true;
    }
    return false;
}

void BME690::logDiag() {
    if (!initialized) {
        ESP_LOGW(TAG, "[DIAG] sensor not initialized");
        return;
    }

    // Raw pressure ADC of the last completed measurement, read straight off the sensor
    // (field0: 0x1D status, 0x1F/0x20/0x21 = press msb/lsb/xlsb, 20 bits left-aligned).
    uint8_t reg = BME69X_REG_FIELD0;
    uint8_t f[9] = {0};
    uint32_t adc_pres = 0;
    if (i2c_master_transmit_receive(dev_handle, &reg, 1, f, sizeof(f), 200) == ESP_OK) {
        adc_pres = ((uint32_t)f[2] << 12) | ((uint32_t)f[3] << 4) | ((uint32_t)f[4] >> 4);
    } else {
        ESP_LOGW(TAG, "[DIAG] raw field0 read failed");
    }

    // A healthy sea-level-ish reading lands the 20-bit ADC roughly mid-scale; 0 or the
    // 0xFFFFF rail means the pressure channel returned nothing usable.
    const char *adc_verdict = (adc_pres == 0)         ? "STUCK LOW (channel dead?)"
                            : (adc_pres >= 0xFFFF0)   ? "STUCK HIGH (channel dead?)"
                                                      : "plausible range";

    ESP_LOGW(TAG, "[DIAG] chip_id=0x%02x variant_id=%u | raw pressure ADC=%lu (0x%05lx) -> %s",
             (unsigned)bme_dev.chip_id, (unsigned)bme_dev.variant_id,
             (unsigned long)adc_pres, (unsigned long)adc_pres, adc_verdict);

    // OTP calibration coefficients. If the one-time read at init glitched, these come back
    // as zeros / 0xFFFF and the (nonlinear) pressure compensation produces nonsense while
    // temperature and humidity can still look believable — exactly the symptom we're chasing.
    const struct bme69x_calib_data &c = bme_dev.calib;
    ESP_LOGW(TAG, "[DIAG] par_t: %u %u %d | par_p1..5: %u %u %d %d %d",
             (unsigned)c.par_t1, (unsigned)c.par_t2, (int)c.par_t3,
             (unsigned)c.par_p1, (unsigned)c.par_p2, (int)c.par_p3, (int)c.par_p4, (int)c.par_p5);
    ESP_LOGW(TAG, "[DIAG] par_p6..11: %d %d %d %d %d %d",
             (int)c.par_p6, (int)c.par_p7, (int)c.par_p8,
             (int)c.par_p9, (int)c.par_p10, (int)c.par_p11);

    // par_p1 is the dominant pressure offset term; a sane part reads a large non-zero value.
    if (c.par_p1 == 0 || c.par_p1 == 0xFFFF || c.par_t1 == 0 || c.par_t1 == 0xFFFF) {
        ESP_LOGE(TAG, "[DIAG] calibration looks CORRUPT -> OTP read at init failed (software-fixable)");
    }
}
