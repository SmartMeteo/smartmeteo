// Light sleep, permitted only where we say so.
//
// The flight recorder settles what this is worth (2026-09-19, 51.6 h, 2362 wakes): the device is
// awake 45.5 minutes a day, and 46 % of that is one thing — waiting for the SCD41's five-second
// conversion and the BSEC heater. A measuring wake is 6111 ms against 1073 for an ordinary one.
// Those five seconds are spent in vTaskDelay with the core clocked down to 40 MHz but fully awake.
//
// Automatic light sleep was in this firmware once and was removed on 2026-09-11, for a reason that
// had nothing to do with the waits: esp_pm armed a timer wake source of its own, the logo sleep
// added none, and the button stopped waking the device from it. So light sleep is not simply turned
// back on. It is enabled and then held OFF by a lock, and the lock is released only inside a Nap —
// a named, bounded wait. Everything outside one behaves exactly as it does today.
//
// Why no Wi-Fi guard is needed, and this is load-bearing: both gated waits live inside measure(),
// which the working cycle calls BEFORE radio_window() (bench_screens.cpp). The radio is down
// whenever a Nap is open, so light sleep cannot stall an association or a socket. The console
// window runs its own single-shot loop and never reaches these two waits. If either of those ever
// stops being true, this needs a guard rather than a comment.
//
// The other lesson kept here is from 2026-07-11: a manual esp_light_sleep_start() on the remainder
// of the SCD41 wait broke esp_timer and drove BSEC's virtual clock to -1.9e18 ns. The conclusion
// recorded then was that the right mechanism is PM-managed sleep, which compensates esp_timer.
// This is that mechanism; there is no manual sleep call anywhere in it.
#pragma once

#include <stdint.h>
#include "driver/gpio.h"

namespace PmGate {

// Enables automatic light sleep, protects the pads that must not be isolated (panel, its power
// gate, I2C, LED) and immediately blocks sleeping. `button` is armed as a wake source while a Nap
// is open, so a press during a wait is not swallowed — the pin number is passed rather than
// repeated here, it belongs to bench_screens.cpp.
//
// `on_press` is called from the end of a Nap when the button is found held down, and must do what
// the pin's own edge handler does on a low line. It is needed because the wake source is a LEVEL,
// and a level cannot be left armed on a pin that also carries an edge interrupt (see pm_gate.cpp):
// the interrupt is therefore off for the length of the wait, and this callback is how a press made
// during it still counts. It runs in task context, not from an ISR.
//
// Safe to call more than once. Returns false when power management or the lock could not be set
// up; then Nap does nothing and the cycle runs exactly as it always has.
bool begin(gpio_num_t button, gpio_num_t led, void (*on_press)() = nullptr);

bool active();

// Light sleep is permitted for the lifetime of this object, and the button can wake the chip out
// of it so a press during a wait is not swallowed. Nests: only the outermost one lifts the block.
class Nap {
public:
    explicit Nap(uint32_t expected_ms);
    ~Nap();
    Nap(const Nap &) = delete;
    Nap &operator=(const Nap &) = delete;
private:
    bool     held_    = false;
    uint32_t want_ms_ = 0;
    int64_t  slept0_  = 0;   // the counters as they stood when this nap opened, so the line it
    uint32_t naps0_   = 0;   // prints at the end is about this wait and not the whole wake
};

// How much of this wake the chip actually spent in light sleep, and how many times it went there.
// This exists because the event log cannot answer the question: awake_ms is measured with
// esp_timer, which the power-management layer compensates across light sleep on purpose so timers
// stay honest. The SCD41 still needs its five seconds of wall time; light sleep does not shorten
// them, it makes them cheap. So the saving is invisible in every number the device already keeps,
// and without this counter the claim "light sleep works here" could not be checked without a
// current meter. Zero when CONFIG_PM_LIGHT_SLEEP_CALLBACKS is off.
int64_t  slept_us();
uint32_t sleeps();

// Below this a nap costs more than it saves: entering and leaving light sleep is not free, and the
// short delays in the I2C paths are measured in milliseconds.
constexpr uint32_t kMinNapMs = 30;

}  // namespace PmGate
