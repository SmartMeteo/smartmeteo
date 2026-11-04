#include "i2c_manager.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char* TAG = "I2C_MANAGER";

#define I2C_SDA_PIN GPIO_NUM_6
#define I2C_SCL_PIN GPIO_NUM_7

I2CManager& I2CManager::getInstance() {
    static I2CManager instance;
    return instance;
}

void I2CManager::init() {
    if (initialized) {
        return;
    }

    ESP_LOGI(TAG, "Initializing I2C Master Bus...");

    // Read before the driver takes the pins, the only moment the level can be read without disturbing
    // anything. The device comes out of deep sleep into this function on every wake, so a bus left jammed
    // by the previous cycle is found within a minute rather than staying dead until someone notices.
    const bool stuck = busIsStuck();

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
            .allow_pd = false,
        }
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C master bus: %s", esp_err_to_name(err));
        return;
    }

    initialized = true;

    // A sensor cut off mid-byte by a reset or a supply dip keeps SDA low, waiting for the clock pulses that
    // would finish the byte, and every transaction on the bus fails. ESP-IDF's remedy is
    // i2c_master_bus_reset() (driver/i2c_master.h): it resets the controller and has it clock SCL until the
    // device lets go — in hardware on this chip (SOC_I2C_SUPPORT_HW_CLR_BUS). The driver does the same by
    // itself after a transaction timeout. It replaces the hand-clocked GPIO sequence this file used to have.
    if (stuck) {
        ESP_LOGW(TAG, "SDA is stuck low — i2c_master_bus_reset()");
        err = i2c_master_bus_reset(bus_handle);
        ESP_LOGW(TAG, "bus reset: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "I2C Master Bus initialized successfully on SDA=GPIO 6, SCL=GPIO 7");
}

bool I2CManager::busIsStuck() {
    // A released bus idles high on both lines, held there by the pull-ups. SDA sitting low with
    // nobody talking means a device is still clocking out a byte it thinks was interrupted.
    // Pins only, so only before init() creates the bus: the driver owns them afterwards.
    if (initialized) return false;
    gpio_set_direction(I2C_SDA_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(I2C_SDA_PIN, GPIO_PULLUP_ONLY);
    return gpio_get_level(I2C_SDA_PIN) == 0;
}

void I2CManager::keepPadsThroughLightSleep() {
    gpio_sleep_sel_dis(I2C_SDA_PIN);
    gpio_sleep_sel_dis(I2C_SCL_PIN);
}
