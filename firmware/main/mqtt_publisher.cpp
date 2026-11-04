#include "mqtt_publisher.h"
#include "bsec_manager.h"
#include "evt_log.h"
#include "power_history.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_mac.h"
#include "esp_crt_bundle.h"
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "ArduinoJson.h"
#include <stdio.h>

static const char *TAG = "MQTT";

// Readings live in telemetry.cpp; the cycle refreshes them on every wake.
extern uint16_t global_co2;
extern float global_s_temp, global_s_hum, global_b_pres;
extern float global_iaq, global_static_iaq, global_dewpoint;
extern uint8_t global_iaq_accuracy;
extern int global_pollution_class;
extern std::string global_pollution_text;
extern float global_soc, global_soc_raw, global_voltage;
extern uint32_t global_btn_noise;
extern bool global_on_usb;
// The finished words of the last event, composed by the cycle. Carried verbatim so the hub says
// exactly what the device decided — see the note beside rtc_al_title in bench_screens.cpp.
extern const char *alert_title();
extern const char *alert_text();
extern const char *alert_level();
extern bool device_is_night();
// The shape of the night, computed at dawn from the log the device kept while its radio was off.
extern uint16_t night_co2_max();
extern uint16_t night_co2_avg();
extern float    night_t_min();
extern float    night_t_max();
extern float    night_h_max();
extern bool     night_valid();

#define BIT_CONNECTED  BIT0
#define BIT_PUBLISHED  BIT1
#define BIT_FAILED     BIT2
#define BIT_DATA       BIT3

static EventGroupHandle_t s_bits = nullptr;
// Where an incoming retained message lands. One slot is enough: the device subscribes to exactly
// one topic, reads it, and disconnects.
static std::string s_rx;

// Message ids the broker has acknowledged in this session. MQTT_EVENT_PUBLISHED carries the msg_id that
// esp_mqtt_client_publish() returned for the message (mqtt_client.h), so a wait for one message is
// matched against its own id, not against whichever acknowledgement arrives first. Written by the MQTT
// task, read by the publishing task; 32 covers the discovery burst, the state and the log.
#define ACKED_SLOTS 32
static int      s_acked[ACKED_SLOTS];
static unsigned s_acked_next = 0;

static void mqtt_event_handler(void *arg, esp_event_base_t, int32_t id, void *data)
{
    auto *ev = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            xEventGroupSetBits(s_bits, BIT_CONNECTED);
            break;
        case MQTT_EVENT_PUBLISHED:
            if (ev) { s_acked[s_acked_next % ACKED_SLOTS] = ev->msg_id; s_acked_next++; }
            xEventGroupSetBits(s_bits, BIT_PUBLISHED);
            break;
        case MQTT_EVENT_DATA:
            // A message longer than the client's receive buffer (1 KB by default) arrives as several
            // DATA events, each a slice carrying its offset into the whole. Treating every slice as
            // the whole message kept only whichever came last, and woke the reader on the first —
            // a JSON document cut part-way. The weather message grows with the outlook in it, so it
            // is assembled here and announced only once its last byte is in.
            if (ev && ev->data_len > 0) {
                if (ev->current_data_offset == 0) s_rx.clear();
                if (ev->total_data_len > 4096) break;          // not a weather message; leave it
                s_rx.append(ev->data, ev->data_len);
                if (ev->current_data_offset + ev->data_len >= ev->total_data_len)
                    xEventGroupSetBits(s_bits, BIT_DATA);
            }
            break;
        case MQTT_EVENT_ERROR:
            // Worth naming: "connection refused" from a broker that wants credentials looks
            // identical to a wrong address unless the reason code is printed.
            if (ev && ev->error_handle) {
                ESP_LOGE(TAG, "error: type %d, connect return code %d, transport errno %d",
                         (int)ev->error_handle->error_type,
                         (int)ev->error_handle->connect_return_code,
                         (int)ev->error_handle->esp_transport_sock_errno);
            }
            xEventGroupSetBits(s_bits, BIT_FAILED);
            break;
        case MQTT_EVENT_DISCONNECTED:
            xEventGroupSetBits(s_bits, BIT_FAILED);
            break;
        default:
            break;
    }
}

static void acked_reset(void)
{
    for (int &m : s_acked) m = -1;
    s_acked_next = 0;
}

// Wait for the broker's PUBACK of message `mid`; false at the timeout or on a disconnect.
static bool wait_puback(int mid, int timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t span  = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        for (int m : s_acked) if (m == mid) return true;
        const TickType_t used = xTaskGetTickCount() - start;
        if (used >= span) return false;
        const EventBits_t b = xEventGroupWaitBits(s_bits, BIT_PUBLISHED | BIT_FAILED, pdTRUE, pdFALSE,
                                                  span - used);
        if (b & BIT_FAILED) {
            for (int m : s_acked) if (m == mid) return true;
            return false;
        }
    }
}

// What this device calls itself: the name set in the console, or the product name.
static std::string device_label(void)
{
    std::string name;
    NVSManager::getInstance().getString("dev_name", name, "");
    return name.empty() ? std::string("Smart Meteo") : name;
}

// Rounding that stays DEFINED when the value is absurd, for the three estimate fields and nothing
// else — the rest of this payload carries temperatures and percentages, which cannot overflow an
// int. A float-to-int conversion whose result does not fit is undefined behaviour, and on this
// target it saturates quietly: the day the estimator fitted a flat series, that line turned an
// unbounded runtime into 2147483647 days, and Home Assistant drew it as a number because that is
// what it was sent (journal 2026-10-13).
//
// The cap is deliberately ABSURD — a century, not a believable ceiling. A plausible cap would hide
// the next arithmetic fault behind a figure nobody questions; a century of runtime still reads as a
// defect, and it is in range, which is all the conversion needs. The fit has a floor of its own now
// (PowerEstimate::MIN_DROP_PCT); this is the second fence, standing where the conversion happens.
static double rounded_fact(double v, double scale, double cap)
{
    if (!(v > 0.0)) return 0.0;            // catches NaN too: it fails every comparison
    if (v > cap) v = cap;
    return (double)(long long)(v * scale + 0.5) / scale;
}

std::string MqttPublisher::buildState()
{
    JsonDocument d;
    const std::string name = device_label();
    d["name"] = name.c_str();

    d["co2"] = global_co2;
    d["temperature"] = (int)(global_s_temp * 10 + (global_s_temp < 0 ? -0.5f : 0.5f)) / 10.0f;
    d["humidity"] = (int)(global_s_hum + 0.5f);
    // Pressure is omitted rather than sent as zero when the barometer has no usable reading:
    // a missing value is honest, a zero would be graphed as a pressure of nothing.
    if (global_b_pres > 30000.0f && global_b_pres < 110000.0f)
        d["pressure"] = (int)(global_b_pres / 10.0f + 0.5f) / 10.0f;   // hPa, one decimal
    d["dewpoint"] = (int)(global_dewpoint * 10 + 0.5f) / 10.0f;

    // A build without Bosch's BSEC has no index at all: the fields are left out rather than sent
    // as zeros, which the hub would graph as perfectly clean air.
    if (BSECManager::builtIn()) {
        d["iaq"] = (int)(global_iaq + 0.5f);
        d["iaq_static"] = (int)(global_static_iaq + 0.5f);
        d["iaq_accuracy"] = global_iaq_accuracy;
    }
    d["air_class"] = global_pollution_class;
    d["air_text"] = global_pollution_text.c_str();

    // The event, in the words the owner will read. Empty when there is nothing to say, which is
    // what keeps the hub from re-announcing an alert it has already passed on.
    d["alert_title"] = alert_title();
    d["alert_text"]  = alert_text();
    d["alert_level"] = alert_level();
    // Night is published because the hub cannot otherwise tell a sleeping device from a broken
    // one: the radio is off all night in every profile, and a rule watching for silence would
    // wake the owner every single night to report it.
    d["night"] = device_is_night();
    // Published every window once computed, not only on the morning it was made: a hub that was
    // restarting at dawn would otherwise miss the one message that described the night.
    if (night_valid()) {
        d["night_co2_max"] = night_co2_max();
        d["night_co2_avg"] = night_co2_avg();
        d["night_t_min"]   = (int)(night_t_min() * 10 + 0.5f) / 10.0f;
        d["night_t_max"]   = (int)(night_t_max() * 10 + 0.5f) / 10.0f;
        d["night_hum_max"] = (int)(night_h_max() + 0.5f);
    }

    d["battery"] = (int)(global_soc + 0.5f);
    // ...and what the cell itself measures, beside it. The two part company on purpose for the span
    // of the glide after a charge (battery_display.h), and until 2026-10-19 the difference was
    // visible nowhere outside the console: on the field board the glass said 67 while its own volts
    // said 74.5, and the only way to see that was to read the voltage and work the cell's table by
    // hand. A figure the owner cannot audit is a figure they cannot trust.
    d["battery_measured"] = (int)(global_soc_raw + 0.5f);
    d["voltage"] = (int)(global_voltage * 1000 + 0.5f) / 1000.0f;
    // The battery estimate — the console's, from the same fit (power_history.h). The numbers are
    // OMITTED, not zeroed, until the charge has fallen by more than the gauge can resolve: Home
    // Assistant used to compute its own from whole percentages and showed "0.174 mA, 392 days" for
    // a device drawing about 0.43. The word is always sent, so the hub can say WHY there is no
    // number yet rather than show a blank. Time to empty is withheld on the cable, as on the card.
    {
        const PowerEstimate::Result pe = PowerHistory::estimate();
        const PowerEstimate::Card pc = PowerEstimate::card(pe, global_soc, global_on_usb);
        d["draw_quality"] = PowerHistory::quality(pe);
        if (pc.mah_day > 0.0f) {
            d["draw_ma"]     = rounded_fact(pe.ua / 1000.0, 1000.0, 100000.0);   // mA, 3 decimals
            d["life_months"] = rounded_fact(pe.months, 10.0, 12000.0);           // on a full charge
            if (pc.tte_h > 0.0f)
                d["days_left"] = (int)rounded_fact(pc.tte_h / 24.0, 1.0, 36500.0);
        }
    }
    // Only when there is something to report: a field that is always zero trains the
    // reader to ignore it, and every byte here is radio time on a battery.
    if (global_btn_noise) d["btn_noise"] = (int)global_btn_noise;
    // Where the gas calibration came back from on this boot: "RTC", "NVS backup", "none stored".
    // Published because the one path that has never been observed working — the flash backup after
    // a power loss or a firmware change — is the path a customer takes on EVERY update, and on a
    // battery device there is no serial port to read the answer from. This makes the bench gate
    // (4.44) readable from the hub without touching the device at all.
    d["gas_src"] = BSECManager::getInstance().lastLoadSource();
    d["on_usb"] = global_on_usb;

    // How the device has actually behaved over the trailing 24 hours. Four numbers, because the
    // storm of 2026-08-29 — 187 wakes out of 207 from a noisy pin, the panel driven every 4.1 s
    // against a vendor minimum of 180 — was spotted by a person looking at the glass. Nothing on
    // the device or in the hub said anything was wrong, and nothing would have: every reading it
    // published stayed perfectly sensible while it wore the panel out and drained the cell.
    //
    // Omitted entirely, not sent as zero, until the window is long enough to divide by. A freshly
    // flashed device would otherwise announce a wildly wrong rate and then quietly correct it,
    // which is exactly how a diagnostic teaches its reader to stop believing it.
    {
        const EvtLog::Health h = EvtLog::getInstance().health();
        if (h.valid) {
            d["wakes_day"]  = (uint32_t)((uint64_t)h.wakes * 86400 / h.span_s);
            d["screen_day"] = (uint32_t)((uint64_t)h.draws * 86400 / h.span_s);
            d["edges_day"]  = (uint32_t)((uint64_t)h.edges * 86400 / h.span_s);
            // Awake time as a percentage, one decimal: the single number that summarises the
            // energy budget, and the one that moves first when anything starts waking the device
            // that should not. NB it reads low during a console session — the per-record awake
            // field saturates at 65.5 s — which is the harmless direction for an alarm.
            d["duty_pct"] = (int)((double)h.awake_ms / (double)h.span_s + 0.5) / 10.0;
        }
    }

    std::string out;
    serializeJson(d, out);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Home Assistant discovery.
//
// Instead of the owner describing eleven sensors in a configuration file, the device describes
// itself: one retained message per reading on homeassistant/<kind>/<id>/config, and the hub builds
// the device on its own. That difference is the whole point — a customer must not have to edit YAML
// to see their own thermometer, and the description belongs with the firmware that produces the
// data anyway, where it cannot drift out of step with it.
//
// The keys are the abbreviated forms Home Assistant accepts (stat_t, val_tpl, dev_cla ...). They
// look cryptic, but they roughly halve the bytes, and every byte here is radio time on a battery.
// ---------------------------------------------------------------------------------------------
static void emit(esp_mqtt_client_handle_t cl, const char *kind, const char *uid_base,
                 const char *slug, const char *name, const char *field, const char *unit,
                 const char *dev_cla, const char *state_topic, const JsonDocument &dev,
                 const char *icon = nullptr, bool diagnostic = false, bool measurement = true)
{
    JsonDocument d;
    d["name"] = name;
    d["uniq_id"] = std::string(uid_base) + "_" + slug;
    // No obj_id here, deliberately. Sending one looks like it should pin the entity id, and it
    // does not: when an entity belongs to a device, Home Assistant builds the id from the DEVICE
    // name plus the entity name and ignores the hint. Verified on hardware — the field was in the
    // payload and the entities still came out as sensor.test1_*. Every MQTT integration behaves
    // this way, ESPHome and Tasmota included, so a dashboard shipped with fixed entity ids is not
    // achievable from this side; it belongs with the installation, not the firmware.
    d["stat_t"] = state_topic;
    // Absent fields must not become zero: a device with a dead barometer sends no pressure at all,
    // and "none" leaves the entity unknown rather than claiming an atmosphere of nothing.
    d["val_tpl"] = std::string("{{ value_json.") + field + " | default(none) }}";
    if (unit)    d["unit_of_meas"] = unit;
    if (dev_cla) d["dev_cla"] = dev_cla;
    if (measurement) d["stat_cla"] = "measurement";
    if (icon)    d["ic"] = icon;
    if (diagnostic) d["ent_cat"] = "diagnostic";
    d["dev"] = dev;

    char topic[160];
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s_%s/config", kind, uid_base, slug);
    std::string body; serializeJson(d, body);
    esp_mqtt_client_publish(cl, topic, body.c_str(), body.size(), 1, 1);   // retained
}

std::string MqttPublisher::deviceId()
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char uid[32];
    snprintf(uid, sizeof(uid), "smartmeteo_%02x%02x%02x", mac[3], mac[4], mac[5]);
    return uid;
}

static void publishDiscovery(esp_mqtt_client_handle_t cl, const char *state_topic)
{
    const std::string uid_s = MqttPublisher::deviceId();
    const char *uid = uid_s.c_str();

    const std::string name = device_label();

    JsonDocument dev;
    dev["ids"][0] = uid;
    dev["name"] = name.c_str();
    dev["mf"] = "Smart Meteo";
    dev["mdl"] = "ESP32-C6";

    emit(cl, "sensor", uid, "co2", "CO2", "co2", "ppm", "carbon_dioxide", state_topic, dev);
    emit(cl, "sensor", uid, "temperature", "Temperature", "temperature", "\xC2\xB0""C", "temperature", state_topic, dev);
    emit(cl, "sensor", uid, "humidity", "Humidity", "humidity", "%", "humidity", state_topic, dev);
    emit(cl, "sensor", uid, "dewpoint", "Dew point", "dewpoint", "\xC2\xB0""C", "temperature", state_topic, dev);
    // Without BSEC the index and its calibration do not exist, so they are not announced: an entity
    // that is «unknown» for ever reads as a broken sensor. The verdict («air») stays — Stuffy and
    // Damp come from the SCD41 and work in every build.
    const bool gas_index = BSECManager::builtIn();
    if (gas_index)
        emit(cl, "sensor", uid, "iaq", "Air quality index", "iaq", nullptr, nullptr, state_topic, dev,
             "mdi:air-filter");
    emit(cl, "sensor", uid, "air", "Air quality", "air_text", nullptr, nullptr, state_topic, dev,
         "mdi:weather-windy", false, false);
    if (gas_index)
        emit(cl, "sensor", uid, "cal", "Gas sensor calibration", "iaq_accuracy", nullptr, nullptr,
             state_topic, dev, "mdi:progress-check", true, false);
    // The alert itself, as an entity: its state is the headline, its attributes carry the full
    // text. A hub rule then forwards what the device wrote instead of re-deciding what counts as
    // an event — which is how the two ended up disagreeing about the same air.
    {
        JsonDocument d;
        d["name"] = "Alert";
        d["uniq_id"] = std::string(uid) + "_alert";
        d["stat_t"] = state_topic;
        d["val_tpl"] = "{{ value_json.alert_title | default('') }}";
        d["json_attr_t"] = state_topic;
        d["json_attr_tpl"] =
            "{{ {'text': value_json.alert_text, 'level': value_json.alert_level} | tojson }}";
        d["ic"] = "mdi:bell-alert";
        d["dev"] = dev;
        char topic[160];
        snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_alert/config", uid);
        std::string body; serializeJson(d, body);
        esp_mqtt_client_publish(cl, topic, body.c_str(), body.size(), 1, 1);
    }
    // The night, as two entities worth having on a wall. The peak is the one that matters in a
    // bedroom — an average of 900 hides an hour at 1600 with the door shut.
    emit(cl, "sensor", uid, "nco2max", "CO2 overnight, peak", "night_co2_max", "ppm",
         "carbon_dioxide", state_topic, dev, nullptr, false, false);
    emit(cl, "sensor", uid, "nco2avg", "CO2 overnight, average", "night_co2_avg", "ppm",
         "carbon_dioxide", state_topic, dev, nullptr, false, false);
    emit(cl, "sensor", uid, "battery", "Battery", "battery", "%", "battery", state_topic, dev);
    // Diagnostic, not a headline: the owner's battery figure is the one above, and this is here so
    // the two can be compared when they disagree (see the state payload).
    emit(cl, "sensor", uid, "battmeas", "Battery (measured)", "battery_measured", "%", "battery",
         state_topic, dev, nullptr, true);
    emit(cl, "sensor", uid, "voltage", "Battery voltage", "voltage", "V", "voltage", state_topic,
         dev, nullptr, true);
    // The battery estimate (see buildState). Current in mA, because Home Assistant's "current"
    // class accepts A and mA and nothing smaller; days as a duration; months have no class at all.
    emit(cl, "sensor", uid, "draw", "Battery draw", "draw_ma", "mA", "current", state_topic, dev,
         "mdi:current-dc");
    emit(cl, "sensor", uid, "daysleft", "Battery days left", "days_left", "d", "duration",
         state_topic, dev, "mdi:battery-clock");
    emit(cl, "sensor", uid, "life", "Battery life on a full charge", "life_months", "mo", nullptr,
         state_topic, dev, "mdi:calendar-clock");
    emit(cl, "sensor", uid, "drawq", "Battery estimate", "draw_quality", nullptr, nullptr,
         state_topic, dev, "mdi:chart-bell-curve", false, false);
    // Announced only when the barometer actually works. On a unit whose pressure channel is dead
    // the field never appears, and an entity that is permanently unknown reads as a broken setup
    // rather than as missing hardware.
    if (global_b_pres > 30000.0f && global_b_pres < 110000.0f)
        emit(cl, "sensor", uid, "pressure", "Pressure", "pressure", "hPa", "pressure", state_topic, dev);

    // The four health figures. Diagnostic, so they sit in their own section of the device page
    // rather than beside the temperature: the owner should be able to find them, not trip over
    // them. They are the answer to "is it behaving?", which until now nothing could answer without
    // a button press and a 60 KB download.
    emit(cl, "sensor", uid, "wakesday", "Wakes per day", "wakes_day", nullptr, nullptr,
         state_topic, dev, "mdi:sleep-off", true);
    emit(cl, "sensor", uid, "screenday", "Screen refreshes per day", "screen_day", nullptr, nullptr,
         state_topic, dev, "mdi:monitor-eye", true);
    emit(cl, "sensor", uid, "duty", "Awake time", "duty_pct", "%", nullptr,
         state_topic, dev, "mdi:timer-outline", true);
    emit(cl, "sensor", uid, "edgesday", "Rejected button edges per day", "edges_day", nullptr,
         nullptr, state_topic, dev, "mdi:gesture-tap-button", true);

    // The plug is a binary sensor, so it needs the on/off payloads spelled out — the JSON carries
    // a boolean and Home Assistant renders it as the words True and False.
    {
        JsonDocument d;
        d["name"] = "On USB power";
        d["uniq_id"] = std::string(uid) + "_usb";
        d["stat_t"] = state_topic;
        d["val_tpl"] = "{{ value_json.on_usb }}";
        d["pl_on"] = "True";
        d["pl_off"] = "False";
        d["dev_cla"] = "plug";
        d["ent_cat"] = "diagnostic";
        d["dev"] = dev;
        char topic[160];
        snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/%s_usb/config", uid);
        std::string body; serializeJson(d, body);
        esp_mqtt_client_publish(cl, topic, body.c_str(), body.size(), 1, 1);
    }
    ESP_LOGW(TAG, "announced this device to Home Assistant as %s (\"%s\")", uid, name.c_str());
}


// Is this broker on the home network, or out on the internet?
//
// The distinction decides whether an unencrypted connection is allowed at all. Plain MQTT carries
// the password and every reading in the clear; on a home network that is a considered trade (one
// less handshake on a battery), out on the internet it is simply handing them over. So the choice
// is not left to a checkbox somebody might not understand: a public address without encryption is
// refused, and the console says why.
//
// A bare name with no dot, or one ending in .local, is a machine on the local network. Anything
// else that is not a private IPv4 range is treated as public — the safe way round, because being
// wrong in that direction costs a handshake and being wrong the other way costs the password.
static bool broker_is_local(const std::string &h)
{
    if (h.empty()) return false;
    if (h.find('.') == std::string::npos) return true;                       // "nas", "raspberrypi"
    // The suffixes people actually give a machine at home. Only ".local" was recognised, so a
    // perfectly ordinary "homeassistant.fritz.box" or "nas.lan" was judged to be out on the
    // internet and the connection refused for want of encryption it does not need.
    for (const char *sfx : { ".local", ".lan", ".home", ".internal", ".home.arpa" }) {
        const size_t n = strlen(sfx);
        if (h.size() > n && h.compare(h.size() - n, n, sfx) == 0) return true;
    }

    unsigned a, b, c, d; int used = 0;
    // %n so the match has to be the WHOLE string. Without it sscanf happily read the first four
    // numbers of "10.0.0.1.someone-elses-host.com" and called it a private address — which is the
    // one answer that must never be given by mistake, because it is what decides whether the
    // readings may go out unencrypted.
    if (sscanf(h.c_str(), "%u.%u.%u.%u%n", &a, &b, &c, &d, &used) != 4) return false;
    if (used != (int)h.size()) return false;                                 // trailing anything
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    if (a == 127) return true;                                              // loopback
    if (a == 10)  return true;                                              // 10.0.0.0/8
    if (a == 192 && b == 168) return true;                                  // 192.168.0.0/16
    if (a == 172 && b >= 16 && b <= 31) return true;                        // 172.16.0.0/12
    if (a == 169 && b == 254) return true;                                  // link-local
    return false;
}

MqttPublisher::Result MqttPublisher::publishState(const std::string &payload, bool also_discovery,
                                                  std::string *weather_out, int timeout_ms,
                                                  const std::string *alert, bool *alert_acked)
{
    if (alert_acked) *alert_acked = false;
    std::string host, topic, user, pass;
    uint32_t port = 1883;
    NVSManager::getInstance().getString("mq_host", host, "");
    NVSManager::getInstance().getString("mq_topic", topic, "smartmeteo");
    NVSManager::getInstance().getString("mq_user", user, "");
    NVSManager::getInstance().getString("mq_pass", pass, "");
    NVSManager::getInstance().getU32("mq_port", port, 1883);

    if (host.empty()) return { false, "no broker" };
    if (topic.empty()) topic = "smartmeteo";

    uint8_t tls = 0;
    NVSManager::getInstance().getU8("mq_tls", tls, 0);
    const bool local = broker_is_local(host);
    // Encryption is not optional once the broker is off the home network. Refusing here rather
    // than connecting anyway is the whole point: the failure is visible in the console and costs
    // nothing, while the alternative would quietly publish the password across the internet.
    if (!local && !tls) {
        ESP_LOGE(TAG, "%s is not on the local network and encryption is off — refusing to connect",
                 host.c_str());
        return { false, "insecure" };
    }
    const bool use_tls = tls || !local;

    char uri[128];
    snprintf(uri, sizeof(uri), "%s://%s:%u", use_tls ? "mqtts" : "mqtt",
             host.c_str(), (unsigned)port);
    char full_topic[96];
    snprintf(full_topic, sizeof(full_topic), "%s/state", topic.c_str());

    if (!s_bits) s_bits = xEventGroupCreate();
    if (!s_bits) return { false, "connect" };
    xEventGroupClearBits(s_bits, BIT_CONNECTED | BIT_PUBLISHED | BIT_FAILED);
    acked_reset();

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri;
    // The same bundle of root certificates the weather and Telegram already use, so a broker with
    // an ordinary Let's Encrypt certificate is verified with nothing to install on the device.
    if (use_tls) cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    if (!user.empty()) {
        cfg.credentials.username = user.c_str();
        cfg.credentials.authentication.password = pass.c_str();
    }
    cfg.session.keepalive = 30;
    cfg.network.timeout_ms = timeout_ms;
    cfg.network.disable_auto_reconnect = true;   // one attempt: this is a window, not a session

    esp_mqtt_client_handle_t cl = esp_mqtt_client_init(&cfg);
    if (!cl) return { false, "connect" };
    esp_mqtt_client_register_event(cl, MQTT_EVENT_ANY, mqtt_event_handler, nullptr);

    ESP_LOGW(TAG, "connecting to %s (%s)", uri,
             use_tls ? "encrypted" : "plain, local network only");
    if (esp_mqtt_client_start(cl) != ESP_OK) {
        esp_mqtt_client_destroy(cl);
        return { false, "connect" };
    }

    EventBits_t b = xEventGroupWaitBits(s_bits, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms));
    if (!(b & BIT_CONNECTED)) {
        ESP_LOGE(TAG, "no connection to %s", uri);
        esp_mqtt_client_stop(cl);
        esp_mqtt_client_destroy(cl);
        return { false, "connect" };
    }

    // Describe the device before reporting it, so a hub seeing it for the first time has the
    // definitions in hand when the readings land a moment later.
    if (also_discovery) publishDiscovery(cl, full_topic);

    // Retained, at least once. Retained because this device speaks every half hour at best: without
    // it, a broker restart leaves Home Assistant showing nothing until the next window.
    const int mid = esp_mqtt_client_publish(cl, full_topic, payload.c_str(), payload.size(), 1, 1);
    bool ok = (mid > 0) && wait_puback(mid, timeout_ms);

    // The alert as an event of its own, after the state it belongs to. Matched by its own msg_id, like
    // the flight recorder below: an acknowledgement of the state must not settle the alert.
    if (ok && alert && !alert->empty() && alert_acked) {
        char atopic[96];
        snprintf(atopic, sizeof(atopic), "%s/alert", topic.c_str());
        const int amid = esp_mqtt_client_publish(cl, atopic, alert->data(), alert->size(), 1, 0);
        *alert_acked = (amid > 0) && wait_puback(amid, 4000);
        ESP_LOGW(TAG, "alert event to %s: %s", atopic, *alert_acked ? "acknowledged" : "NOT acknowledged");
    }

    // While the connection is still up, take the outdoor weather the hub leaves for us. This is
    // the whole reason the device no longer needs the internet: the hub already knows the weather,
    // and a retained message is delivered the moment a subscription is made — no request, no TLS
    // handshake with a weather service, no API key, and nothing added to the time the radio is on.
    if (ok && weather_out) {
        char wtopic[96];
        snprintf(wtopic, sizeof(wtopic), "%s/weather", topic.c_str());
        s_rx.clear();
        xEventGroupClearBits(s_bits, BIT_DATA);
        if (esp_mqtt_client_subscribe(cl, wtopic, 0) >= 0) {
            // Short on purpose. A retained message is already sitting on the broker, so it comes
            // back in milliseconds; waiting longer would only be waiting for one that is not there.
            const EventBits_t wb = xEventGroupWaitBits(s_bits, BIT_DATA, pdFALSE, pdFALSE,
                                                       pdMS_TO_TICKS(1500));
            if (wb & BIT_DATA) {
                *weather_out = s_rx;
                ESP_LOGW(TAG, "outdoor weather from the hub: %u bytes", (unsigned)s_rx.size());
            } else {
                ESP_LOGW(TAG, "no weather waiting on %s — the hub may not be publishing it", wtopic);
            }
        }
    }

    // The flight recorder, riding along in a session that is already open. The marginal cost is the
    // bytes alone — well under a kilobyte in a normal window, against a ~20 s association — which is why
    // it is attached here rather than given a window of its own: radio is the largest item in this
    // device's energy budget, and it is the CONNECTION that costs, not the payload.
    //
    // NOT retained, unlike the state message. Retaining one arbitrary batch would pin it on the
    // broker for good and hand every fresh subscriber a stale slice of history — the opposite of
    // what a log is for. State is retained because it describes NOW and a hub restart must not show
    // empty sensors; a log describes a span, and spans do not replace one another.
    //
    // The ring on the device remains the system of record and this is only a mirror, so the
    // high-water mark advances solely on an acknowledged publish. A hub that was down gets more
    // records next window instead of leaving a hole, and anything sent twice is dropped by sequence
    // number when it is read back.
    if (ok) {
        EvtLog &log = EvtLog::getInstance();
        std::string chunk;
        const uint32_t sent = log.lastSent();
        // 128 x 32 B = 4 KB, and the ceiling is about the PUBLISH, not about catching up: the
        // client buffer is 1 KB by default and QoS 1 parks the whole message in the outbox on the
        // heap, next to the ~90 KB Wi-Fi has already taken. A 16 KB message there is the same
        // shape as the silent DMA failure that once ate a panel refresh, and it would be reached
        // only when something is already wrong. At the measured ~1065 wakes/day a window produces
        // roughly twenty records, so this is never approached; after a hub outage it drains ~6000
        // records a day, which clears a day of backlog in about four hours. A backlog that grows
        // window over window is therefore a signal in its own right, not something to raise the
        // ceiling for — the ring on the device is the record, and it holds 23 days regardless.
        const uint32_t high = log.buildChunk(sent, 128, chunk);
        if (high > sent && !chunk.empty()) {
            char etopic[96];
            snprintf(etopic, sizeof(etopic), "%s/events", topic.c_str());
            // Matched by its own msg_id: the acknowledgement of a discovery message or of the state
            // must not mark these records as delivered.
            const int emid = esp_mqtt_client_publish(cl, etopic, chunk.data(), chunk.size(), 1, 0);
            if (emid > 0) {
                if (wait_puback(emid, 4000)) {
                    log.markSent(high);
                    ESP_LOGW(TAG, "flight recorder: %u bytes to %s (through seq %u)",
                             (unsigned)chunk.size(), etopic, (unsigned)high);
                } else {
                    ESP_LOGW(TAG, "flight recorder not acknowledged — resending next window");
                }
            }
        }
    }

    esp_mqtt_client_stop(cl);
    esp_mqtt_client_destroy(cl);

    if (!ok) { ESP_LOGE(TAG, "publish to %s failed", full_topic); return { false, "publish" }; }
    ESP_LOGW(TAG, "published %u bytes to %s", (unsigned)payload.size(), full_topic);
    return { true, nullptr };
}
