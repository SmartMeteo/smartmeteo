#include "max17048.h"
#include "esp_log.h"
#include <math.h>

static const char* TAG = "MAX17048";

bool MAX17048::init(i2c_master_bus_handle_t bus_handle) {
    if (initialized) {
        return true;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x36,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        }
    };

    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(err));
        return false;
    }

    initialized = true;
    ESP_LOGI(TAG, "MAX17048 initialized successfully at I2C address 0x36");
    return true;
}

bool MAX17048::readRegister(uint8_t reg, uint16_t &value) {
    if (!initialized) return false;

    uint8_t data_buf[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(dev_handle, &reg, 1, data_buf, 2, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Read reg 0x%02X failed: %s", reg, esp_err_to_name(err));
        return false;
    }

    value = (data_buf[0] << 8) | data_buf[1];
    return true;
}

bool MAX17048::writeRegister(uint8_t reg, uint16_t value) {
    if (!initialized) return false;

    uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    esp_err_t err = i2c_master_transmit(dev_handle, buf, 3, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Write reg 0x%02X failed: %s", reg, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool MAX17048::getVCELL(float &voltage) {
    uint16_t value;
    if (readRegister(0x02, value)) {
        voltage = value * 0.000078125f;
        return true;
    }
    return false;
}

bool MAX17048::getSOC(float &percentage) {
    uint16_t value;
    if (readRegister(0x04, value)) {
        percentage = (value >> 8) + (value & 0xFF) / 256.0f;
        return true;
    }
    return false;
}

bool MAX17048::getCRATE(float &rate) {
    uint16_t value;
    if (readRegister(0x16, value)) {
        rate = (int16_t)value * 0.208f;
        return true;
    }
    return false;
}

bool MAX17048::getVersion(uint16_t &version) {
    return readRegister(0x08, version);
}

bool MAX17048::getID(uint8_t &id) {
    uint16_t value;
    if (!readRegister(0x18, value)) return false;
    id = value & 0xFF;
    return true;
}

bool MAX17048::getStatusFlags(uint8_t &flags) {
    uint16_t value;
    if (!readRegister(0x1A, value)) return false;
    flags = (value >> 8) & 0x3F;
    return true;
}

bool MAX17048::clearStatusFlags(uint8_t mask) {
    uint16_t value;
    if (!readRegister(0x1A, value)) return false;
    value &= ~((uint16_t)(mask & 0x3F) << 8);
    if (!writeRegister(0x1A, value)) return false;
    // CONFIG.ALRT (bit 5) latches with the alert and is cleared by the host. The pin is not wired on
    // this board; the bit is cleared anyway so the register reflects the flags just cleared.
    uint16_t config;
    if (!readRegister(0x0C, config)) return false;
    if (!(config & 0x0020)) return true;
    return writeRegister(0x0C, config & ~0x0020);
}

// ESP-IDF I2C master: a device added to the bus is removed with i2c_master_bus_rm_device().
MAX17048::~MAX17048() {
    if (dev_handle) i2c_master_bus_rm_device(dev_handle);
}

bool MAX17048::setTempCompensation(float tempC) {
    // Default-model coefficients: RCOMP0=0x97 at 20C, -0.5 counts/C above,
    // -5.0 counts/C below (MAX17048 datasheet).
    float rcomp = (float)0x97;
    rcomp += (tempC > 20.0f) ? (tempC - 20.0f) * -0.5f
                             : (tempC - 20.0f) * -5.0f;
    if (rcomp < 0.0f) rcomp = 0.0f;
    if (rcomp > 255.0f) rcomp = 255.0f;

    uint16_t config;
    if (!readRegister(0x0C, config)) return false;
    config = (config & 0x00FF) | ((uint16_t)lroundf(rcomp) << 8);
    return writeRegister(0x0C, config);
}

bool MAX17048::enableHibernate() {
    return writeRegister(0x0A, 0xFFFF);
}

bool MAX17048::setVoltageAlerts(float vmin, float vmax) {
    // VALRT (0x14): high byte = min, low byte = max, 20 mV per LSB
    auto toReg = [](float v) -> uint16_t {
        long c = lroundf(v / 0.020f);
        if (c < 0) c = 0;
        if (c > 255) c = 255;
        return (uint16_t)c;
    };
    return writeRegister(0x14, (toReg(vmin) << 8) | toReg(vmax));
}

bool MAX17048::setSOCAlertThreshold(uint8_t percent) {
    if (percent < 1) percent = 1;
    if (percent > 32) percent = 32;
    uint16_t config;
    if (!readRegister(0x0C, config)) return false;
    // ATHD field (CONFIG[4:0]) encodes threshold as (32 - percent)
    config = (config & ~0x001F) | (uint16_t)(32 - percent);
    return writeRegister(0x0C, config);
}

bool MAX17048::quickstart() {
    ESP_LOGI(TAG, "Triggering ModelGauge Quickstart (re-estimating SOC)...");
    return writeRegister(0x06, 0x4000);
}

