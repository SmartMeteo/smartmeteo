#pragma once

// Bosch's BSEC library is closed and its licence (December 2024, §3.1) forbids passing on the
// library, its headers or its configuration files — so the public source cannot carry them. This
// header therefore names nothing of Bosch's: every BSEC type lives in bsec_manager.cpp, and a build
// without components/bsec2 compiles bsec_manager_none.cpp in its place (main/CMakeLists.txt picks).
// Nothing else in the firmware may include bsec_interface.h or bsec_datatypes.h — t_console checks.
#include <stdint.h>

struct BSECOutputs {
    float iaq;                     // Indoor air quality index (0-500), adapting baseline
    float static_iaq;              // Unscaled IAQ, fixed baseline (better for outdoor/absolute)
    float co2_equivalent;          // Estimated CO2 equivalent [ppm]
    float breath_voc_equivalent;   // Estimated breath-VOC concentration [ppm]
    float gas_percentage;          // Gas signal as % of filtered min/max window
    float raw_gas;                 // Raw gas resistance passed through BSEC [Ohm]
    uint8_t accuracy;              // IAQ accuracy / calibration state (0=unreliable .. 3=high)
    float temperature;             // Heat-compensated temperature [C]
    float humidity;                // Heat-compensated relative humidity [%]
    float stabilization_status;    // 0 = still stabilizing after power-on, 1 = stabilized
    float run_in_status;           // 0 = still running in, 1 = run-in complete
};

// Sensor settings BSEC asks the host to apply BEFORE each measurement.
// This is how the closed loop works: BSEC dictates heater + oversampling, the
// BME690 measures with exactly those, then the result is fed back to BSEC.
struct BSECSensorRequest {
    bool trigger;          // true = BSEC wants a measurement this cycle
    uint16_t heater_temp;  // heater target temperature [C]
    uint16_t heater_dur;   // heater duration [ms]
    uint8_t os_temp;       // temperature oversampling (BME69X_OS_* 0..5)
    uint8_t os_pres;       // pressure oversampling
    uint8_t os_hum;        // humidity oversampling
    bool run_gas;          // whether the gas heater should run this measurement
    int32_t process_data;  // bitmask of the inputs BSEC wants fed back (BSEC_CHECK_INPUT).
                           // Feeding a set BSEC did not ask for makes bsec_do_steps return
                           // OK with ZERO outputs — silently, so IAQ/accuracy read as 0.
    int64_t next_call_ns;  // when BSEC wants bsec_sensor_control called again (AN011 p.11: the
                           // framework schedules the next call from this). Observed on this
                           // device at ULP: ~3 s after the first trigger of a period, when the
                           // heater profile has a second step, then ~300 s — so the caller keeps
                           // calling until outputs appear.
};

class BSECManager {
public:
    static BSECManager& getInstance();

    // Is Bosch's algorithm in this build at all? False only in a build made without
    // components/bsec2: there is then no air-quality index to wait for, and the device says so
    // instead of «Warming up» for ever. The barometer still works — the cycle reads the BME690
    // plainly — because the rain forecast must not depend on a library someone could not get.
    static bool builtIn();

    // One mode only. The 3 s LP config existed to calibrate quickly on the cable, and it was
    // dropped once the ULP pipeline was fixed: accuracy now reaches 2 in about forty minutes
    // on battery, so a second config, a second saved state and a second set of code paths
    // bought nothing and had to be kept correct for ever (4.5).
    // false = 300 s ULP (battery). LP and ULP states are NOT interchangeable, so
    // persisted state is tagged with the mode and ignored on a mismatch.
    bool init();
    // Step 1 of the loop: ask BSEC what to measure now (heater + oversampling).
    // Returns true if BSEC wants a measurement at this timestamp (req.trigger).
    bool getSensorControl(int64_t timestamp_ns, BSECSensorRequest &req);

    // Step 2 of the loop: feed the measurement (taken with req's settings) back
    // to BSEC and read the processed outputs.
    bool processData(int64_t timestamp_ns, const BSECSensorRequest &req,
                     float temp, float hum, float pres, float gas_res, bool gas_valid,
                     BSECOutputs &outputs);

    // BSEC state persistence. The full algorithm state must be carried across
    // EVERY restart/sleep or run-in never accumulates. loadState() prefers the
    // RTC copy (fresh, survives deep sleep + esp_restart, zero flash wear) and
    // falls back to NVS on a cold boot. saveState() snapshots to RTC every cycle;
    // saveStateNVS() mirrors that to NVS occasionally as a power-loss backup.
    bool loadState();
private:
    bool loadStateInner();
public:
    // How the state came back on this boot. The answer is decided in the first two seconds after
    // reset, which on this hardware is before the USB port finishes enumerating -- so the log line
    // that says it can never be read on a battery-cadence device. Kept here so a later, readable
    // line can report it.
    const char* loadSource() const { return load_src_; }
    // Survives wakes — see the note in the .cpp. Use THIS for anything a console or a hub reads.
    const char* lastLoadSource() const;
    // The bench gate. armLoadProbe() is called on the boot whose answer matters — the first after a
    // firmware change or a power loss — and the next loadState() latches its verdict permanently.
    // Without this the reading is whatever the most recent ordinary wake did, which is always "RTC".
    void        armLoadProbe();
    const char* gateSource() const;
    uint32_t    gateBlobLen() const;
    // Did the algorithm actually run on this wake? A calibration snapshot taken when it did not is
    // a snapshot of nothing, and writing one over a good backup is how 4.32 lost a real one.
    bool isRunning() const { return initialized && bsec_inst != nullptr; }
    bool saveState();      // -> RTC (call every cycle)
    bool saveStateNVS();
    // NB: there is deliberately no saveHandoff() here any more. Handing calibration to a new image
    // needs SIX keys written together — the state blob is meaningless beside a clock an hour older,
    // and the room's norm lives outside this class entirely. That job belongs to the cycle, which
    // owns all six, and it is done there by save_calibration_snapshot() at the moment the update is
    // armed (4.53). A method here that wrote only the blob was an invitation to get it wrong.
    static void invalidateRtcState();  // clear the RTC copy (called on cold boot)

private:
    BSECManager() : initialized(false), bsec_inst(nullptr) {}
    ~BSECManager() = default;
    BSECManager(const BSECManager&) = delete;
    BSECManager& operator=(const BSECManager&) = delete;

    bool initialized;
    const char* load_src_ = "not attempted";   // where this boot's state came from
    void* bsec_inst;   // BSEC 3.x instance buffer (allocated via bsec_get_instance_size)
    // The work buffer and the output readers moved into bsec_manager.cpp: they are typed by Bosch's
    // headers, and this one must not need them.
};
