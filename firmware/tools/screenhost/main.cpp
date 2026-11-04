// Renders the REAL dashboard: screen.cpp and epaper.cpp compiled here, with ESP-IDF stubbed.
// The point is that the text metrics and glyphs are the firmware's own, not a copy that can drift.
#include "screen.h"
#include "epaper.h"
#include <cstdio>
#include <cstring>
#include <string>

static void write_pgm(const EPaper &e, const char *path) {
    const uint8_t *fb = e.frame_buffer;
    FILE *f = fopen(path, "wb");
    fprintf(f, "P5\n%d %d\n255\n", EPD_WIDTH, EPD_HEIGHT);
    for (int y = 0; y < EPD_HEIGHT; y++)
        for (int x = 0; x < EPD_WIDTH; x++) {
            const uint8_t b = fb[(y * (EPD_WIDTH / 8)) + (x / 8)];
            fputc((b & (0x80 >> (x % 8))) ? 255 : 0, f);
        }
    fclose(f);
}

int main(int argc, char **argv) {
    ScreenData d = {};
    d.date = "18.09.2026"; d.time = "19:41"; d.batt = 87; d.co2 = "795"; d.fresh = "4m";
    d.temp = "22.8"; d.hum = "45"; d.aq = "Good"; d.aqLevel = 1; d.cal = "3/3";
    d.pres = "1012"; d.presU = "hPa";
    d.city = "Darmstadt, DE"; d.desc = "Partly cloudy"; d.wxIcon = "02d"; d.wxFresh = "32m";
    d.outT = 13; d.mmLo = "+9"; d.mmHi = "+16";
    d.outHum = "64"; d.wind = "3"; d.feels = "13";
    d.graphSpan = "24H";
    const char *dayn[4] = {"Sat 12","Sun 13","Mon 14","Tue 15"};
    const char *ic[4]   = {"02d","10d","11","01d"};
    const char *hi[4]   = {"22","19","16","24"};
    const char *lo[4]   = {"13","11","9","14"};
    for (int i = 0; i < 4; i++){ d.fcDay[i]=dayn[i]; d.fcIcon[i]=ic[i]; d.fcHi[i]=hi[i]; d.fcLo[i]=lo[i]; }
    d.fcN = 4;

    const std::string mode = argc > 1 ? argv[1] : "normal";
    if (mode == "extreme") {
        d.co2 = "12345"; d.temp = "-40.5"; d.hum = "100"; d.pres = "1084";
        d.aq = "Extreme"; d.aqLevel = 6; d.aqInvert = true; d.cal = "3/3";
        d.city = "Garmisch-Partenkirchen"; d.desc = "Thunderstorm with heavy hail";
        d.outT = -40; d.mmLo = "-40"; d.mmHi = "+125"; d.batt = 100;
        d.outHum = "100"; d.wind = "99"; d.feels = "-40";
        static const char *hx[4] = {"-40","125","-9","100"};
        static const char *lx[4] = {"-99","104","-40","99"};
        for (int i = 0; i < 4; i++){ d.fcHi[i]=hx[i]; d.fcLo[i]=lx[i]; }
        d.date = "18.09.2026"; d.time = "23:59";
    } else if (mode == "band") {
        d.setupLine = "smartmeteo.local (192.168.100.200)";
        d.setupLine2 = "Qw3rty!@";       // значение без подписи, как шлёт прошивка: 8 знаков,
                                 // обе регистра, цифра и символы — новый алфавит целиком
        d.fcN = 0;
    } else if (mode == "damp") {
        // Damp moved out of the air verdict into the humidity block, so Air Quality keeps showing
        // what its seven cells actually mean instead of being taken over.
        d.humDamp = true; d.hum = "72"; d.temp = "21.4"; d.co2 = "860";
        d.aq = "Light"; d.aqLevel = 2;
        d.outHum = "88"; d.feels = "11"; d.wind = "4"; d.outT = 11;
    } else if (mode == "dampstorm") {
        // The common pair: a front brings a falling barometer AND humidity. Two inverted halves,
        // one black band — the column rule has to be redrawn in white or it is swallowed.
        d.humDamp = true; d.hum = "74"; d.temp = "19.8"; d.co2 = "720";
        d.aq = "Good"; d.aqLevel = 1;
        d.presMode = 2; d.pres = "998"; d.ptrend = -1;
        d.outHum = "92"; d.feels = "8"; d.wind = "7"; d.outT = 9;
    } else if (mode == "dampstuffy") {
        // Damp AND stuffy at once — the case the old cascade could not report: «Damp» took the
        // word and the CO2 verdict vanished. Now both are on the glass.
        d.humDamp = true; d.hum = "73"; d.temp = "22.1"; d.co2 = "1340";
        d.aq = "Stuffy"; d.aqLevel = 2; d.aqInvert = true;
        d.outHum = "80"; d.feels = "10"; d.wind = "3"; d.outT = 10;
    } else if (mode == "mmhg") {
        d.pres = "813"; d.presU = "mmHg"; d.temp = "125.5"; d.co2 = "400"; d.hum = "3";
        d.outT = 125; d.feels = "125"; d.outHum = "3"; d.wind = "0";
    } else if (mode == "strip_sun") {
        // The owner's own strip: sunrise+sunset in one cell, the moon, and UV. The clock strings are
        // the WIDEST a day can produce ("00:00"/"20:00"), because "07:41" carries a '1' and a ':'
        // and is among the narrowest — designing against it is how the first cut overflowed.
        d.strip = (uint32_t)STRIP_SUN | ((uint32_t)STRIP_MOON << 8) | ((uint32_t)STRIP_UV << 16);
        d.sunrise = "00:00"; d.sunset = "20:00"; d.moon = 0.22f; d.uv = "11";
    } else if (mode == "strip_mixed") {
        // A widget beside numbers: the numbers must keep size 3, the moon must not drag them down.
        d.strip = (uint32_t)STRIP_MOON | ((uint32_t)STRIP_HUM << 8) | ((uint32_t)STRIP_FEELS << 16);
        d.moon = 0.5f; d.outHum = "100"; d.feels = "-12";
    } else if (mode == "strip_wide") {
        // "-40" is 48 px at size 3 and drops the WHOLE row to size 2 — the data deciding the size,
        // not the option. "100" next to it does not, because it is three plain digits and no sign.
        // Feels like is the only surviving cell that can go negative, so it carries the worst case.
        d.strip = (uint32_t)STRIP_FEELS | ((uint32_t)STRIP_HUM << 8) | ((uint32_t)STRIP_WIND << 16);
        d.feels = "-40"; d.outHum = "100"; d.wind = "12";
    } else if (mode == "strip_web") {
        // For the website guide: a weather-led strip with no widget in it, so the whole row is
        // numbers at one size. Rendered here rather than mocked up, for the reason the whole
        // screenhost exists — a hand-drawn picture of the screen starts lying the day the screen
        // changes, and this one lives on a public page where nobody would notice.
        d.strip = (uint32_t)STRIP_UV | ((uint32_t)STRIP_HUM << 8) | ((uint32_t)STRIP_FEELS << 16);
        d.uv = "4"; d.outHum = "64"; d.feels = "13";
    } else if (mode == "strip_nodata") {
        // No weather and no clock: the sun cell has nothing to print, the moon nothing to draw, and
        // the number falls back to dashes. This is the strip in Ultra Save on a device that has
        // never been online.
        d.strip = (uint32_t)STRIP_SUN | ((uint32_t)STRIP_MOON << 8) | ((uint32_t)STRIP_HUM << 16);
        d.sunrise = nullptr; d.sunset = nullptr; d.moon = -1.0f; d.outHum = nullptr;
    } else if (mode == "empty") {
        // Exactly what bench_screens.cpp passes when there is no weather: city is nullptr, and the
        // block draws its own "No weather yet". An empty string is a state the firmware never sends.
        d.co2 = "---"; d.err = true; d.temp = "--"; d.hum = "--"; d.pres = "--";
        d.aq = "Error"; d.aqLevel = -1; d.cal = nullptr;
        d.city = nullptr; d.desc = "--"; d.wxIcon = "01d"; d.outT = 0;
        d.mmLo = "--"; d.mmHi = "--";
        d.outHum = "--"; d.wind = "--"; d.feels = "--"; d.fcN = 0;
        d.date = "--.--.----"; d.time = "--:--"; d.batt = -1; d.wifiFail = true;
    } else if (mode == "night") {
        // Damp lives in the humidity block since 2026-10-02, so the air verdict here is the gas
        // band the device would really be showing — this state used to assert one the firmware can
        // no longer produce, which is how screenhost grows false findings.
        d.night = true; d.batt = 4; d.lowbat = true;
        d.humDamp = true; d.hum = "76";
        d.aq = "Heavy"; d.aqLevel = 4; d.aqInvert = true;
        // pres carries the NUMBER; the word comes from presMode
        d.presMode = 2; d.pres = "994"; d.ptrend = -1; d.co2 = "1680"; d.fresh = "12m";
    } else if (mode == "stuffy") {
        // For the website: an evening with the door shut. CO2 over the owner's level turns the air
        // block black, while its seven cells keep showing the gas class (here Good) underneath.
        d.time = "21:14"; d.co2 = "1650"; d.fresh = "2m"; d.temp = "23.4"; d.hum = "52";
        d.aq = "Stuffy"; d.aqLevel = 1; d.aqInvert = true;
        static float R[48];
        for (int i = 0; i < 48; i++) R[i] = 430.0f + 1224.0f * (i / 47.0f) * (i / 47.0f)
                                            + 40.0f * __builtin_sinf(i * 1.3f);
        d.hist = R; d.histN = 48; d.tStart = "07:41"; d.tEnd = "21:14";
    } else if (mode == "moderate") {
        // For the website: cooking. The gas sensor sees fumes that CO2 cannot, so the verdict is
        // Moderate rather than Stuffy, and the block stays white (an advisory, not a warning).
        d.time = "18:42"; d.co2 = "840"; d.fresh = "1m"; d.temp = "23.9"; d.hum = "55";
        d.aq = "Moderate"; d.aqLevel = 3;
    } else if (mode == "strip_gsun") {
        // For the website guide: the sun, the moon and UV on an ordinary day.
        d.strip = (uint32_t)STRIP_SUN | ((uint32_t)STRIP_MOON << 8) | ((uint32_t)STRIP_UV << 16);
        d.sunrise = "07:08"; d.sunset = "19:31"; d.moon = 0.22f; d.uv = "4";
    } else if (mode == "charge") {
        d.chg = true; d.usb = true; d.batt = 100; d.updClock = true; d.time = "14:05";
        d.presMode = 1; d.pres = "1003"; d.ptrend = -1; d.aq = "Warm-up"; d.aqLevel = -1; d.cal = "0/3";
    } else if (mode == "offline") {
        d.noNet = 2; d.city = nullptr; d.fcN = 0;
        d.tStart = "07:41"; d.tMid = "13:41"; d.tEnd = "19:41";
    } else if (mode == "bandoffline") {
        // The offline layout draws the same band from its own branch. Without this state a change
        // to that copy went unnoticed — which is how the duplicate was found.
        d.noNet = 2; d.city = nullptr; d.fcN = 0;
        d.tStart = "07:41"; d.tMid = "13:41"; d.tEnd = "19:41";
        d.setupLine = "smartmeteo.local (192.168.100.200)";
        d.setupLine2 = "Qw3rty!@";
    } else if (mode == "bandnopw") {
        // No password to show: NVS would not keep one, so the band falls back to one row.
        d.setupLine = "192.168.4.1";
        d.setupLine2 = nullptr;
        d.fcN = 0;
    }
    static float H[48];
    for (int i = 0; i < 48; i++) H[i] = 430.0f + 620.0f * (0.5f + 0.5f * __builtin_sinf(i / 5.0f));
    if (!d.hist) { d.hist = H; d.histN = 48; d.tStart = d.tStart ? d.tStart : "07:41"; d.tEnd = d.tEnd ? d.tEnd : "19:41"; }

    EPaper epd;
    epd.init();
    epd.clear();
    // The two screens of switching off are not the dashboard: they take a name and nothing else.
    if (mode == "off" || mode == "offconfirm" || mode == "offlongname") {
        if (mode == "off")            drawOffScreen(epd, "Living room");
        else if (mode == "offlongname") drawOffScreen(epd, "Bedroom upstairs on the left by the window");
        else                          drawOffConfirmScreen(epd, "Living room");
        char p2[128]; snprintf(p2, sizeof(p2), "out_%s.pgm", mode.c_str());
        write_pgm(epd, p2);
        printf("saved %s\n", p2);
        return 0;
    }
    // The CO2 calibration, which is its own screen for the same reason: the owner is outside with
    // the device and nothing else can reach them. Four states, the ones the run actually passes
    // through — not a sampler: each is a real combination the firmware produces.
    if (mode.rfind("calib", 0) == 0) {
        CalibScreen c{};
        if (mode == "calib_settling") {
            c.phase = 0; c.value = 412; c.ran_min = 6; c.trail_n = 6;
            const int16_t t[6] = {470, 448, 430, 419, 414, 412};
            memcpy(c.trail, t, sizeof(t));
        } else if (mode == "calib_measuring") {
            c.phase = 1; c.value = 414; c.ran_min = 11; c.done = 4; c.trail_n = 4;
            const int16_t t[4] = {412, 415, 411, 414};
            memcpy(c.trail, t, sizeof(t));
        } else if (mode == "calib_done") {
            c.phase = 2; c.value = 420; c.ran_min = 13; c.done = 6; c.trail_n = 6; c.corr = -350;
            const int16_t t[6] = {412, 415, 411, 414, 413, 416};
            memcpy(c.trail, t, sizeof(t));
        } else {
            c.phase = 3; c.value = -1; c.why = "nothing was changed - the sensor did not answer";
        }
        drawCalibScreen(epd, c);
        char p3[128]; snprintf(p3, sizeof(p3), "out_%s.pgm", mode.c_str());
        write_pgm(epd, p3);
        printf("saved %s\n", p3);
        return 0;
    }
    drawMainScreen(epd, d);
    char path[128]; snprintf(path, sizeof(path), "out_%s.pgm", mode.c_str());
    write_pgm(epd, path);
    printf("saved %s\n", path);
    return 0;
}
