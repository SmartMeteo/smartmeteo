// ============================================================================
// One question, nothing else: is the BME690's pressure channel dead, or is the firmware asking for
// it wrongly?
//
// Two boards in a row have come back with the raw pressure ADC reading 0xFFFFC — every bit of the
// 20-bit word set — while temperature, humidity and gas from the SAME burst read are correct. That
// rules out the bus and the solder joints on the data pins: those bytes come from adjacent
// registers in one transfer. What it does not yet rule out is:
//
//   1. the oversampling register never reaching the sensor (a skipped measurement has its own
//      pattern, 0x80000, but a config path that fails silently is worth excluding by reading the
//      register back);
//   2. the gas heater browning out the sensor's supply while the pressure conversion runs — the
//      heater is the only load on this part that draws real current, so the test runs every
//      pressure setting twice, with the heater on and off;
//   3. the channel being stuck since power-up and never recovering — hence a soft reset in the
//      middle of the run, after which the whole sweep repeats.
//
// If the word is 0xFFFFC in every combination, including with no heater and after a reset, the
// answer is the sensor. If any combination returns something else, the answer is in our code or on
// the board, and this print says which one.
// ============================================================================
#include "bme_probe.h"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "i2c_manager.h"
#include "bme690.h"
#include "bme69x.h"

static const char *TAG = "BMEPROBE";

static i2c_master_dev_handle_t s_raw = nullptr;   // our own handle, for reading registers back

static bool raw_read(uint8_t reg, uint8_t *out, size_t n)
{
    if (!s_raw) return false;
    return i2c_master_transmit_receive(s_raw, &reg, 1, out, n, 200) == ESP_OK;
}

static bool raw_write(uint8_t reg, uint8_t val)
{
    if (!s_raw) return false;
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_raw, buf, sizeof(buf), 200) == ESP_OK;
}

// The data field as the sensor holds it, byte for byte. Printed rather than interpreted: a field of
// 0xFF next to a sane temperature is a different story from a field of zeros, and only the bytes
// themselves tell them apart.
static void dump_field(const char *when)
{
    uint8_t f[9] = {0};
    if (!raw_read(BME69X_REG_FIELD0, f, sizeof(f))) { ESP_LOGW(TAG, "%s: field read failed", when); return; }
    const uint32_t p = ((uint32_t)f[2] << 12) | ((uint32_t)f[3] << 4) | ((uint32_t)f[4] >> 4);
    const uint32_t t = ((uint32_t)f[5] << 12) | ((uint32_t)f[6] << 4) | ((uint32_t)f[7] >> 4);
    ESP_LOGW(TAG, "%s field0 %02x %02x %02x %02x %02x %02x %02x %02x %02x | adc_p=%lu adc_t=%lu",
             when, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8],
             (unsigned long)p, (unsigned long)t);
}

static void dump_ctrl(const char *when)
{
    uint8_t hum = 0, meas = 0, cfg = 0, id = 0, var = 0;
    raw_read(0x72, &hum, 1);
    raw_read(0x74, &meas, 1);
    raw_read(0x75, &cfg, 1);
    raw_read(BME69X_REG_CHIP_ID, &id, 1);
    raw_read(BME69X_REG_VARIANT_ID, &var, 1);
    // osrs_p lives in bits 4:2 of ctrl_meas. If it reads back as 0 the sensor was told to skip
    // pressure, and everything downstream is our bug rather than its fault.
    ESP_LOGW(TAG, "%s ctrl_hum=0x%02x ctrl_meas=0x%02x (osrs_t=%u osrs_p=%u mode=%u) config=0x%02x id=0x%02x var=%u",
             when, hum, meas, (unsigned)(meas >> 5), (unsigned)((meas >> 2) & 7), (unsigned)(meas & 3),
             cfg, id, (unsigned)var);
}

// One forced measurement at the requested settings, with the raw word printed beside the result.
static void one(BME690 &bme, const char *label, uint8_t os_pres, bool heater)
{
    float t = 0, rh = 0, p = 0, gas = 0;
    bool  gas_valid = false;
    const bool ok = bme.readDataWithHeater(heater ? 300 : 0, heater ? 100 : 0,
                                           BME69X_OS_2X, os_pres, BME69X_OS_1X, heater,
                                           t, rh, p, gas, gas_valid);
    ESP_LOGW(TAG, "---- %s: %s  T=%.2f C  RH=%.1f %%  P=%.1f Pa (%.1f hPa)",
             label, ok ? "read ok" : "READ FAILED", t, rh, p, p / 100.0f);
    dump_ctrl(label);
    dump_field(label);
}

static void sweep(const char *round)
{
    BME690 bme;
    if (!I2CManager::getInstance().getBusHandle() || !bme.init(I2CManager::getInstance().getBusHandle())) {
        ESP_LOGE(TAG, "%s: sensor did not initialise", round);
        return;
    }
    ESP_LOGW(TAG, "======== %s ========", round);
    dump_ctrl("after init");

    // Every oversampling setting, heater off first. If the heater were starving the supply, the
    // "off" column would be the one that works.
    one(bme, "os_p=1x  heater off",  BME69X_OS_1X,  false);
    one(bme, "os_p=4x  heater off",  BME69X_OS_4X,  false);
    one(bme, "os_p=16x heater off",  BME69X_OS_16X, false);
    one(bme, "os_p=16x heater ON",   BME69X_OS_16X, true);
    one(bme, "os_p=1x  heater ON",   BME69X_OS_1X,  true);
    // Deliberately skipped: the sensor's own "no pressure" answer, so the run contains a known
    // reference for what a skipped channel looks like on this part.
    one(bme, "os_p=SKIPPED",         BME69X_OS_NONE, false);
}

void bme_probe_run()
{
    I2CManager::getInstance().init();
    i2c_master_bus_handle_t bus = I2CManager::getInstance().getBusHandle();

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = BME69X_I2C_ADDR_LOW;   // 0x76
    cfg.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(bus, &cfg, &s_raw) != ESP_OK) {
        s_raw = nullptr;
        ESP_LOGW(TAG, "no second handle at 0x76 - register dumps unavailable, results still valid");
    }

    while (true) {
        sweep("ROUND 1 - as found");

        // A soft reset puts every register back to its power-on value, including the trimming the
        // API reloads afterwards. If the channel is merely latched, this is what unlatches it.
        ESP_LOGW(TAG, "soft reset (0xE0 = 0xB6)");
        raw_write(BME69X_REG_SOFT_RESET, BME69X_SOFT_RESET_CMD);
        vTaskDelay(pdMS_TO_TICKS(50));
        dump_ctrl("after reset");

        sweep("ROUND 2 - after a soft reset");

        ESP_LOGW(TAG, "======== that is the whole test; repeating in 20 s ========");
        vTaskDelay(pdMS_TO_TICKS(20000));
    }
}
