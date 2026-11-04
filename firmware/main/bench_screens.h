#pragma once

// The device cycle: wake, measure, go online only if something needs the network, draw once,
// sleep. Never returns — it ends in deep sleep. Implemented in bench_screens.cpp.
void bench_screens_run(void);

// Whether the owner has finished setting the device up: a network it joined (or a new name on a device
// that was set up before), a choice to run without one, or Matter. The cycle and the console's setup
// cookie both ask this, so they cannot disagree.
bool device_configured();
