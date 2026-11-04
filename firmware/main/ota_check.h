#pragma once
#include <time.h>
#include <stdint.h>

// Asking whether a newer firmware exists. Only asking: installing is a separate brick, and keeping
// them separate means this one can be shipped, watched and trusted before anything is ever written
// to flash from the network.
namespace OtaCheck {

// Is a check due? Answered from the schedule alone — it never opens a radio session of its own, so
// the answer only matters while one is already up for another reason.
bool due(time_t now);

// Fetch the manifest and decide. Call LAST in a radio window, after the state publish, the clock
// and the weather: those are what the window exists for, and an update check must never be the
// reason any of them was skipped.
void run(void);

// What the last check found, for the console. Empty version means "nothing newer".
extern char     available_ver[16];
extern uint32_t available_seq;
extern time_t   last_checked;      // ATTEMPT, not success
extern char     last_error[24];    // empty when the last check worked
extern char     available_path[128];  // manifest path of the offered image, validated

// These four live in .bss and this device cold-boots out of deep sleep about once a minute, while
// a check happens once a day — so without these the console could only ever see a result produced
// in the very same wake. Kept in NVS: one write a day.
void persist(void);
void restore(void);

}  // namespace OtaCheck
