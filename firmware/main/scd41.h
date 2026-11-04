#pragma once

#include <cstddef>
#include <stdint.h>
#include "driver/i2c_master.h"

// Sensirion SCD41, driven the way Sensirion documents it: SCD4x datasheet v1.7 (April 2025) and the
// application note "SCD4x Low Power Operation" v1.0. Command codes and execution times are Table 9
// of the datasheet; Sensirion's own driver (github.com/Sensirion/embedded-i2c-scd4x, scd4x_i2c.c)
// uses the same codes and waits.
//
// This device runs the sensor in power-cycled single shot mode (datasheet §3.11, app note §2.4):
// wake_up, measure_single_shot, wait 5000 ms, read_measurement, power_down. Sensirion recommends
// it for sampling periods above 380 s. Automatic self-calibration (ASC) is not available in this
// mode (datasheet §3.11); the baseline is corrected with a forced recalibration (FRC).
class SCD41 {
public:
    // measure_single_shot, max. command duration (Table 9).
    static constexpr uint32_t SINGLE_SHOT_MS = 5000;
    // Readings a minute apart before a forced recalibration. The datasheet asks for more than three
    // single shots at a 1-minute interval (§3.8.1), the app note for five minutes at a 1-minute
    // sampling period in power-cycled mode (§5.1). Six readings span five minutes: both are met.
    static constexpr int      FRC_RUNUP_SHOTS = 6;
    static constexpr uint32_t FRC_RUNUP_PERIOD_MS = 60000;

    SCD41() : dev_handle(nullptr), initialized(false) {}
    ~SCD41();
    SCD41(const SCD41 &) = delete;
    SCD41 &operator=(const SCD41 &) = delete;

    bool init(i2c_master_bus_handle_t bus_handle);

    // wake_up (0x36f6, 30 ms; the sensor does not acknowledge it), then confirm the idle state by
    // reading the serial number (§3.11.4). A sensor left in periodic mode does not answer that read
    // (Table 9: not allowed during measurement); then Sensirion's clean-state sequence from their
    // example follows — stop_periodic_measurement, reinit — and the serial number is read again.
    // True when the sensor is idle and ready for commands.
    bool wakeToIdle();
    bool wakeUp();
    bool powerDown();                    // 0x36e0, 1 ms (§3.11.3)

    bool startPeriodicMeasurement();     // 0x21b1
    bool stopPeriodicMeasurement();      // 0x3f86; the sensor answers again after 500 ms
    bool measureSingleShot();            // 0x219d; read after SINGLE_SHOT_MS

    bool isDataReady(bool &ready);       // 0xe4b8
    bool readMeasurement(uint16_t &co2, float &temperature, float &humidity);   // 0xec05

    // Ambient pressure for CO2 compensation (0xe000, §3.7.5). Valid 70 000–120 000 Pa; outside that
    // nothing is sent and false is returned. Held in RAM, so it is sent before every single shot.
    bool setAmbientPressure(float pressure_pa);

    // Temperature offset (0x2318 / 0x241d, §3.7.1), in °C, recommended 0–20 °C. The sensor subtracts
    // it from its raw temperature and derives humidity from the result. Idle state only. Held in RAM
    // until persistSettings(), and power_down counts as a power cycle.
    bool getTemperatureOffset(float &celsius);
    bool setTemperatureOffset(float celsius);
    bool persistSettings();              // 0x3615, 800 ms; EEPROM rated for 2000 writes (§3.10.1)

    // Forced recalibration (§3.8.1): stop_periodic_measurement, 500 ms, perform_forced_recalibration,
    // 400 ms, read the correction. The caller takes the FRC_RUNUP_SHOTS readings first. The sensor
    // stores the result itself (§3.10.1), so no persistSettings() follows. False when it returns
    // 0xffff.
    bool performForcedRecalibration(uint16_t target_ppm, int16_t &correction);

    bool reinit();                       // 0x3646, 30 ms: reload settings from EEPROM (§3.10.5)
    bool getSerial(uint64_t &serial);    // 0x3682

    // Bench only. perform_factory_reset erases the FRC and ASC history (§3.10.4), and
    // perform_self_test is an end-of-line test (§3.10.3); the device cycle uses neither.
    bool factoryReset();                 // 0x3632, 1200 ms
    bool selfTest(uint16_t &result);     // 0x3639, 10 000 ms

private:
    bool sendCommand(uint16_t command);
    bool readWords(uint16_t command, uint8_t *data, size_t len, uint32_t exec_ms);
    uint8_t calculateCRC(uint8_t* data, uint8_t len);

    i2c_master_dev_handle_t dev_handle;
    bool initialized;
};
