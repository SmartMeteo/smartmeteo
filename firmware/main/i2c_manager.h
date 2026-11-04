#pragma once

#include "driver/i2c_master.h"

class I2CManager {
public:
    static I2CManager& getInstance();
    
    void init();
    i2c_master_bus_handle_t getBusHandle() const { return bus_handle; }

    // Is a device holding the data line down? Reads the pin, so it answers only before init() creates the
    // bus; the driver owns the pin afterwards.
    bool busIsStuck();

    // Keep SDA and SCL, with their pull-ups, as they are through automatic light sleep (Matter mode):
    // gpio_sleep_sel_dis(), as the PM_SLP_DISABLE_GPIO help text prescribes.
    static void keepPadsThroughLightSleep();
    
private:
    I2CManager() : bus_handle(nullptr), initialized(false) {}
    ~I2CManager() = default;
    I2CManager(const I2CManager&) = delete;
    I2CManager& operator=(const I2CManager&) = delete;

    i2c_master_bus_handle_t bus_handle;
    bool initialized;
};
