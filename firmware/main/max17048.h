#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"

class MAX17048 {
public:
    // STATUS register alert flags (high byte of reg 0x1A). Latched by the IC
    // until cleared, so they report events that happened while the MCU slept.
    static constexpr uint8_t STATUS_RI = 0x01; // reset indicator: IC lost power (battery swap / first power-up)
    static constexpr uint8_t STATUS_VH = 0x02; // voltage above VALRT.MAX
    static constexpr uint8_t STATUS_VL = 0x04; // voltage below VALRT.MIN
    static constexpr uint8_t STATUS_VR = 0x08; // voltage dipped below VRESET (cell brown-out / re-insert)
    static constexpr uint8_t STATUS_HD = 0x10; // SOC below the ATHD empty-alert threshold
    static constexpr uint8_t STATUS_SC = 0x20; // SOC changed by >= 1%

    MAX17048() : dev_handle(nullptr), initialized(false) {}
    ~MAX17048();
    MAX17048(const MAX17048 &) = delete;
    MAX17048 &operator=(const MAX17048 &) = delete;

    bool init(i2c_master_bus_handle_t bus_handle);

    bool readRegister(uint8_t reg, uint16_t &value);
    bool writeRegister(uint8_t reg, uint16_t value);

    bool getVCELL(float &voltage);
    bool getSOC(float &percentage);
    bool getCRATE(float &rate);          // %/h, signed; resolution 0.208 %/h
    bool getVersion(uint16_t &version);
    bool getID(uint8_t &id);

    bool getStatusFlags(uint8_t &flags); // STATUS_* bits
    bool clearStatusFlags(uint8_t mask);

    // ModelGauge RCOMP temperature compensation (datasheet default-model
    // coefficients). Call every cycle with the current board temperature —
    // this is the main SOC-accuracy lever the chip offers.
    bool setTempCompensation(float tempC);

    // Hibernate: ModelGauge keeps running at a 45 s cadence (~4 uA) — unlike the chip's sleep
    // mode, which freezes gauging entirely. This product uses permanent hibernate so SOC/CRATE
    // stay valid across the MCU's deep-sleep cycles.
    bool enableHibernate();              // HIBRT = 0xFFFF: hibernate always

    bool setVoltageAlerts(float vmin, float vmax); // latches STATUS_VL/VH; 20 mV steps
    bool setSOCAlertThreshold(uint8_t percent);    // 1..32%; latches STATUS_HD

    bool quickstart();

private:
    i2c_master_dev_handle_t dev_handle;
    bool initialized;
};
