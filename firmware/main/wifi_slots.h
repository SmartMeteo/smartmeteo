#pragma once
// The networks the device remembers, and the rules for them — in one place because four readers
// need them: the connect path (wifi_manager.cpp), /api/save and /api/status (web_server.cpp), and
// the console, which is checked against this file by t_console.
//
// WHY MORE THAN ONE. A device that travels sees home and work, and until 2026-10-01 arriving
// somewhere new meant holding the button, joining its access point and typing a network in again —
// every time, in both directions. Owner's idea, 2026-10-01, after carrying it to an office.
//
// WHY THREE. Storage is not the limit (three networks are ~300 bytes of NVS); the choice between
// them is. Home, work and one more covers what a device like this actually travels between, and a
// short list keeps the "which of these is in the air" question answerable with a single scan.
#include <string>

namespace WifiSlots {

static constexpr int COUNT = 3;

struct Net { std::string ssid, pass; };

// SLOT 0 IS "ssid"/"pass" — the very keys device_configured(), the setup screen, the wizard and
// /api/status have read since the beginning, and they keep reading them untouched. That is the whole
// migration story: everything written for one network still sees one network, and what it sees is
// the one that worked last. Slots 1 and 2 are history, under "ssid2"/"pass2" and "ssid3"/"pass3".
const char *ssidKey(int i);
const char *passKey(int i);

// Fills `out` newest first and returns how many are real. Empty slots are skipped, so a gap left by
// a forgotten network never hides the ones behind it.
int  load(Net out[COUNT]);

// A network the owner just gave us, or one whose password changed: it goes to slot 0 and the oldest
// falls off the end. Saving a network already known moves it to the front instead of duplicating it.
void remember(const std::string &ssid, const std::string &pass);

// The one that just carried a connection. Same as remember(), without needing the password back.
void promote(const std::string &ssid);

bool forget(const std::string &ssid);

// "No network at all". Clears every slot, not just the first: leaving history behind would mean a
// device told to forget its network quietly joining the one before it.
void clear();

}  // namespace WifiSlots
