#pragma once
#include <string>

// Publishes one message to a local broker and disconnects again.
//
// Deliberately NOT a long-lived connection. Keeping an MQTT session alive means keeping Wi-Fi
// associated, which on this device costs weeks of battery life against months — the same reason
// Matter-over-Wi-Fi is offered on the cable only. So this connects inside the radio window that
// already exists for the weather, says its piece, and goes away.
//
// The message is retained, which matters for a device that speaks once every half hour: a broker
// restart, or Home Assistant reloading, would otherwise show empty sensors until the next window.
namespace MqttPublisher {

struct Result {
    bool ok;
    const char *error;   // "off" | "no broker" | "insecure" | "connect" | "publish" | nullptr
};

// Reads host/port/topic/credentials from NVS. Returns quickly when MQTT is not the selected mode.
// `payload` is the JSON body; the topic is "<mq_topic>/state".
//
// With `also_discovery` the session first announces every reading to Home Assistant, so the hub
// builds the device by itself instead of the owner hand-writing YAML. Both happen in ONE connection:
// the expensive part is the radio, not the bytes.
//
// With `alert` the same session also publishes that event to "<mq_topic>/alert" — not retained,
// at least once — and `alert_acked` says whether the broker acknowledged it. An event is not state:
// retaining it would re-announce an old alert to every hub that subscribes later.
Result publishState(const std::string &payload, bool also_discovery = false,
                    std::string *weather_out = nullptr, int timeout_ms = 8000,
                    const std::string *alert = nullptr, bool *alert_acked = nullptr);

// Outdoor weather, when the hub is asked to provide it: the same session subscribes to
// "<mq_topic>/weather" and takes whatever retained message is waiting there. Costs no extra radio —
// the connection is already open, and a retained message arrives the instant you subscribe.
//
// Builds the JSON body from the current readings.
std::string buildState();

// The identifier the device announces in discovery ("smartmeteo_" + the last three bytes of the station
// MAC). Stable across renames, so a hub can find THIS device's entities by it — the console writes it
// into the Home Assistant automation it hands out. One function, so the two cannot drift apart.
std::string deviceId();

}  // namespace MqttPublisher
