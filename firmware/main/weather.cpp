#include "weather.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "https_client.h"
#include "ArduinoJson.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include "esp_app_desc.h"

static const char* TAG = "WEATHER";

// URL-encode a city string (spaces, commas are fine for OWM but encode spaces)
static std::string urlEncode(const std::string &s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == ',' || c == '-' || c == '_' || c == '.') {
            out += (char)c;
        } else if (c == ' ') {
            out += "%20";
        } else {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Client buffers as this module has always sized them; the timeout and the body deadline are the
// defaults.
static const Https::Options WX = { 8000, 1536, 1536, 15LL * 1000 * 1000, nullptr, nullptr };

// A GET whose body has to arrive whole. Returns the HTTP status (>0), or -1 on a transport error or a
// body cut short; `out_len` gets the number of bytes read into `buf`.
static int httpsGet(const std::string &url, char *buf, int buf_cap, int &out_len) {
    const Https::Body b = Https::get(url.c_str(), buf, buf_cap, WX);
    out_len = b.len;
    if (b.err != ESP_OK) return -1;
    if (!b.complete) {
        ESP_LOGW(TAG, "HTTPS body incomplete after %d bytes", b.len);
        return -1;
    }
    return b.status;
}

bool Weather::fetch(const std::string &city, const std::string &apikey,
                    float &temp, std::string &desc, std::string &icon, std::string &name,
                    int &tz_offset_sec,
                    float &feels, float &tmin, float &tmax, int &humidity, float &wind,
                    float *sea) {
    if (city.empty() || apikey.empty()) return false;

    // HTTPS so the API key is never transmitted in clear text.
    std::string url = "https://api.openweathermap.org/data/2.5/weather?q=" +
                      urlEncode(city) + "&appid=" + apikey + "&units=metric&lang=en";

    std::vector<char> buf(2048);   // heap, not stack (TLS handshake needs the stack)
    int len = 0;
    int status = httpsGet(url, buf.data(), (int)buf.size(), len);

    if (status != 200) {
        ESP_LOGW(TAG, "OpenWeatherMap HTTP %d (bad key or city?)", status);
        return false;
    }

    // Parse only the fields we need (filter keeps memory small)
    JsonDocument filter;
    filter["weather"][0]["description"] = true;
    filter["weather"][0]["icon"] = true;
    filter["main"]["temp"] = true;
    filter["main"]["feels_like"] = true;
    filter["main"]["temp_min"] = true;
    filter["main"]["temp_max"] = true;
    filter["main"]["humidity"] = true;
    filter["main"]["sea_level"] = true;
    filter["wind"]["speed"] = true;
    filter["name"] = true;
    filter["timezone"] = true;

    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, buf.data(), DeserializationOption::Filter(filter));
    if (e) {
        ESP_LOGW(TAG, "Weather JSON parse failed: %s", e.c_str());
        return false;
    }
    temp = doc["main"]["temp"] | -999.0f;
    desc = doc["weather"][0]["description"] | "";
    icon = doc["weather"][0]["icon"] | "";
    name = doc["name"] | "";
    tz_offset_sec = doc["timezone"] | 0;   // OWM: shift from UTC (seconds)
    feels    = doc["main"]["feels_like"] | temp;
    tmin     = doc["main"]["temp_min"]   | temp;
    tmax     = doc["main"]["temp_max"]   | temp;
    humidity = doc["main"]["humidity"]   | 0;
    if (sea) *sea = doc["main"]["sea_level"] | -1.0f;   // sentinel when OWM omits it
    wind     = doc["wind"]["speed"]      | 0.0f;
    if (temp > -100.0f && !desc.empty()) {
        desc[0] = toupper((unsigned char)desc[0]);  // capitalize first letter
        ESP_LOGI(TAG, "Weather for %s: %.1f C, %s (%s)",
                 name.c_str(), temp, desc.c_str(), icon.c_str());
        return true;
    }
    return false;
}

// ---- one picture and one range per day, from the three-hour steps ------------------------------------------
// The picture (owner, 2026-09-14). Only the daytime steps count, as OpenWeatherMap marks them (sys.pod "d").
// Precipitation outranks cloud when it is likely: a thunderstorm in any daytime step makes a thunderstorm day,
// then snow; rain or showers need a step whose probability of precipitation (pop) is at least 50 % —
// OpenWeatherMap draws rain for a 34 % chance as well (its own example response), and without the bar most
// days would carry rain. Otherwise the day gets the sky it has most often; fog only when fog holds more than
// half of the daytime; a tie goes to the step nearest noon. A day with no daytime steps at all (polar night) is
// judged on all of its steps.
struct DayTally {
    int  steps = 0;
    bool thunder = false, snow = false, rain = false, showers = false;
    int  count[5] = {0, 0, 0, 0, 0};          // 01 clear, 02 few, 03 scattered, 04 broken/overcast, 50 fog
    int  nearest[5] = {99, 99, 99, 99, 99};   // hours from noon of the closest step with that sky
};

static void tally_step(DayTally &t, const char *icon, float pop, int clouds, int hour)
{
    t.steps++;
    const int code = (icon[0] - '0') * 10 + (icon[1] - '0');
    int sky = -1;
    switch (code) {
        case 1:  sky = 0; break;
        case 2:  sky = 1; break;
        case 3:  sky = 2; break;
        case 4:  sky = 3; break;
        case 50: sky = 4; break;
        case 11: t.thunder = true; break;
        case 13: t.snow = true; break;
        case 10: if (pop >= 0.5f) t.rain = true; break;
        case 9:  if (pop >= 0.5f) t.showers = true; break;
        default: break;
    }
    // A wet step that does not earn its own picture still says how cloudy the sky is, by the cloud-cover bands
    // OpenWeatherMap gives its own icons: 801 few clouds 11-25 %, 802 scattered 25-50 %, 803-804 51-100 %.
    if (sky < 0 && (code == 9 || code == 10 || code == 11 || code == 13))
        sky = clouds < 0 ? 3 : clouds <= 10 ? 0 : clouds <= 25 ? 1 : clouds <= 50 ? 2 : 3;
    if (sky >= 0) {
        t.count[sky]++;
        const int dist = hour > 12 ? hour - 12 : 12 - hour;
        if (dist < t.nearest[sky]) t.nearest[sky] = dist;
    }
}

static void pick_icon(const DayTally &t, char out[4])
{
    static const char *SKY[5] = { "01", "02", "03", "04", "50" };
    if (t.steps == 0) return;                    // nothing known: the row shows "--" for the day
    const char *c;
    if (t.thunder)                      c = "11";
    else if (t.snow)                    c = "13";
    else if (t.rain)                    c = "10";
    else if (t.showers)                 c = "09";
    else if (t.count[4] * 2 > t.steps)  c = "50";
    else {
        int best = -1;
        for (int k = 0; k < 4; k++) {
            if (!t.count[k]) continue;
            if (best < 0 || t.count[k] > t.count[best] ||
                (t.count[k] == t.count[best] && t.nearest[k] < t.nearest[best])) best = k;
        }
        c = best >= 0 ? SKY[best] : (t.count[4] ? "50" : "04");
    }
    out[0] = c[0]; out[1] = c[1]; out[2] = 'd'; out[3] = '\0';   // always the day picture
}

// The range: the forecast temperature (main.temp) at each step. temp_min and temp_max are not the step's range:
// OpenWeatherMap describes them as the spread "at the moment of calculation ... (within large megalopolises and
// urban areas), use this parameter optionally", and taking them widened every day by up to a few degrees. Eight
// steps a day can still miss the true low and high by about a degree; this forecast holds nothing finer.
static void summarise_forecast(JsonArrayConst list, int tz_offset_sec, int32_t today, float &tmin, float &tmax,
                               WxDay *days, int max_days, int &n_days)
{
    enum { MAX_DAYS = 4 };
    if (max_days > MAX_DAYS) max_days = MAX_DAYS;
    DayTally tally[MAX_DAYS][2];                 // [day][0] its daytime steps, [day][1] all of them
    n_days = 0;
    for (JsonObjectConst it : list) {
        const long long dt = it["dt"] | 0LL;
        if (dt <= 0) continue;
        const float t = it["main"]["temp"] | -999.0f;
        if (t < -100.0f || t > 100.0f) continue;
        const long long local = dt + tz_offset_sec;
        const int32_t day = (int32_t)(local / 86400);
        const int hour = (int)((local % 86400) / 3600);
        if (day < today) continue;
        if (day == today) {
            if (t < tmin) tmin = t;
            if (t > tmax) tmax = t;
            continue;
        }
        // The entries are in time order, so a new date opens the next day. The days that fit are always whole
        // ones: forty steps reach 120 h ahead, and four full days after today end inside that however late in
        // the day the request is made.
        if (n_days == 0 || days[n_days - 1].day != day) {
            if (n_days >= max_days) break;
            WxDay &nd = days[n_days];
            nd.day = day; nd.lo = 999.0f; nd.hi = -999.0f; nd.icon[0] = '\0';
            tally[n_days][0] = DayTally(); tally[n_days][1] = DayTally();
            n_days++;
        }
        WxDay &d = days[n_days - 1];
        if (t < d.lo) d.lo = t;
        if (t > d.hi) d.hi = t;
        const char *ic = it["weather"][0]["icon"] | (const char *)nullptr;
        if (ic && isdigit((unsigned char)ic[0]) && isdigit((unsigned char)ic[1])) {
            const float pop = it["pop"] | 0.0f;
            const int clouds = it["clouds"]["all"] | -1;
            const char *pod = it["sys"]["pod"] | "";
            tally_step(tally[n_days - 1][1], ic, pop, clouds, hour);
            if (pod[0] == 'd') tally_step(tally[n_days - 1][0], ic, pop, clouds, hour);
        }
    }
    for (int i = 0; i < n_days; i++)
        pick_icon(tally[i][0].steps ? tally[i][0] : tally[i][1], days[i].icon);
}
// ---- end of the per-day summary ----

bool Weather::forecast(const std::string &city, const std::string &apikey, int tz_offset_sec,
                       float &tmin, float &tmax, WxDay *days, int max_days, int &n_days) {
    n_days = 0;
    if (city.empty() || apikey.empty()) return false;
    const time_t nw = time(NULL);
    if (nw < 1600000000) return false;   // need a real clock to know which day is "today"

    // All forty 3-hour steps, five days ahead. The rest of today and the days after it come out of
    // this one request: asking for the outlook separately would mean a second TLS handshake in the
    // same window, and it is the handshake the radio pays for, not the extra kilobytes.
    std::string url = "https://api.openweathermap.org/data/2.5/forecast?q=" +
                      urlEncode(city) + "&appid=" + apikey + "&units=metric&lang=en&cnt=40";
    int status = -1;
    esp_http_client_handle_t client = Https::open(url.c_str(), status, WX);
    if (!client) return false;
    if (status != 200) {
        ESP_LOGW(TAG, "Forecast HTTP %d", status);
        Https::close(client);
        return false;
    }

    JsonDocument filter;
    filter["list"][0]["dt"] = true;
    filter["list"][0]["main"]["temp"] = true;
    filter["list"][0]["weather"][0]["icon"] = true;
    filter["list"][0]["clouds"]["all"] = true;
    filter["list"][0]["pop"] = true;
    filter["list"][0]["sys"]["pod"] = true;
    JsonDocument doc;
    Https::Reader in(client, esp_timer_get_time() + WX.transfer_us);
    const DeserializationError e = deserializeJson(doc, in, DeserializationOption::Filter(filter));
    Https::close(client);
    if (e) {
        ESP_LOGW(TAG, "Forecast JSON parse failed: %s", e.c_str());
        return false;
    }

    // Local calendar days, from OWM's own offset for the city.
    const int32_t today = (int32_t)(((long long)nw + tz_offset_sec) / 86400);
    summarise_forecast(doc["list"].as<JsonArrayConst>(), tz_offset_sec, today, tmin, tmax, days, max_days, n_days);
    ESP_LOGI(TAG, "Forecast: today %.0f..%.0f, %d day(s) ahead", tmin, tmax, n_days);
    for (int i = 0; i < n_days; i++)
        ESP_LOGI(TAG, "  day %ld: %.0f..%.0f %s", (long)days[i].day, days[i].lo, days[i].hi, days[i].icon);
    return true;
}

WxResult Weather::validate(const std::string &city, const std::string &apikey,
                           float &temp, std::string &desc, std::string &name) {
    if (city.empty()) return WxResult::CityBad;
    if (apikey.empty()) return WxResult::KeyBad;

    std::string url = "https://api.openweathermap.org/data/2.5/weather?q=" +
                      urlEncode(city) + "&appid=" + apikey + "&units=metric&lang=en";
    std::vector<char> buf(2048);   // heap, not stack
    int len = 0;
    int status = httpsGet(url, buf.data(), (int)buf.size(), len);

    if (status == 401) return WxResult::KeyBad;     // OWM: invalid API key
    if (status == 404) return WxResult::CityBad;    // OWM: city not found
    if (status != 200) return WxResult::NetErr;

    JsonDocument filter;
    filter["weather"][0]["description"] = true;
    filter["main"]["temp"] = true;
    filter["name"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, buf.data(), DeserializationOption::Filter(filter))) return WxResult::NetErr;

    temp = doc["main"]["temp"] | -999.0f;
    desc = doc["weather"][0]["description"] | "";
    name = doc["name"] | "";
    if (!desc.empty()) desc[0] = toupper((unsigned char)desc[0]);
    return WxResult::Ok;
}

// Our own city search (tools/vps/geo): GeoNames' populated places from 500 inhabitants, with coordinates
// and IANA zones, on the update host. The data is CC BY 4.0 and ours to serve commercially, which Open-Meteo's
// free geocoder is not — that one stays only as the fallback for when our server does not answer.
static const char *GEO_URL = "https://ota.smartmeteo.org/v1/geo/search";

static bool geocode_own(const std::string &query, std::vector<GeoCity> &out, int n, const char *cc) {
    char tail[24];
    snprintf(tail, sizeof(tail), "&n=%d%s%s", n, cc && cc[0] ? "&cc=" : "", cc && cc[0] ? cc : "");
    std::string url = std::string(GEO_URL) + "?q=" + urlEncode(query) + tail;
    std::vector<char> buf(4096);
    int len = 0;
    const int status = httpsGet(url, buf.data(), (int)buf.size(), len);
    if (status != 200) {
        ESP_LOGW(TAG, "own city search: HTTP %d", status);
        return false;
    }
    JsonDocument filter;
    JsonObject fr = filter["results"].add<JsonObject>();
    for (const char *k : {"name", "country", "state", "lat", "lon", "tz"}) fr[k] = true;
    JsonDocument doc;
    if (deserializeJson(doc, buf.data(), DeserializationOption::Filter(filter))) return false;
    JsonArrayConst results = doc["results"];
    if (results.isNull()) return false;               // not our answer
    for (JsonObjectConst r : results) {
        GeoCity g;
        g.name    = r["name"]    | "";
        g.country = r["country"] | "";
        g.state   = r["state"]   | "";
        g.lat     = r["lat"]     | 0.0f;
        g.lon     = r["lon"]     | 0.0f;
        g.tz      = r["tz"]      | "";
        if (!g.name.empty()) out.push_back(g);
    }
    return true;
}

static bool geocode_open_meteo(const std::string &query, std::vector<GeoCity> &out, int n, const char *cc) {
    // Open-Meteo geocoding — keyless, HTTPS. Its free use is non-commercial, hence second place.
    char tail[40];
    snprintf(tail, sizeof(tail), "&count=%d%s%s", n, cc && cc[0] ? "&countryCode=" : "", cc && cc[0] ? cc : "");
    std::string url = "https://geocoding-api.open-meteo.com/v1/search?name=" +
                      urlEncode(query) + tail + "&language=en&format=json";
    std::vector<char> buf(4096);   // heap, not stack (this one is large)
    int len = 0;
    int status = httpsGet(url, buf.data(), (int)buf.size(), len);
    if (status != 200) {
        ESP_LOGW(TAG, "Geocode HTTP %d", status);
        return false;
    }

    JsonDocument filter;
    filter["results"][0]["name"] = true;
    filter["results"][0]["country_code"] = true;
    filter["results"][0]["admin1"] = true;
    filter["results"][0]["latitude"] = true;
    filter["results"][0]["longitude"] = true;
    filter["results"][0]["timezone"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, buf.data(), DeserializationOption::Filter(filter))) return false;

    // A name with no match comes back without "results" at all: an answer, just an empty one.
    for (JsonObject r : doc["results"].as<JsonArray>()) {
        GeoCity g;
        g.name    = r["name"]         | "";
        g.country = r["country_code"] | "";
        g.state   = r["admin1"]       | "";
        g.lat     = r["latitude"]     | 0.0f;
        g.lon     = r["longitude"]    | 0.0f;
        g.tz      = r["timezone"]     | "";
        if (!g.name.empty()) out.push_back(g);
    }
    return true;
}

bool Weather::geocode(const std::string &query, std::vector<GeoCity> &out, int limit, const char *cc,
                      const char **source) {
    out.clear();
    if (source) *source = "";
    if (query.size() < 2) return false;
    const int n = limit < 1 ? 1 : (limit > 10 ? 10 : limit);
    // An empty list from our server is an answer, not a failure: it does not send the query elsewhere.
    if (geocode_own(query, out, n, cc)) {
        if (source) *source = "GeoNames";
        return true;
    }
    out.clear();
    ESP_LOGW(TAG, "own city search unavailable — asking Open-Meteo");
    if (geocode_open_meteo(query, out, n, cc)) {
        if (source) *source = "Open-Meteo";
        return true;
    }
    return false;
}

bool Weather::locate(const std::string &city, GeoCity &out) {
    // "Darmstadt,DE": the name goes to the search, the country picks among its matches — Open-Meteo
    // matches names only, and a whole "City,CC" string finds nothing.
    std::string name = city, cc;
    const size_t comma = city.find(',');
    if (comma != std::string::npos) {
        name = city.substr(0, comma);
        cc = city.substr(comma + 1);
        while (!cc.empty() && cc[0] == ' ') cc.erase(0, 1);
    }
    std::vector<GeoCity> found;
    char ccz[3] = {0};
    if (cc.size() == 2) { ccz[0] = cc[0]; ccz[1] = cc[1]; }
    if (!geocode(name, found, 10, ccz[0] ? ccz : nullptr) || found.empty()) return false;
    for (const GeoCity &g : found) {
        if (cc.empty() || strcasecmp(g.country.c_str(), cc.c_str()) == 0) { out = g; return true; }
    }
    return false;
}

// ================================================================================================
// MET Norway — Locationforecast 2.0 (api.met.no), owner's choice 2026-09-17: free and keyless, and its
// terms allow commercial use. What the terms ask of us, and where it is done:
//   * an identifying User-Agent (userAgent());
//   * no repeat before the response's Expires time (the caller keeps it, fetch_own_weather());
//   * coordinates with at most four decimals ("%.4f" below; five or more is answered with 403);
//   * credit for the data, CC BY 4.0 (the console, next to the source choice).
// "complete" rather than "compact": it carries the apparent temperature and each six-hour window's low
// and high, so neither has to be approximated here. It is ~62 KB for nine days, read as a stream one
// timeseries entry at a time, so memory holds one entry, not the response.

static const Https::Options &met_options(Https::HeaderCapture *cap)
{
    static Https::Options o;
    o.timeout_ms = 8000; o.rx_buffer = 1536; o.tx_buffer = 1536;
    o.transfer_us = 20LL * 1000 * 1000;
    o.user_agent = Weather::userAgent();
    o.capture = cap;
    return o;
}

const char *Weather::userAgent() {
    static char ua[64];
    if (!ua[0]) {
        const esp_app_desc_t *d = esp_app_get_description();
        snprintf(ua, sizeof(ua), "SmartMeteo/%s smartmeteo.org", d ? d->version : "0");
    }
    return ua;
}

// Days since 1970-01-01 for a calendar date (Howard Hinnant's days_from_civil).
static int32_t civil_days(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

// "2026-09-17T10:00:00Z"
static time_t parse_iso_utc(const char *s) {
    int Y, M, D, h, m, sec;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    return (time_t)civil_days(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + m * 60 + sec;
}

// "Thu, 17 Sep 2026 10:36:46 GMT" (RFC 9110 IMF-fixdate)
static time_t parse_http_date(const char *s) {
    static const char *MON[12] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    int D, Y, h, m, sec; char mon[4] = {0};
    if (!s || sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &D, mon, &Y, &h, &m, &sec) != 6) return 0;
    for (int i = 0; i < 12; i++)
        if (strcmp(mon, MON[i]) == 0)
            return (time_t)civil_days(Y, (unsigned)(i + 1), (unsigned)D) * 86400 + h * 3600 + m * 60 + sec;
    return 0;
}

// MET's weather symbols (github.com/metno/weathericons, weather/legend.csv; the two misspelt ids are MET's
// own and are what the API sends), mapped to the OpenWeatherMap icon codes the screen's bitmaps are keyed by:
// 01 clear, 02 fair, 03 partly cloudy, 04 cloudy, 09 showers, 10 rain, 11 thunder, 13 snow or sleet, 50 fog.
struct MetSymbol { const char *id; const char *text; const char *code; };
static const MetSymbol MET_SYMBOLS[] = {
    {"clearsky", "Clear sky", "01"},            {"fair", "Fair", "02"},
    {"partlycloudy", "Partly cloudy", "03"},    {"cloudy", "Cloudy", "04"},
    {"fog", "Fog", "50"},
    {"lightrainshowers", "Light rain showers", "09"},   {"rainshowers", "Rain showers", "09"},
    {"heavyrainshowers", "Heavy rain showers", "09"},
    {"lightrainshowersandthunder", "Light showers, thunder", "11"},
    {"rainshowersandthunder", "Showers and thunder", "11"},
    {"heavyrainshowersandthunder", "Heavy showers, thunder", "11"},
    {"lightsleetshowers", "Light sleet showers", "13"}, {"sleetshowers", "Sleet showers", "13"},
    {"heavysleetshowers", "Heavy sleet showers", "13"},
    {"lightssleetshowersandthunder", "Sleet showers, thunder", "11"},
    {"lightsleetshowersandthunder", "Sleet showers, thunder", "11"},
    {"sleetshowersandthunder", "Sleet showers, thunder", "11"},
    {"heavysleetshowersandthunder", "Sleet showers, thunder", "11"},
    {"lightsnowshowers", "Light snow showers", "13"},   {"snowshowers", "Snow showers", "13"},
    {"heavysnowshowers", "Heavy snow showers", "13"},
    {"lightssnowshowersandthunder", "Snow showers, thunder", "11"},
    {"lightsnowshowersandthunder", "Snow showers, thunder", "11"},
    {"snowshowersandthunder", "Snow showers, thunder", "11"},
    {"heavysnowshowersandthunder", "Snow showers, thunder", "11"},
    {"lightrain", "Light rain", "10"},          {"rain", "Rain", "10"},
    {"heavyrain", "Heavy rain", "10"},
    {"lightrainandthunder", "Light rain, thunder", "11"},
    {"rainandthunder", "Rain and thunder", "11"},
    {"heavyrainandthunder", "Heavy rain, thunder", "11"},
    {"lightsleet", "Light sleet", "13"},        {"sleet", "Sleet", "13"},
    {"heavysleet", "Heavy sleet", "13"},
    {"lightsleetandthunder", "Sleet and thunder", "11"},
    {"sleetandthunder", "Sleet and thunder", "11"},
    {"heavysleetandthunder", "Sleet and thunder", "11"},
    {"lightsnow", "Light snow", "13"},          {"snow", "Snow", "13"},
    {"heavysnow", "Heavy snow", "13"},
    {"lightsnowandthunder", "Snow and thunder", "11"},
    {"snowandthunder", "Snow and thunder", "11"},
    {"heavysnowandthunder", "Snow and thunder", "11"},
};

// "partlycloudy_night" -> "03n" and "Partly cloudy". False for a symbol not in the table.
static bool met_symbol(const char *sym, char icon[4], const char **text) {
    if (!sym || !sym[0]) return false;
    char base[40];
    size_t n = strcspn(sym, "_");
    if (n >= sizeof(base)) return false;
    memcpy(base, sym, n); base[n] = '\0';
    const bool night = strstr(sym, "_night") != nullptr;   // _day and _polartwilight draw as day
    for (const MetSymbol &m : MET_SYMBOLS) {
        if (strcmp(m.id, base) != 0) continue;
        icon[0] = m.code[0]; icon[1] = m.code[1]; icon[2] = night ? 'n' : 'd'; icon[3] = '\0';
        if (text) *text = m.text;
        return true;
    }
    ESP_LOGW(TAG, "MET symbol '%s' not in the table", sym);
    return false;
}

// ArduinoJson reads from anything with read() and readBytes(). This adds the one character of push-back
// the element loop needs to look for the next '{' without consuming it.
class PeekReader {
public:
    explicit PeekReader(Https::Reader &in) : in_(in) {}
    int read() {
        if (back_ >= 0) { const int c = back_; back_ = -1; return c; }
        return in_.read();
    }
    size_t readBytes(char *out, size_t n) {
        size_t got = 0;
        if (n && back_ >= 0) { out[got++] = (char)back_; back_ = -1; }
        return got + in_.readBytes(out + got, n - got);
    }
    void unread(int c) { back_ = c; }
    int next_token() {                       // the next character that is not white space
        int c;
        do { c = read(); } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
        return c;
    }
    bool skip_past(const char *pat) {        // consume everything up to and including `pat`
        const size_t len = strlen(pat);
        size_t m = 0;
        for (;;) {
            const int c = read();
            if (c < 0) return false;
            if (c == pat[m]) { if (++m == len) return true; }
            else m = (c == pat[0]) ? 1 : 0;
        }
    }
private:
    Https::Reader &in_;
    int back_ = -1;
};

bool Weather::met(float lat, float lon, int tz_offset_sec, WxNow &now, float &tmin, float &tmax,
                  WxDay *days, int max_days, int &n_days, time_t &expires, int &status) {
    n_days = 0; expires = 0; status = -1;
    const time_t nw = time(NULL);
    if (nw < 1600000000) return false;   // no clock, no "today" to sort the entries into
    enum { MAX_DAYS = 4 };
    if (max_days > MAX_DAYS) max_days = MAX_DAYS;

    char url[160];
    snprintf(url, sizeof(url),
             "https://api.met.no/weatherapi/locationforecast/2.0/complete?lat=%.4f&lon=%.4f",
             (double)lat, (double)lon);
    char exp_hdr[40] = {0};
    Https::HeaderCapture cap = { "Expires", exp_hdr, sizeof(exp_hdr) };
    const Https::Options &opt = met_options(&cap);
    esp_http_client_handle_t client = Https::open(url, status, opt);
    if (!client) return false;
    expires = parse_http_date(exp_hdr);
    if (status != 200) {
        ESP_LOGW(TAG, "MET Norway HTTP %d", status);
        Https::close(client);
        return false;
    }

    Https::Reader raw(client, esp_timer_get_time() + opt.transfer_us);
    PeekReader in(raw);
    // "timeseries": [ {entry}, {entry}, ... ]
    bool ok = in.skip_past("\"timeseries\"") && in.next_token() == ':' && in.next_token() == '[';
    if (!ok) {
        ESP_LOGW(TAG, "MET response has no timeseries");
        Https::close(client);
        return false;
    }

    JsonDocument filter;
    filter["time"] = true;
    JsonObject fd = filter["data"].to<JsonObject>();
    fd["instant"]["details"]["air_temperature"] = true;
    fd["instant"]["details"]["apparent_air_temperature"] = true;
    fd["instant"]["details"]["relative_humidity"] = true;
    fd["instant"]["details"]["wind_speed"] = true;
    fd["instant"]["details"]["ultraviolet_index_clear_sky"] = true;
    fd["instant"]["details"]["dew_point_temperature"] = true;
    fd["instant"]["details"]["cloud_area_fraction"] = true;
    fd["instant"]["details"]["air_pressure_at_sea_level"] = true;
    fd["next_1_hours"]["summary"]["symbol_code"] = true;
    fd["next_6_hours"]["summary"]["symbol_code"] = true;
    fd["next_6_hours"]["details"]["air_temperature_max"] = true;
    fd["next_6_hours"]["details"]["air_temperature_min"] = true;
    fd["next_12_hours"]["summary"]["symbol_code"] = true;

    const int32_t today = (int32_t)(((long long)nw + tz_offset_sec) / 86400);
    bool have_now = false;
    DayTally tally[MAX_DAYS];
    int best12[MAX_DAYS];                 // distance from 06:00 of the chosen 12-hour daytime symbol
    int entries = 0;
    for (;;) {
        const int c = in.next_token();
        if (c < 0) { ok = false; break; }            // cut short
        if (c == ']') break;
        if (c == ',') continue;
        in.unread(c);
        JsonDocument e;
        const DeserializationError err = deserializeJson(e, in, DeserializationOption::Filter(filter));
        if (err) { ESP_LOGW(TAG, "MET entry %d: %s", entries, err.c_str()); ok = false; break; }
        entries++;
        const time_t t = parse_iso_utc(e["time"] | "");
        if (t <= 0) continue;
        JsonObjectConst d = e["data"];
        const float temp = d["instant"]["details"]["air_temperature"] | -999.0f;
        const char *s1  = d["next_1_hours"]["summary"]["symbol_code"] | "";
        const char *s6  = d["next_6_hours"]["summary"]["symbol_code"] | "";
        const char *s12 = d["next_12_hours"]["summary"]["symbol_code"] | "";

        // Now: the first entry, which is the current hour.
        if (!have_now && temp > -100.0f) {
            now.temp  = temp;
            now.feels = d["instant"]["details"]["apparent_air_temperature"] | temp;
            now.hum   = (int)lroundf(d["instant"]["details"]["relative_humidity"] | 0.0f);
            now.wind  = d["instant"]["details"]["wind_speed"] | 0.0f;
            // Each keeps its sentinel when the key is absent: MET drops fields it has no value for.
            // Probability of precipitation used to be read here too, from next_1_hours. It is not
            // asked for any more: MET publishes it only inside the Nordics, so for this device it
            // was a field that could never arrive (verified live, 2026-09-28).
            now.uv    = d["instant"]["details"]["ultraviolet_index_clear_sky"] | -1.0f;
            now.p_sea = d["instant"]["details"]["air_pressure_at_sea_level"] | -1.0f;
            now.dew   = d["instant"]["details"]["dew_point_temperature"] | -999.0f;
            {
                const float cf = d["instant"]["details"]["cloud_area_fraction"] | -1.0f;
                now.cloud = (cf >= 0.0f) ? (int)lroundf(cf) : -1;
            }
            const char *text = nullptr;
            if (!met_symbol(s1[0] ? s1 : s6, now.icon, &text)) { strcpy(now.icon, "03d"); text = "--"; }
            now.desc = text;
            have_now = true;
        }

        const long long local = (long long)t + tz_offset_sec;
        const int32_t day = (int32_t)(local / 86400);
        const int hour = (int)((local % 86400) / 3600);
        if (day < today) continue;

        // A six-hour window counts for the day only when it lies wholly inside it.
        const float wmax = d["next_6_hours"]["details"]["air_temperature_max"] | -999.0f;
        const float wmin = d["next_6_hours"]["details"]["air_temperature_min"] | 999.0f;
        const bool window = (hour + 6 <= 24) && wmax > -100.0f && wmin < 100.0f;

        if (day == today) {
            if (temp > -100.0f) { if (temp < tmin) tmin = temp; if (temp > tmax) tmax = temp; }
            if (window)         { if (wmin < tmin) tmin = wmin; if (wmax > tmax) tmax = wmax; }
            continue;
        }
        if (n_days == 0 || days[n_days - 1].day != day) {
            if (n_days >= max_days) break;              // entries are in time order: the rest is later
            WxDay &nd = days[n_days];
            nd.day = day; nd.lo = 999.0f; nd.hi = -999.0f; nd.icon[0] = '\0';
            tally[n_days] = DayTally();
            best12[n_days] = 99;
            n_days++;
        }
        const int k = n_days - 1;
        WxDay &dd = days[k];
        if (temp > -100.0f) { if (temp < dd.lo) dd.lo = temp; if (temp > dd.hi) dd.hi = temp; }
        if (window)         { if (wmin < dd.lo) dd.lo = wmin; if (wmax > dd.hi) dd.hi = wmax; }
        // The day's picture: MET's own twelve-hour summary for the daytime, taken from the entry nearest
        // 06:00 local (it covers roughly 06-18). Without one, the daytime hourly or six-hourly symbols are
        // tallied the way the OpenWeatherMap steps are; MET's symbol is already a verdict, so a wet one
        // counts as certain.
        char ic[4];
        const int dist = hour > 6 ? hour - 6 : 6 - hour;
        if (s12[0] && hour <= 12 && dist < best12[k] && met_symbol(s12, ic, nullptr)) {
            best12[k] = dist;
            ic[2] = 'd';
            memcpy(dd.icon, ic, 4);
        }
        const char *sd = s1[0] ? s1 : s6;
        if (sd[0] && hour >= 6 && hour < 18 && met_symbol(sd, ic, nullptr))
            tally_step(tally[k], ic, 1.0f, -1, hour);
    }
    Https::close(client);
    for (int i = 0; i < n_days; i++) {
        if (!days[i].icon[0]) pick_icon(tally[i], days[i].icon);
        if (days[i].lo > 900.0f || days[i].hi < -900.0f) { n_days = i; break; }   // a day with no numbers ends the row
    }
    if (!have_now) return false;
    ESP_LOGI(TAG, "MET: %d entries, now %.1f C (feels %.1f) %s %s, today %.0f..%.0f, %d day(s) ahead",
             entries, now.temp, now.feels, now.icon, now.desc.c_str(), tmin, tmax, n_days);
    for (int i = 0; i < n_days; i++)
        ESP_LOGI(TAG, "  day %ld: %.0f..%.0f %s", (long)days[i].day, days[i].lo, days[i].hi, days[i].icon);
    // A stream cut short still leaves a usable "now" and the days it reached; the caller keeps what came.
    if (!ok) ESP_LOGW(TAG, "MET response ended early after %d entries", entries);
    return true;
}
