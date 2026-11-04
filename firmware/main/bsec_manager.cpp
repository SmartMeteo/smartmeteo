#include <stdio.h>
#include "bsec_manager.h"
// The only file that sees Bosch's headers (bsec_manager.h explains why).
#include "bsec_interface.h"
#include "bsec_datatypes.h"
#include "evt_log.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "nvs_manager.h"
#include <string.h>
#include <stdlib.h>

// BSEC algorithm state kept in RTC memory. RTC_NOINIT_ATTR (not RTC_DATA_ATTR!)
// so it survives deep sleep AND esp_restart (the USB path restarts every cycle);
// only a full power loss clears it, with zero flash wear. This is the primary
// persistence layer — it MUST be carried every cycle or run-in never accumulates.
// On a cold boot it holds garbage; main.cpp calls invalidateRtcState() then, and
// loadState() also validates the length + lets bsec_set_state reject bad blobs.
RTC_NOINIT_ATTR static uint8_t  rtc_bsec_state[BSEC_MAX_STATE_BLOB_SIZE];
RTC_NOINIT_ATTR static uint32_t rtc_bsec_state_len;
RTC_NOINIT_ATTR static uint8_t  rtc_bsec_state_mode;  // 0=ULP, 1=LP (state is mode-specific)
// Where the state last came back from, kept across wakes on purpose. The answer is decided inside
// read_sensors(), which only runs on a MEASURING wake — while the console that has to report it is
// raised by a button press, on a wake that never touches BSEC. Reported live, the field read
// "not attempted" almost every time it was asked, which is exactly what it did during the first
// bench-gate run: the instrument answered about the wrong wake.
RTC_NOINIT_ATTR static char     rtc_bsec_src[24];   // longest is "RTC mode mismatch"
// The bench gate needs the decision made on ONE specific boot — the first after a firmware change
// or a power loss — and every ordinary wake afterwards legitimately loads from RTC and overwrites
// it. Twice tonight the instrument answered about the wrong wake. So the gate result is armed
// deliberately, written once, and then left alone until something arms it again.
RTC_NOINIT_ATTR static char     rtc_gate_src[24];
RTC_NOINIT_ATTR static uint32_t rtc_gate_len;     // bytes of state actually applied
RTC_NOINIT_ATTR static uint32_t rtc_gate_armed;
#define GATE_ARMED_MAGIC 0x6A7E0001u
RTC_NOINIT_ATTR static uint32_t rtc_bsec_src_magic;
#define BSEC_SRC_MAGIC 0x5C0DE111u

void BSECManager::invalidateRtcState() { rtc_bsec_state_len = 0; rtc_bsec_state_mode = 0xFF; }

bool BSECManager::builtIn() { return true; }

static const char* TAG = "BSEC_MGR";

// BSEC's scratch space for configuration and state. It was a member of the singleton; it lives here
// so the header does not need BSEC_MAX_WORKBUFFER_SIZE. Same size, same lifetime (static storage).
static uint8_t work_buffer[BSEC_MAX_WORKBUFFER_SIZE];

static float getSignalValue(const bsec_output_t *outputs, uint8_t n_outputs, uint8_t sensor_id);
static uint8_t getSignalAccuracy(const bsec_output_t *outputs, uint8_t n_outputs, uint8_t sensor_id);

// Same definition Bosch uses in bsec_integration.h: the sensor ids are 1-based, the mask
// bits are 0-based.
#define BSEC_CHECK_INPUT(x, shift) ((x) & (1 << ((shift) - 1)))

// Embed BSEC configuration binaries via target_add_binary_data.
// ULP (300 s, battery) and LP (3 s, USB — fast calibration).
extern const uint8_t bsec_config_start[] asm("_binary_bsec_iaq_config_start");
extern const uint8_t bsec_config_end[] asm("_binary_bsec_iaq_config_end");

BSECManager& BSECManager::getInstance() {
    static BSECManager instance;
    return instance;
}

bool BSECManager::init() {
    if (initialized) return true;
    ESP_LOGI(TAG, "BSEC init (ULP, 300 s)");

    // BSEC 3.x is instance-based: allocate the instance buffer once and pass it
    // as the first argument to every bsec_* call. The buffer holds all internal
    // state and must persist for the app lifetime (never freed on this singleton).
    if (bsec_inst == nullptr) {
        size_t inst_size = bsec_get_instance_size();
        bsec_inst = malloc(inst_size);
        if (bsec_inst == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate BSEC instance (%u bytes)", (unsigned int)inst_size);
            return false;
        }
        memset(bsec_inst, 0, inst_size);
        ESP_LOGI(TAG, "Allocated BSEC instance (%u bytes)", (unsigned int)inst_size);
    }

    bsec_library_return_t ret = bsec_init(bsec_inst);
    if (ret != BSEC_OK) {
        ESP_LOGE(TAG, "bsec_init failed: %d", ret);
        return false;
    }

    bsec_version_t version;
    bsec_get_version(bsec_inst, &version);
    ESP_LOGI(TAG, "BSEC library version: %d.%d.%d.%d", version.major, version.minor, version.major_bugfix, version.minor_bugfix);

    const uint8_t *cfg_start = bsec_config_start;
    const uint8_t *cfg_end   = bsec_config_end;
    uint32_t config_len = cfg_end - cfg_start;
    ESP_LOGI(TAG, "Loading BSEC config binary (%u bytes total, skipping 4-byte size header)...", (unsigned int)config_len);

    // Skip 4-byte size header at the beginning of the .config file
    const uint8_t *config_payload = cfg_start + 4;
    uint32_t payload_len = config_len - 4;

    if (payload_len >= 16) {
        ESP_LOGI(TAG, "Raw config payload header: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                 config_payload[0], config_payload[1], config_payload[2], config_payload[3],
                 config_payload[4], config_payload[5], config_payload[6], config_payload[7],
                 config_payload[8], config_payload[9], config_payload[10], config_payload[11],
                 config_payload[12], config_payload[13], config_payload[14], config_payload[15]);
    }

    ret = bsec_set_configuration(bsec_inst, config_payload, payload_len, work_buffer, sizeof(work_buffer));
    if (ret < BSEC_OK) {
        ESP_LOGE(TAG, "bsec_set_configuration failed: %d", ret);
        return false;
    } else if (ret > BSEC_OK) {
        ESP_LOGW(TAG, "bsec_set_configuration warning: %d", ret);
    }
    ESP_LOGI(TAG, "BSEC configuration loaded successfully.");

    // Bosch's order (bsec_interface.h; bsec_integration.c, bsec_iot_init): configuration, then the saved
    // state, then the subscription. Subscriptions are not part of the saved state (AN011 p.45), so they
    // are made on every start, after the state is in.
    loadState();

    // The IAQ outputs at the ULP rate — the set in Bosch's bsec_iot_example for bme690_iaq_33v_300s_4d.
    // BSEC_OUTPUT_BREATH_VOC_EQUIVALENT is not in it: this config does not provide it at ULP.
    const bsec_virtual_sensor_t wanted[] = {
        BSEC_OUTPUT_IAQ,
        BSEC_OUTPUT_STATIC_IAQ,
        BSEC_OUTPUT_CO2_EQUIVALENT,
        BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
        BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
        BSEC_OUTPUT_RAW_PRESSURE,
        BSEC_OUTPUT_RAW_TEMPERATURE,
        BSEC_OUTPUT_RAW_HUMIDITY,
        BSEC_OUTPUT_RAW_GAS,
        BSEC_OUTPUT_STABILIZATION_STATUS,
        BSEC_OUTPUT_RUN_IN_STATUS,
        BSEC_OUTPUT_GAS_PERCENTAGE,
    };
    const uint8_t n_wanted = sizeof(wanted) / sizeof(wanted[0]);
    bsec_sensor_configuration_t requested_virtual_sensors[sizeof(wanted) / sizeof(wanted[0])];
    bsec_sensor_configuration_t required_sensor_settings[BSEC_MAX_PHYSICAL_SENSOR];
    for (uint8_t i = 0; i < n_wanted; i++) {
        requested_virtual_sensors[i].sample_rate = BSEC_SAMPLE_RATE_ULP;
        requested_virtual_sensors[i].sensor_id = wanted[i];
    }
    uint8_t n_required_sensor_settings = BSEC_MAX_PHYSICAL_SENSOR;
    ret = bsec_update_subscription(bsec_inst, requested_virtual_sensors, n_wanted,
                                   required_sensor_settings, &n_required_sensor_settings);
    if (ret < BSEC_OK) {
        ESP_LOGE(TAG, "bsec_update_subscription failed: %d", ret);
        return false;
    }
    if (ret > BSEC_OK) {
        // AN011 p.45: a warning here means an output is unavailable and the request was ignored. Bosch's
        // example stops only on errors, so this carries on too, but says so and records it.
        ESP_LOGE(TAG, "bsec_update_subscription warning %d — outputs may be missing", ret);
        EvtLog::getInstance().event(EVT_GAS, GAS_WARN_SUBSCRIBE | (uint16_t)(int16_t)ret);
    }

    initialized = true;
    ESP_LOGI(TAG, "BSEC library initialized successfully.");
    
    return true;
}

bool BSECManager::getSensorControl(int64_t timestamp_ns, BSECSensorRequest &req) {
    req = {};
    if (!initialized) return false;

    bsec_bme_settings_t settings;
    bsec_library_return_t ret = bsec_sensor_control(bsec_inst, timestamp_ns, &settings);
    if (ret < BSEC_OK) {
        ESP_LOGE(TAG, "bsec_sensor_control failed: %d", ret);
        return false;
    }
    if (ret > BSEC_OK) {
        // Kept, not thrown away: the timing warning (AN011 p.26) is BSEC saying it was called off its
        // schedule, and nothing else on the device can tell.
        ESP_LOGW(TAG, "bsec_sensor_control warning: %d (t=%.1f s)", ret, timestamp_ns / 1e9);
        EvtLog::getInstance().event(EVT_GAS, GAS_WARN_CONTROL | (uint16_t)(int16_t)ret);
    }

    req.trigger     = (settings.trigger_measurement != 0);
    req.heater_temp = settings.heater_temperature;
    req.heater_dur  = settings.heater_duration;
    req.os_temp     = settings.temperature_oversampling;
    req.os_pres     = settings.pressure_oversampling;
    req.os_hum      = settings.humidity_oversampling;
    req.run_gas     = (settings.run_gas != 0);
    req.process_data = settings.process_data;
    req.next_call_ns = settings.next_call;

    if (!req.trigger) {
        ESP_LOGW(TAG, "BSEC does not want a measurement now. Next call in %lld ms",
                 (settings.next_call - timestamp_ns) / 1000000LL);
    } else {
        // next_call vs the timestamp we pass matters at ULP: each deep-sleep cycle hands
        // BSEC a slightly late timestamp (sleep + this cycle's awake time), so log the
        // schedule to see whether we are drifting off the 300 s grid BSEC expects.
        ESP_LOGI(TAG, "BSEC requests measurement: heater=%dC/%dms, os(T/P/H)=%d/%d/%d, run_gas=%d"
                 " | t=%.1f s, next_call=%.1f s (delta %+.1f s), op_mode=%d",
                 req.heater_temp, req.heater_dur, req.os_temp, req.os_pres, req.os_hum, req.run_gas,
                 timestamp_ns / 1e9, settings.next_call / 1e9,
                 (settings.next_call - timestamp_ns) / 1e9, (int)settings.op_mode);
    }
    return req.trigger;
}

bool BSECManager::processData(int64_t timestamp_ns, const BSECSensorRequest &req,
                              float temp, float hum, float pres, float gas_res, bool gas_valid,
                              BSECOutputs &outputs) {
    if (!initialized) return false;
    if (!req.trigger) return false; // BSEC didn't ask for a measurement this cycle

    // Feed EXACTLY the inputs BSEC asked for, no more and no less. bsec_sensor_control
    // hands back a `process_data` bitmask naming them, and BSEC is unforgiving here: give
    // it a set it did not request and bsec_do_steps returns BSEC_OK with ZERO outputs —
    // silently. The caller then keeps its zero-initialised struct and the screen shows
    // "IAQ 0.0 / acc 0/3", which looks exactly like a dead gas sensor. This mirrors
    // Bosch's reference processData() in bsec_integration.c.
    bsec_input_t inputs[BSEC_MAX_PHYSICAL_SENSOR];
    uint8_t n_inputs = 0;
    const int32_t pd = req.process_data;

    if (BSEC_CHECK_INPUT(pd, BSEC_INPUT_HEATSOURCE)) {
        // External heat-source offset. Our displayed climate comes from the SCD41, so we
        // do not apply an offset here and BSEC gets a clean 0.
        inputs[n_inputs++] = {
            .time_stamp = timestamp_ns,
            .signal = 0.0f,
            .signal_dimensions = 1,
            .sensor_id = BSEC_INPUT_HEATSOURCE
        };
    }
    if (BSEC_CHECK_INPUT(pd, BSEC_INPUT_TEMPERATURE)) {
        inputs[n_inputs++] = {
            .time_stamp = timestamp_ns,
            .signal = temp,
            .signal_dimensions = 1,
            .sensor_id = BSEC_INPUT_TEMPERATURE
        };
    }
    if (BSEC_CHECK_INPUT(pd, BSEC_INPUT_HUMIDITY)) {
        inputs[n_inputs++] = {
            .time_stamp = timestamp_ns,
            .signal = hum,
            .signal_dimensions = 1,
            .sensor_id = BSEC_INPUT_HUMIDITY
        };
    }
    if (BSEC_CHECK_INPUT(pd, BSEC_INPUT_PRESSURE)) {
        inputs[n_inputs++] = {
            .time_stamp = timestamp_ns,
            .signal = pres, // Already in Pa from the BME690 driver
            .signal_dimensions = 1,
            .sensor_id = BSEC_INPUT_PRESSURE
        };
    }
    // Gas inputs only with BME69X_GASM_VALID_MSK set, as in Bosch's process_data (bsec_integration.c).
    if (BSEC_CHECK_INPUT(pd, BSEC_INPUT_GASRESISTOR) && gas_valid) {
        inputs[n_inputs++] = {
            .time_stamp = timestamp_ns,
            .signal = gas_res,
            .signal_dimensions = 1,
            .sensor_id = BSEC_INPUT_GASRESISTOR
        };
    }
    if (BSEC_CHECK_INPUT(pd, BSEC_INPUT_PROFILE_PART) && gas_valid) {
        // Which step of the heater profile this sample belongs to. Always 0 in forced
        // mode (a single step); only scan mode walks a multi-step profile. We never sent
        // this input at all, which is the likeliest reason BSEC emitted nothing.
        inputs[n_inputs++] = {
            .time_stamp = timestamp_ns,
            .signal = 0.0f,
            .signal_dimensions = 1,
            .sensor_id = BSEC_INPUT_PROFILE_PART
        };
    }
    if (n_inputs == 0) return false;

    bsec_output_t bsec_outputs[BSEC_NUMBER_OUTPUTS];
    uint8_t n_outputs = BSEC_NUMBER_OUTPUTS;

    bsec_library_return_t ret = bsec_do_steps(bsec_inst, inputs, n_inputs, bsec_outputs, &n_outputs);
    if (ret < BSEC_OK) {
        ESP_LOGE(TAG, "bsec_do_steps failed: %d", ret);
        return false;
    } else if (ret > BSEC_OK) {
        ESP_LOGW(TAG, "bsec_do_steps warning: %d", ret);
        EvtLog::getInstance().event(EVT_GAS, GAS_WARN_STEPS | (uint16_t)(int16_t)ret);
    }

    if (n_outputs > 0) {
        outputs.iaq = getSignalValue(bsec_outputs, n_outputs, BSEC_OUTPUT_IAQ);
        outputs.static_iaq = getSignalValue(bsec_outputs, n_outputs, BSEC_OUTPUT_STATIC_IAQ);
        outputs.co2_equivalent = getSignalValue(bsec_outputs, n_outputs, BSEC_OUTPUT_CO2_EQUIVALENT);
        outputs.breath_voc_equivalent = getSignalValue(bsec_outputs, n_outputs, BSEC_OUTPUT_BREATH_VOC_EQUIVALENT);
        outputs.gas_percentage = getSignalValue(bsec_outputs, n_outputs, BSEC_OUTPUT_GAS_PERCENTAGE);
        outputs.raw_gas = getSignalValue(bsec_outputs, n_outputs, BSEC_OUTPUT_RAW_GAS);
        outputs.accuracy = getSignalAccuracy(bsec_outputs, n_outputs, BSEC_OUTPUT_IAQ);
        outputs.temperature = getSignalValue(bsec_outputs, n_outputs, BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE);
        outputs.humidity = getSignalValue(bsec_outputs, n_outputs, BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY);
        outputs.stabilization_status = getSignalValue(bsec_outputs, n_outputs, BSEC_OUTPUT_STABILIZATION_STATUS);
        outputs.run_in_status = getSignalValue(bsec_outputs, n_outputs, BSEC_OUTPUT_RUN_IN_STATUS);
        return true;
    }

    // bsec_do_steps can succeed and still emit nothing — the caller then keeps its
    // zero-initialised outputs, which reads on screen as "IAQ 0.0 / acc 0" and is easy to
    // mistake for "the sensor is broken". Say so explicitly instead.
    ESP_LOGW(TAG, "bsec_do_steps OK but produced 0 outputs (t=%.1f s, n_inputs=%d, run_gas=%d, gas=%.0f Ohm)",
             timestamp_ns / 1e9, (int)n_inputs, (int)req.run_gas, gas_res);
    return false;
}

// The last real answer, whichever wake produced it. Falls back to the live field only when RTC
// holds nothing trustworthy — a cold start, or a firmware whose layout moved.
const char* BSECManager::lastLoadSource() const {
    if (rtc_bsec_src_magic == BSEC_SRC_MAGIC && rtc_bsec_src[0]) return rtc_bsec_src;
    return load_src_;
}

void BSECManager::armLoadProbe() {
    rtc_gate_armed = GATE_ARMED_MAGIC;
    rtc_gate_src[0] = '\0';
    rtc_gate_len = 0;
}
const char* BSECManager::gateSource() const {
    return (rtc_gate_armed == 0 && rtc_gate_src[0]) ? rtc_gate_src : "(not armed)";
}
uint32_t BSECManager::gateBlobLen() const { return rtc_gate_len; }

bool BSECManager::loadState() {
    const bool armed = (rtc_gate_armed == GATE_ARMED_MAGIC);
    const bool ok = loadStateInner();
    if (armed) {
        snprintf(rtc_gate_src, sizeof(rtc_gate_src), "%s", load_src_);
        rtc_gate_len = rtc_bsec_state_len;      // what ended up applied, whichever source won
        rtc_gate_armed = 0;                     // written once; ordinary wakes must not touch it
    }
    return ok;
}

bool BSECManager::loadStateInner() {
    const uint8_t mode = 0;   // ULP, the only one
    const char* nvs_key = "bsec_state";

    // Prefer the RTC copy (fresh — written every cycle, survives sleep + restart),
    // but only if it was saved in the SAME mode (LP and ULP states are not compatible).
    if (rtc_bsec_state_len > 0 && rtc_bsec_state_len <= sizeof(rtc_bsec_state) && rtc_bsec_state_mode == mode) {
        bsec_library_return_t ret = bsec_set_state(bsec_inst, rtc_bsec_state, rtc_bsec_state_len,
                                                   work_buffer, sizeof(work_buffer));
        if (ret == BSEC_OK) {
            ESP_LOGI(TAG, "Loaded BSEC state from RTC (%u bytes, %s).", (unsigned)rtc_bsec_state_len,
                     "ULP");
            load_src_ = "RTC"; snprintf(rtc_bsec_src, sizeof(rtc_bsec_src), "RTC"); rtc_bsec_src_magic = BSEC_SRC_MAGIC;
            return true;
        }
        ESP_LOGW(TAG, "Failed to apply RTC BSEC state: %d (falling back to NVS)", ret);
        load_src_ = "RTC rejected"; snprintf(rtc_bsec_src, sizeof(rtc_bsec_src), "RTC rejected"); rtc_bsec_src_magic = BSEC_SRC_MAGIC;
    } else if (rtc_bsec_state_len > 0 && rtc_bsec_state_mode != mode) {
        ESP_LOGI(TAG, "RTC BSEC state is other mode — starting fresh for %s.", "ULP");
        load_src_ = "RTC mode mismatch"; snprintf(rtc_bsec_src, sizeof(rtc_bsec_src), "RTC mode mismatch"); rtc_bsec_src_magic = BSEC_SRC_MAGIC;
    }

    // Cold boot (power loss cleared RTC): fall back to the mode-specific NVS backup.
    uint8_t state_blob[BSEC_MAX_STATE_BLOB_SIZE];
    size_t state_len = sizeof(state_blob);
    if (NVSManager::getInstance().getBlob(nvs_key, state_blob, state_len)) {
        bsec_library_return_t ret = bsec_set_state(bsec_inst, state_blob, state_len, work_buffer, sizeof(work_buffer));
        if (ret == BSEC_OK) {
            memcpy(rtc_bsec_state, state_blob, state_len);   // seed the RTC copy
            rtc_bsec_state_len = state_len;
            rtc_bsec_state_mode = mode;
            ESP_LOGI(TAG, "Loaded BSEC state from NVS backup (%u bytes, %s).", (unsigned)state_len,
                     "ULP");
            load_src_ = "NVS backup"; snprintf(rtc_bsec_src, sizeof(rtc_bsec_src), "NVS backup"); rtc_bsec_src_magic = BSEC_SRC_MAGIC;
            return true;
        }
        ESP_LOGW(TAG, "Failed to apply NVS BSEC state blob: %d", ret);
        load_src_ = "NVS rejected"; snprintf(rtc_bsec_src, sizeof(rtc_bsec_src), "NVS rejected"); rtc_bsec_src_magic = BSEC_SRC_MAGIC;
    } else {
        ESP_LOGI(TAG, "No BSEC state found (fresh start).");
        load_src_ = "none stored"; snprintf(rtc_bsec_src, sizeof(rtc_bsec_src), "none stored"); rtc_bsec_src_magic = BSEC_SRC_MAGIC;
    }
    return false;
}

// Snapshot the live BSEC state into RTC. Cheap, zero flash wear — call EVERY cycle
// so the algorithm's run-in/accuracy persists across restarts and deep sleep.
bool BSECManager::saveState() {
    if (!initialized) return false;
    uint32_t state_len = 0;
    bsec_library_return_t ret = bsec_get_state(bsec_inst, 0, rtc_bsec_state, sizeof(rtc_bsec_state),
                                               work_buffer, sizeof(work_buffer), &state_len);
    if (ret == BSEC_OK && state_len > 0) {
        rtc_bsec_state_len = state_len;
        rtc_bsec_state_mode = 0;
        return true;
    }
    ESP_LOGE(TAG, "bsec_get_state failed: %d", ret);
    return false;
}

// Mirror the RTC copy to NVS as a power-loss backup. Call sparingly (NVS wear).
bool BSECManager::saveStateNVS() {
    if (rtc_bsec_state_len == 0) return false;
    const char* nvs_key = "bsec_state";
    if (NVSManager::getInstance().setBlob(nvs_key, rtc_bsec_state, rtc_bsec_state_len)) {
        ESP_LOGI(TAG, "Backed up BSEC state to NVS (%u bytes).", (unsigned)rtc_bsec_state_len);
        return true;
    }
    ESP_LOGE(TAG, "Failed to back up BSEC state to NVS.");
    return false;
}


static float getSignalValue(const bsec_output_t *outputs, uint8_t n_outputs, uint8_t sensor_id) {
    for (uint8_t i = 0; i < n_outputs; i++) {
        if (outputs[i].sensor_id == sensor_id) {
            return outputs[i].signal;
        }
    }
    return 0.0f;
}

static uint8_t getSignalAccuracy(const bsec_output_t *outputs, uint8_t n_outputs, uint8_t sensor_id) {
    for (uint8_t i = 0; i < n_outputs; i++) {
        if (outputs[i].sensor_id == sensor_id) {
            return outputs[i].accuracy;
        }
    }
    return 0;
}
