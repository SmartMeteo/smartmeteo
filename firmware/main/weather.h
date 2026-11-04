#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

// Outdoor weather. Two sources (NVS "wx_src"): MET Norway's Locationforecast 2.0, free and keyless,
// commercial use allowed (the default since 0.10.0), and OpenWeatherMap with the owner's own key.
// Requires an active internet connection (call only while Wi-Fi STA is connected). All requests go
// over HTTPS with the mbedTLS certificate bundle — the API key is never sent in clear text.
// Returns false on any error (no network, bad key, city not found, parse failure).

// Typed result for the setup-time validation (so the UI can tell the user
// exactly what's wrong).
enum class WxResult { Ok = 0, KeyBad = 1, CityBad = 2, NetErr = 3 };

// One geocoding suggestion (from Open-Meteo, keyless).
struct GeoCity {
    std::string name;     // "Darmstadt"
    std::string country;  // ISO-2 code, "DE"
    std::string state;    // admin1, "Hesse" (may be empty)
    float lat = 0, lon = 0;
    std::string tz;       // IANA zone, "Europe/Berlin" (the console turns it into a POSIX rule)
};

enum WxSource : uint8_t { WX_SRC_MET = 0, WX_SRC_OWM = 1 };

// Conditions now, as the screen and the console want them.
struct WxNow {
    float temp = -999.0f, feels = -999.0f, wind = 0.0f;
    int   hum = 0;
    // Extra cells of the configurable bottom strip (strip_cells.h). MET sends all four in the very
    // response the forecast already costs, so reading them is parsing work and nothing on the air.
    // Sentinels rather than zeroes: 0 % cloud is a real reading, "not sent" is not.
    float uv    = -1.0f;     // ultraviolet_index_clear_sky — a CLEAR-SKY upper bound, not a measurement
    float dew   = -999.0f;   // dew_point_temperature, °C
    int   cloud = -1;        // cloud_area_fraction, %
    // Pressure reduced to sea level, hPa. Not for the screen — the device learns its OWN altitude
    // factor from it (this value divided by what the barometer reads here), and that factor is what
    // turns a local reading into a level the forecast can be judged against. Sentinel when absent.
    float p_sea = -1.0f;
    std::string desc;     // "Partly cloudy"
    char  icon[4] = {0};  // OWM icon code, "03d" — the screen's bitmaps are keyed by it
};

// One day of the outlook. `day` is the local calendar day it describes, as days since 1 January
// 1970 counted in the location's own time — so it can be matched against today's date when drawn,
// instead of trusting its position in a list that may be a day old. 0 = empty slot.
struct WxDay {
    int32_t day;
    float   lo, hi;       // °C
    char    icon[4];      // OWM code for the daytime, "10d"
};

class Weather {
public:
    // Fetches current weather for `city` (e.g. "Berlin,DE") using `apikey`.
    // On success fills temp (°C), description, icon code (OWM, e.g. "01d") and
    // the resolved city name.
    static bool fetch(const std::string &city, const std::string &apikey,
                      float &temp, std::string &desc, std::string &icon, std::string &name,
                      int &tz_offset_sec,
                      float &feels, float &tmin, float &tmax, int &humidity, float &wind,
                      // Optional, and only for learning this device's altitude factor — OWM calls
                      // it main.sea_level. Without it a unit set to OWM would never learn the
                      // factor, and the barometric plaque would go silent on it for good.
                      float *sea = nullptr);

    // The 5-day/3-hour forecast, read once and serving two things. Every entry on TODAY's local
    // date widens tmin/tmax (widening only — CLAUDE.md 4.14). The days after today go into `days`,
    // up to `max_days` of them, earliest first: the low and high of the forecast temperature over
    // that day's entries, and one icon chosen from its daytime entries (weather.cpp, pick_icon). `n_days` gets the count. tz_offset_sec = OWM timezone offset (seconds).
    // Returns false on a transport or parse error; tmin/tmax are only ever widened.
    static bool forecast(const std::string &city, const std::string &apikey, int tz_offset_sec,
                         float &tmin, float &tmax, WxDay *days, int max_days, int &n_days);

    // Validate a city+key against OpenWeatherMap and, on success, return the
    // current temp/description/resolved name. Distinguishes bad-key (HTTP 401)
    // from unknown-city (HTTP 404) so the portal can guide the user.
    static WxResult validate(const std::string &city, const std::string &apikey,
                             float &temp, std::string &desc, std::string &name);

    // City autocomplete. Our own search first (ota.smartmeteo.org/v1/geo/search, GeoNames), Open-Meteo's
    // geocoder when ours does not answer — setup must not depend on one server. `cc` (ISO-2) narrows to a
    // country. `source`, when given, names who answered. Returns false when neither answered.
    static bool geocode(const std::string &query, std::vector<GeoCity> &out, int limit = 6,
                        const char *cc = nullptr, const char **source = nullptr);

    // The first match for a stored "City,CC" (or plain "City"), for a device that has a city but no
    // coordinates. Returns false when the geocoder fails or finds nothing.
    static bool locate(const std::string &city, GeoCity &out);

    // MET Norway, Locationforecast 2.0 "complete", in one request: the conditions now, today's low and
    // high (widened only, as forecast() does), and up to `max_days` days after today. `tz_offset_sec`
    // is the location's offset from UTC (MET works in UTC only). `expires` gets the response's Expires
    // time (0 if absent): MET's terms ask for no repeat before it. `status` gets the HTTP status.
    static bool met(float lat, float lon, int tz_offset_sec, WxNow &now, float &tmin, float &tmax,
                    WxDay *days, int max_days, int &n_days, time_t &expires, int &status);

    // The User-Agent MET requires: product, version and where to find its maker.
    static const char *userAgent();
};
