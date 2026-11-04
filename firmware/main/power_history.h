#pragma once
// The battery estimate, read from the history ring — the ONE place it is computed for every reader:
// the console's battery card (/api/status) and the hub (MQTT state, and through discovery Home
// Assistant). Until 2026-10-04 the hub had none of its own and Home Assistant filled the gap with a
// template: the derivative of WHOLE percentages over a day, which on the flat of the curve, with the
// gauge flickering 82 <-> 83, swung from 0.95 to 0.17 mA in two days while nothing about the device
// changed. Now it shows what the console shows, from the same fit.
//
// The arithmetic is power_estimate.cpp (pure, host-tested); this end only reads the ring and keeps
// the answer for half a minute — the page polls every few seconds, and a new record appears once
// every few minutes at most. Called from the httpd task and from the loop's radio window, so the
// cache is behind a mutex.

#include "power_estimate.h"

namespace PowerHistory {

PowerEstimate::Result estimate();

// What the hub is told about the estimate, in one word: "collecting" until the drop passes the
// gauge's own accuracy, then "rough", then "good" — the same three states as the console's notice.
const char *quality(const PowerEstimate::Result &r);

}  // namespace PowerHistory
