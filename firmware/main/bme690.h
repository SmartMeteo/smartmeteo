#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "bme69x.h"

class BME690 {
public:
    BME690() : dev_handle(nullptr), initialized(false) {}
    ~BME690();
    BME690(const BME690 &) = delete;
    BME690 &operator=(const BME690 &) = delete;
    
    bool init(i2c_master_bus_handle_t bus_handle);

    // Closed-loop measurement: apply the exact heater + oversampling that BSEC
    // requested via bsec_sensor_control(), then measure. This is the by-the-book
    // BSEC integration path and the result should be fed straight back to BSEC.
    bool readDataWithHeater(uint16_t heatr_temp, uint16_t heatr_dur,
                            uint8_t os_temp, uint8_t os_pres, uint8_t os_hum, bool run_gas,
                            float &temperature, float &humidity, float &pressure, float &gas_resistance,
                            bool &gas_valid);   // BME69X_GASM_VALID_MSK set in the sample

    // Diagnostics for a suspect pressure reading: dumps the calibration coefficients
    // read out of the sensor's OTP at init plus the raw pressure ADC word of the last
    // measurement. Tells apart "OTP read came back corrupt" (garbage par_p*) from
    // "the pressure channel itself is dead" (ADC stuck at 0 / 0xFFFFF).
    void logDiag();

private:
    i2c_master_dev_handle_t dev_handle;
    struct bme69x_dev bme_dev;
    struct bme69x_conf conf;
    struct bme69x_heatr_conf heatr_conf;
    bool initialized;
};
