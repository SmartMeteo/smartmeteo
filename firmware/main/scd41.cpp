#include "scd41.h"
#include <math.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "SCD41";

// Wait at least `ms`. Sensirion's execution times are minimums, and at this build's 100 Hz tick
// vTaskDelay(pdMS_TO_TICKS(ms)) can return early: pdMS_TO_TICKS rounds down, and the first tick of
// a delay is partial. So round up to whole ticks and add one; below one tick, busy-wait instead.
static void wait_ms(uint32_t ms)
{
    if (ms < portTICK_PERIOD_MS) { esp_rom_delay_us(ms * 1000u); return; }
    vTaskDelay(pdMS_TO_TICKS(ms + portTICK_PERIOD_MS - 1) + 1);
}

bool SCD41::init(i2c_master_bus_handle_t bus_handle) {
    if (initialized) {
        return true;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x62,
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
    ESP_LOGI(TAG, "SCD41 initialized successfully at I2C address 0x62");
    return true;
}

// ESP-IDF I2C master: a device added to the bus is removed with i2c_master_bus_rm_device().
SCD41::~SCD41() {
    if (initialized && dev_handle) i2c_master_bus_rm_device(dev_handle);
}

bool SCD41::sendCommand(uint16_t command) {
    if (!initialized) return false;

    uint8_t cmd[2] = { (uint8_t)(command >> 8), (uint8_t)(command & 0xFF) };
    esp_err_t err = i2c_master_transmit(dev_handle, cmd, 2, 100);
    return (err == ESP_OK);
}

// "Read" and "send command and fetch result" sequences (§3.4): write the command, wait its
// execution time, then issue the read header. Each received word is followed by its CRC.
bool SCD41::readWords(uint16_t command, uint8_t *data, size_t len, uint32_t exec_ms) {
    if (!initialized) return false;
    uint8_t cmd[2] = { (uint8_t)(command >> 8), (uint8_t)(command & 0xFF) };
    if (i2c_master_transmit(dev_handle, cmd, 2, 100) != ESP_OK) return false;
    wait_ms(exec_ms);
    if (i2c_master_receive(dev_handle, data, len, 200) != ESP_OK) return false;
    for (size_t i = 0; i + 3 <= len; i += 3) {
        if (calculateCRC(&data[i], 2) != data[i + 2]) {
            ESP_LOGW(TAG, "CRC mismatch in reply to 0x%04X", command);
            return false;
        }
    }
    return true;
}

uint8_t SCD41::calculateCRC(uint8_t* data, uint8_t len) {
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

bool SCD41::wakeUp() {
    // Not acknowledged by the sensor (§3.11.4), so the bus reports a NACK; that is expected.
    sendCommand(0x36F6);
    wait_ms(30);
    return true;
}

bool SCD41::wakeToIdle() {
    if (!initialized) return false;
    wakeUp();
    uint64_t serial = 0;
    if (getSerial(serial)) return true;
    // Not idle. Sensirion's own example brings a sensor of unknown state back the same way:
    // wake_up, stop_periodic_measurement, reinit, then get_serial_number
    // (embedded-i2c-scd4x, example-usage/scd4x_i2c_example_usage.c, "Ensure sensor is in clean state").
    ESP_LOGW(TAG, "no answer after wake_up — stop_periodic_measurement + reinit");
    stopPeriodicMeasurement();
    reinit();
    if (getSerial(serial)) return true;
    ESP_LOGE(TAG, "sensor not idle after wake_up, stop_periodic_measurement and reinit");
    return false;
}

bool SCD41::powerDown() {
    bool ret = sendCommand(0x36E0);
    wait_ms(1);
    return ret;
}

bool SCD41::measureSingleShot() {
    ESP_LOGI(TAG, "Triggering single-shot measurement...");
    return sendCommand(0x219D);
}

bool SCD41::startPeriodicMeasurement() {
    ESP_LOGI(TAG, "Starting periodic measurement...");
    return sendCommand(0x21B1);
}

bool SCD41::stopPeriodicMeasurement() {
    ESP_LOGI(TAG, "Stopping periodic measurement...");
    bool ret = sendCommand(0x3F86);
    wait_ms(500);
    return ret;
}

bool SCD41::isDataReady(bool &ready) {
    uint8_t data[3] = {0};
    if (!readWords(0xE4B8, data, sizeof(data), 1)) return false;
    uint16_t status = (data[0] << 8) | data[1];
    ready = ((status & 0x07FF) != 0);
    return true;
}

bool SCD41::readMeasurement(uint16_t &co2, float &temperature, float &humidity) {
    uint8_t data[9] = {0};
    if (!readWords(0xEC05, data, sizeof(data), 1)) {
        ESP_LOGE(TAG, "read_measurement failed");
        return false;
    }

    co2 = (data[0] << 8) | data[1];

    uint16_t temp_raw = (data[3] << 8) | data[4];
    temperature = -45.0f + 175.0f * (float)temp_raw / 65535.0f;   // Table 11: word / (2^16 - 1)

    uint16_t hum_raw = (data[6] << 8) | data[7];
    humidity = 100.0f * (float)hum_raw / 65535.0f;

    return true;
}

bool SCD41::setAmbientPressure(float pressure_pa) {
    if (!initialized) return false;
    if (!(pressure_pa >= 70000.0f && pressure_pa <= 120000.0f)) {
        ESP_LOGW(TAG, "ambient pressure %.0f Pa outside 70000-120000 Pa — not sent", pressure_pa);
        return false;
    }

    // word = pressure [Pa] / 100 (e.g. 98700 Pa -> 987). Frame: cmd(2) + data(2) + CRC(1).
    uint16_t value = (uint16_t)((pressure_pa / 100.0f) + 0.5f);

    uint8_t buf[5];
    buf[0] = 0xE0;
    buf[1] = 0x00;
    buf[2] = (uint8_t)(value >> 8);
    buf[3] = (uint8_t)(value & 0xFF);
    buf[4] = calculateCRC(&buf[2], 2);

    esp_err_t err = i2c_master_transmit(dev_handle, buf, 5, 100);
    wait_ms(1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_ambient_pressure failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Ambient pressure set for CO2 compensation: %.0f Pa (word %u)", pressure_pa, value);
    return true;
}

bool SCD41::getTemperatureOffset(float &celsius) {
    uint8_t data[3] = {0};
    if (!readWords(0x2318, data, sizeof(data), 1)) return false;
    celsius = ((data[0] << 8) | data[1]) * 175.0f / 65535.0f;       // §3.7.2
    return true;
}

bool SCD41::setTemperatureOffset(float celsius) {
    if (!initialized) return false;
    if (celsius < 0.0f)  celsius = 0.0f;     // recommended range 0–20 °C (§3.7.1)
    if (celsius > 20.0f) celsius = 20.0f;
    const uint16_t raw = (uint16_t)lroundf(celsius * 65535.0f / 175.0f);
    uint8_t pkt[5] = { 0x24, 0x1D, (uint8_t)(raw >> 8), (uint8_t)(raw & 0xFF), 0 };
    pkt[4] = calculateCRC(&pkt[2], 2);
    const esp_err_t err = i2c_master_transmit(dev_handle, pkt, 5, 100);
    wait_ms(1);
    if (err != ESP_OK) return false;
    ESP_LOGW(TAG, "temperature offset set to %.2f C", celsius);
    return true;
}

bool SCD41::persistSettings() {
    if (!sendCommand(0x3615)) return false;
    wait_ms(800);
    ESP_LOGI(TAG, "settings persisted to SCD41 EEPROM");
    return true;
}

bool SCD41::performForcedRecalibration(uint16_t target_ppm, int16_t &correction) {
    if (!initialized) return false;

    stopPeriodicMeasurement();                    // §3.8.1 step 2: stop, wait 500 ms

    uint8_t buf[5];
    buf[0] = 0x36; buf[1] = 0x2F;                 // perform_forced_recalibration
    buf[2] = (uint8_t)(target_ppm >> 8);
    buf[3] = (uint8_t)(target_ppm & 0xFF);
    buf[4] = calculateCRC(&buf[2], 2);
    if (i2c_master_transmit(dev_handle, buf, 5, 100) != ESP_OK) {
        ESP_LOGE(TAG, "FRC command write failed");
        return false;
    }

    wait_ms(400);                                 // §3.8.1 step 3

    uint8_t data[3] = {0};
    if (i2c_master_receive(dev_handle, data, 3, 200) != ESP_OK) {
        ESP_LOGE(TAG, "FRC result read failed");
        return false;
    }
    if (calculateCRC(data, 2) != data[2]) {
        ESP_LOGE(TAG, "FRC result CRC failed");
        return false;
    }
    uint16_t raw = (data[0] << 8) | data[1];
    if (raw == 0xFFFF) {
        ESP_LOGW(TAG, "FRC returned 0xFFFF — the sensor was not operated before the command");
        return false;
    }
    correction = (int16_t)(raw - 0x8000);         // FRC correction [ppm] = word - 0x8000
    ESP_LOGI(TAG, "FRC to %u ppm succeeded, correction %d ppm", target_ppm, correction);
    return true;
}

bool SCD41::reinit() {
    if (!sendCommand(0x3646)) return false;
    wait_ms(30);
    return true;
}

bool SCD41::getSerial(uint64_t &serial) {
    uint8_t data[9] = {0};
    if (!readWords(0x3682, data, sizeof(data), 1)) return false;
    serial = ((uint64_t)data[0] << 40) | ((uint64_t)data[1] << 32) |
             ((uint64_t)data[3] << 24) | ((uint64_t)data[4] << 16) |
             ((uint64_t)data[6] <<  8) |  (uint64_t)data[7];
    return true;
}

bool SCD41::factoryReset() {
    if (!sendCommand(0x3632)) return false;
    wait_ms(1200);
    ESP_LOGW(TAG, "factory reset done — FRC and ASC history erased");
    return true;
}

bool SCD41::selfTest(uint16_t &result) {
    uint8_t data[3] = {0};
    if (!readWords(0x3639, data, sizeof(data), 10000)) return false;
    result = (data[0] << 8) | data[1];
    return true;
}
