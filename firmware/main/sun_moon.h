#pragma once
// Sunrise, sunset and the phase of the moon, worked out on the device.
//
// This exists so those three cells of the bottom strip (strip_cells.h) need no network. Everything
// else in that strip comes from the weather, which Matter, no_wifi and Ultra-Save-on-plain-Wi-Fi
// never fetch; these are computed from the coordinates already in NVS and the clock, so they stay
// true in exactly the modes where the rest of the strip is blank. Asking a hub for them over MQTT
// would trade a always-correct local sum for a dependency and buy nothing.
//
// Sun: the NOAA/"sunrise equation" formulation — mean anomaly, equation of centre, ecliptic
// longitude, then the hour angle at a zenith of 90.833 degrees (the usual allowance for refraction
// and the sun's radius). Good to a minute or two, which is well inside what a screen showing HH:MM
// can express. Verified on the computer against a second implementation written from a different
// formulation, and against invariants that hold without any reference table: day length at an
// equinox, symmetry about solar noon, and polar day and night at high latitude.
//
// Moon: mean synodic phase from a known new moon. This ignores the moon's real, uneven motion, so
// the phase can be out by up to about half a day near the quarters — a few per cent of the lit
// fraction. For a 19 px disc that is invisible, and the honest alternative costs far more code than
// the picture is worth. Do not reuse this for anything that needs the real phase angle.
#include <time.h>
#include <stdint.h>

struct SunTimes {
    bool   valid;        // false when the sun does not rise or set at all on this day
    bool   always_up;    // polar day (only meaningful when !valid)
    int    rise_min;     // minutes past local midnight, already shifted by tz_h
    int    set_min;
};

// lat/lon in degrees, tz_offset_s the same offset the rest of the firmware uses (tz_h * 3600).
// `when` is a UTC time_t; the day resolved is the LOCAL day at that instant.
SunTimes sun_times(double lat, double lon, time_t when, int tz_offset_s);

// 0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter. Always in [0,1).
double moon_phase(time_t when);
