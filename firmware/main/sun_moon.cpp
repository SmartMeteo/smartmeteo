#include "sun_moon.h"
#include <math.h>

namespace {

constexpr double PI  = 3.14159265358979323846;
constexpr double DEG = PI / 180.0;

// Unix epoch as a Julian day. time_t is UTC seconds, so this is the only bridge needed.
constexpr double JD_UNIX = 2440587.5;
constexpr double J2000   = 2451545.0;

inline double wrap360(double d) { d = fmod(d, 360.0); return d < 0 ? d + 360.0 : d; }

}  // namespace

SunTimes sun_times(double lat, double lon, time_t when, int tz_offset_s)
{
    SunTimes out = {false, false, 0, 0};

    // The local day this instant falls in, as a whole number of days since the epoch. Working in
    // local days and only then converting back is what makes the answer belong to the day the
    // screen is showing, rather than to a UTC day that may already have rolled over.
    const long long local = (long long)when + tz_offset_s;
    const long long day   = (long long)floor((double)local / 86400.0);
    const double midnight_utc = (double)day * 86400.0 - (double)tz_offset_s;   // UTC seconds
    const double jd_midnight  = JD_UNIX + midnight_utc / 86400.0;

    // Solar noon for a given day number n, and the hour angle that puts the sun on the horizon.
    // Returns false when the sun neither rises nor sets that day.
    struct Day { double transit; double hour_angle; bool has_event; bool up_all_day; };
    auto solve = [&](double n) {
        Day r{};
        // J* is mean solar time as days. East of Greenwich the sun crosses earlier, hence -lon/360.
        const double Jstar = n + 0.0008 - lon / 360.0;
        const double M = wrap360(357.5291 + 0.98560028 * Jstar);          // solar mean anomaly
        const double C = 1.9148 * sin(M * DEG) + 0.0200 * sin(2 * M * DEG)
                       + 0.0003 * sin(3 * M * DEG);                        // equation of centre
        const double L = wrap360(M + C + 180.0 + 102.9372);                // ecliptic longitude
        r.transit = J2000 + Jstar + 0.0053 * sin(M * DEG) - 0.0069 * sin(2 * L * DEG);

        const double sin_d = sin(L * DEG) * sin(23.4397 * DEG);            // declination
        const double cos_d = sqrt(1.0 - sin_d * sin_d);
        // -0.833 deg: the sun's upper limb at the horizon, refraction included.
        const double cos_w = (sin(-0.833 * DEG) - sin(lat * DEG) * sin_d) / (cos(lat * DEG) * cos_d);
        if (cos_w > 1.0)       { r.has_event = false; r.up_all_day = false; }
        else if (cos_w < -1.0) { r.has_event = false; r.up_all_day = true;  }
        else                   { r.has_event = true;  r.hour_angle = acos(cos_w) / DEG; }
        return r;
    };

    // Which day number belongs to this local day is not obvious: the offset can be anything from
    // -12 to +14, so local noon can sit either side of the UTC day boundary. Rather than reason it
    // out and be wrong at one edge, the three candidates are tried and the one whose transit lands
    // inside this local day is taken. Off-by-one here is a whole day of wrong times, and it cost
    // one debugging round already.
    const double n0 = floor(jd_midnight + 0.5 - J2000);
    Day best{}; double best_local = 0; bool found = false;
    for (int k = -1; k <= 1 && !found; k++) {
        Day c = solve(n0 + k);
        const double l = (c.transit - JD_UNIX) * 86400.0 + (double)tz_offset_s - (double)day * 86400.0;
        if (l >= 0.0 && l < 86400.0) { best = c; best_local = l; found = true; }
    }
    if (!found) return out;                     // cannot happen for a real offset; refuse rather than guess
    if (!best.has_event) { out.valid = false; out.always_up = best.up_all_day; return out; }

    // Minutes past local midnight. Clamped rather than wrapped: at the edge of the polar circle a
    // rise can land a few minutes either side of midnight, and a wrap would throw it to the far end
    // of the day, which reads as a gross error rather than the boundary case it is.
    auto to_min = [](double secs) {
        long m = lround(secs / 60.0);
        if (m < 0) m = 0;
        if (m > 1439) m = 1439;
        return (int)m;
    };
    const double half = best.hour_angle / 360.0 * 86400.0;   // seconds from transit to the horizon
    out.rise_min = to_min(best_local - half);
    out.set_min  = to_min(best_local + half);
    out.valid    = true;
    return out;
}

double moon_phase(time_t when)
{
    // New moon of 2000-01-06 18:14 UTC, and the mean synodic month.
    constexpr double NEW_MOON_JD = 2451550.26;
    constexpr double SYNODIC     = 29.530588853;
    const double jd = JD_UNIX + (double)when / 86400.0;
    double p = fmod((jd - NEW_MOON_JD) / SYNODIC, 1.0);
    if (p < 0) p += 1.0;
    return p;
}
