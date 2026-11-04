// Sunrise, sunset and moon phase — the real main/sun_moon.cpp, compiled as it ships.
//
// There is no reference table here on purpose. A table would only say that I copied it correctly.
// What is checked instead are facts that hold whatever the algorithm is, and that fail loudly if it
// is wrong: an equinox gives twelve hours of daylight everywhere, rise and set sit symmetrically
// about solar noon, noon itself follows the longitude, and above the polar circle the sun refuses
// to rise in December and to set in June. A second, independent implementation is compared against
// this one by tests/t_sunmoon.py, which builds the same times from the equation-of-time formulation
// rather than the hour-angle one.
#include "../main/sun_moon.h"
#include "check.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>

// A UTC time_t for a date at noon, without touching the host timezone.
static time_t utc_noon(int y, int m, int d)
{
    // Days from the civil epoch — Howard Hinnant's days_from_civil, which is exact and has no
    // library dependency. mktime() would drag in whatever TZ this machine happens to have.
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = (long long)era * 146097 + (long long)doe - 719468;
    return (time_t)(days * 86400 + 43200);
}

int main(int argc, char **argv)
{
    // --dump feeds the cross-check in t_sunmoon.py: one line per case, minutes past local midnight.
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        struct { double lat, lon; int tz; } P[] = {
            {49.8728,  8.6512, 3600},   // Darmstadt
            {51.5074, -0.1278,    0},   // London
            {-33.8688,151.2093,39600},  // Sydney
            {64.1466,-21.9426,    0},   // Reykjavik
            {1.3521, 103.8198,28800},   // Singapore
        };
        for (auto &p : P)
            for (int m = 1; m <= 12; m++)
                for (int d : {1, 15}) {
                    SunTimes s = sun_times(p.lat, p.lon, utc_noon(2026, m, d), p.tz);
                    printf("%.4f,%.4f,%d,2026-%02d-%02d,%d,%d,%d\n",
                           p.lat, p.lon, p.tz, m, d, s.valid ? 1 : 0, s.rise_min, s.set_min);
                }
        return 0;
    }

    area("равноденствие");
    // On an equinox the sun is above the horizon for twelve hours everywhere. Not exactly twelve —
    // refraction and the sun's radius add several minutes, and more the further from the equator,
    // which is why the tolerance widens with latitude rather than being one loose number.
    struct { double lat; const char *name; double tol; } EQ[] = {
        { 0.0,  "экватор",   10},
        {49.87, "Дармштадт", 15},
        {60.0,  "Осло",      20},
    };
    for (auto &e : EQ) {
        SunTimes s = sun_times(e.lat, 0.0, utc_noon(2026, 3, 20), 0);
        ok(s.valid, e.name);
        near(s.set_min - s.rise_min, 720, e.tol, e.name);
    }

    area("симметрия");
    // Rise and set straddle solar noon. Solar noon at longitude 0 with no timezone offset is a few
    // minutes either side of 12:00 — the equation of time — so what is checked is the symmetry, not
    // the absolute noon.
    for (int m = 1; m <= 12; m++) {
        SunTimes s = sun_times(49.8728, 0.0, utc_noon(2026, m, 15), 0);
        ok(s.valid, "средние широты: солнце восходит круглый год");
        const double noon = (s.rise_min + s.set_min) / 2.0;
        near(noon, 720, 17, "полдень в пределах уравнения времени");
    }

    area("долгота");
    // Move 15 degrees east and, at the same local offset, everything happens an hour earlier.
    {
        SunTimes a = sun_times(50.0,  0.0, utc_noon(2026, 6, 15), 0);
        SunTimes b = sun_times(50.0, 15.0, utc_noon(2026, 6, 15), 0);
        near(a.rise_min - b.rise_min, 60, 2, "15° на восток = на час раньше");
        near(a.set_min  - b.set_min,  60, 2, "то же для заката");
    }

    area("полярные день и ночь");
    {
        SunTimes w = sun_times(78.2, 15.6, utc_noon(2026, 12, 21), 3600);   // Longyearbyen
        ok(!w.valid && !w.always_up, "в декабре за полярным кругом солнце не встаёт");
        SunTimes s = sun_times(78.2, 15.6, utc_noon(2026, 6, 21), 3600);
        ok(!s.valid && s.always_up,  "в июне оно не садится");
    }

    area("часовой пояс");
    // The offset shifts the clock, not the sun: an hour of tz moves both times by exactly an hour.
    {
        SunTimes a = sun_times(49.8728, 8.6512, utc_noon(2026, 9, 24), 3600);
        SunTimes b = sun_times(49.8728, 8.6512, utc_noon(2026, 9, 24), 7200);
        eq_int(b.rise_min - a.rise_min, 60, "пояс сдвигает восход ровно на час");
        eq_int(b.set_min  - a.set_min,  60, "и закат тоже");
    }

    area("границы");
    // Whatever happens, the strip gets something it can print in HH:MM.
    for (double lat = -89; lat <= 89; lat += 7)
        for (int m = 1; m <= 12; m++) {
            SunTimes s = sun_times(lat, 8.65, utc_noon(2026, m, 10), 3600);
            if (!s.valid) continue;
            ok(s.rise_min >= 0 && s.rise_min <= 1439 && s.set_min >= 0 && s.set_min <= 1439,
               "минуты всегда в пределах суток");
        }

    area("луна");
    {
        // A synodic month brings the phase back to where it was.
        const time_t t0 = utc_noon(2026, 9, 24);
        near(moon_phase(t0 + (time_t)(29.530588853 * 86400)), moon_phase(t0), 0.002,
             "через синодический месяц фаза та же");
        // Half a month away is the opposite phase.
        double a = moon_phase(t0), b = moon_phase(t0 + (time_t)(29.530588853 * 86400 / 2));
        near(fabs(a - b), 0.5, 0.01, "через полмесяца — противоположная фаза");
        // Always in range, over ten years of days.
        bool in_range = true, saw_new = false, saw_full = false;
        for (time_t t = utc_noon(2020, 1, 1); t < utc_noon(2030, 1, 1); t += 86400) {
            double p = moon_phase(t);
            if (!(p >= 0.0 && p < 1.0)) in_range = false;
            if (p < 0.02 || p > 0.98) saw_new = true;
            if (fabs(p - 0.5) < 0.02)  saw_full = true;
        }
        ok(in_range, "фаза всегда в [0,1)");
        ok(saw_new && saw_full, "за десять лет бывают и новолуние, и полнолуние");
    }

    return report("солнце и луна");
}
