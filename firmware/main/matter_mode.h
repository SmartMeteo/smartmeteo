// Matter over Thread — the device's second radio personality (owner, 2026-09-16).
//
// One image carries both: Wi-Fi mode (weather, Telegram, MQTT, updates, deep sleep) and this one,
// chosen by the NVS key "wl_mode" (2 = Matter). Everything CHIP-specific lives behind this header so
// the cycle in bench_screens.cpp never includes a CHIP header.
//
// What is proven and where (CLAUDE.md, 2026-09-16 journal): commissioning to a Nest Hub over Thread,
// the three sensor endpoints below, the CHIP-served clusters written through their own APIs, the
// panel and the sensors running beside the stack. What is new here: per-device setup codes, the
// Time Synchronization cluster, light sleep, and the stack living inside the product's cycle.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace MatterMode {

// Is Matter the selected mode? (NVS "wl_mode" == 2 — the console's third connectivity mode; NVS must
// already be initialised.)
bool selected();

// ---- setup codes: available with or without the stack ----------------------------------------
// Each device has its own passcode, discriminator and PASE salt, generated once and kept in NVS
// ("mt_pin", "mt_disc", "mt_salt"). rf_active says whether a radio is running: the hardware RNG
// only gives true random numbers then, and otherwise the generator is switched to the internal
// entropy source for the duration (ESP-IDF "Random Number Generation").
struct Codes {
    char qr[40];        // "MT:..." — the payload the QR code carries
    char manual[16];    // "12345678901"
    char manual_fmt[16];// "1234-567-8901", as printed under the code
};
bool codes(Codes &out, bool rf_active);

// The QR module grid for `text`: returns the side length in modules (0 on failure) and fills
// `grid` row by row, one byte per module, 1 = dark. Versions up to 10 (57x57) are accepted; the
// Matter payload encodes as version 2 (25x25), so 29*29 bytes is enough for it.
int qr_grid(const char *text, uint8_t *grid, size_t grid_len);

// An SVG rendering of the pairing QR, for the console.
bool qr_svg(std::string &out, bool rf_active);

// ---- the stack ---------------------------------------------------------------------------------
// Creates the node (air quality with CO2, temperature, humidity; Time Synchronization on the root),
// configures OpenThread as a sleepy end device and starts esp_matter. Once per boot.
bool start();
bool running();

// Readings to publish. Values the device does not have are passed as !valid and published as null,
// so a hub shows "unavailable" rather than a stale number.
struct Readings {
    bool  co2_valid;  float co2;       // ppm
    bool  climate_valid; float temp_c, hum;
    int   air_quality;                 // Matter AirQualityEnum: 0 unknown, 1 good .. 6 extremely poor
};
void publish(const Readings &r);

// State for the cycle and the screen.
uint8_t  fabrics();          // live while running; otherwise the last count stored ("mt_fab")
bool     window_open();      // the commissioning window
bool     session_active();   // a commissioner is talking to us right now
bool     open_window();      // reopen after a timeout; false if the stack refused
bool     thread_attached();

// Events since the last call, raised from the Matter thread.
enum : uint32_t {
    EV_PAIRED        = 1u << 0,   // commissioning completed
    EV_LAST_FABRIC   = 1u << 1,   // the last fabric was removed: the device leaves the home
    EV_WINDOW        = 1u << 2,   // the commissioning window opened or closed
    EV_SESSION       = 1u << 3,   // a commissioning session started or stopped
    EV_FAILSAFE      = 1u << 4,   // commissioning was abandoned (fail-safe expired)
};
uint32_t take_events();

// Called on every event, from the Matter thread. The cycle uses it to end its wait early.
void set_event_hook(void (*hook)(void));

// Leaving the home: erases the fabrics, the Thread network and esp_matter's data, then restarts.
// Only valid while running. Does not return.
[[noreturn]] void forget_and_restart();

// The console's "forget the smart home" button, carried to the next Matter start ("mt_forget"): the stack
// is not running in a console session. request_forget(false) withdraws a request still pending.
// forget_requested() only reads it; clear_forget() once it is being carried out, so a stack that fails
// to start keeps the request for the next boot.
void request_forget(bool on = true);
bool forget_requested();
void clear_forget();

} // namespace MatterMode
