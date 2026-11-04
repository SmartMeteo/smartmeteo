#include <math.h>
#include <cmath>
#include "web_server.h"
#include "fw_version.h"
#include "ota_check.h"
#include "ota_install.h"
#include "bsec_manager.h"
#include "scd41.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "wifi_slots.h"
#include "nvs_manager.h"
#include "battery_spec.h"
#include "power_estimate.h"
#include "power_history.h"
#include "co2_alert.h"
#include "units.h"
#include "battery_source.h"
#include "i2c_manager.h"
#include "matter_mode.h"
#include "bench_screens.h"
#include "strip_cells.h"
#include "nvs_flash.h"
#include "ArduinoJson.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_idf_version.h"
#include "esp_private/esp_clk.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "driver/temperature_sensor.h"
#include <time.h>
#include <sys/time.h>
#include <strings.h>   // strncasecmp, for the Content-Type check
#include "esp_random.h"        // esp_fill_random, for the session token and the nonce
#include "esp_timer.h"         // the nonce and lockout clocks
#include "mbedtls/sha256.h"    // the console's challenge-response, so the password stays off the wire
#include <ctype.h>
#include <vector>
#include "hist_store.h"
#include "evt_log.h"
#include "weather.h"
#include "telegram.h"
#include "mqtt_publisher.h"

// Human-readable reset reason for the System panel
static const char* resetReasonStr() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "Power-on";
        case ESP_RST_SW:        return "Software";
        case ESP_RST_DEEPSLEEP: return "Deep Sleep";
        case ESP_RST_BROWNOUT:  return "Brown-out";
        case ESP_RST_PANIC:     return "Panic";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:       return "Watchdog";
        default:                return "Other";
    }
}

// Internal chip temperature sensor (installed lazily, kept enabled)
static float readChipTemp() {
    static temperature_sensor_handle_t ts = nullptr;
    if (!ts) {
        temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&c, &ts) != ESP_OK) { ts = nullptr; return 0.0f; }
        temperature_sensor_enable(ts);
    }
    float v = 0.0f;
    temperature_sensor_get_celsius(ts, &v);
    return v;
}

static const char* TAG = "WEB_SERVER";

// Any page open in the user's browser can POST to this device without being allowed to: a form-safe
// content type (text/plain, form-encoded) needs no permission from us, the browser just sends it,
// and a body of JSON labelled text/plain parses exactly like the real thing. The attacker never
// reads the reply, but does not need to — /api/factory acts on arrival.
//
// Insisting on application/json shuts that path. It is not a type a form can produce, so the browser
// must ask the device for permission first, and a page from another origin never gets an answer. Our
// own console sends the header on every POST, including the bodyless ones.
//
// Deliberately checked BEFORE the request is stamped as activity: a rejected request must not
// stretch the setup window.
static bool require_json(httpd_req_t *req) {
    char ct[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK ||
        strncasecmp(ct, "application/json", 16) != 0) {
        ESP_LOGW(TAG, "POST %s rejected: Content-Type '%s'", req->uri, ct);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected Content-Type: application/json");
        return false;
    }
    return true;
}

// The same barrier as require_json, for a body that is not JSON. Worth spelling out why it is a
// barrier at all: a browser will send a cross-origin POST without asking permission only when the
// content type is one a plain HTML form can produce — form-urlencoded, multipart, or text/plain.
// application/octet-stream is not among them, so a page on another site cannot quietly make the
// owner's browser push firmware at their device; it has to ask first, and the device never answers.
// That is the same property require_json buys, and it is a CSRF defence rather than authentication:
// what actually guards this endpoint is that the console only exists for three minutes after
// somebody physically pressed the button (4.34).
static bool require_octet_stream(httpd_req_t *req) {
    char ct[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK ||
        strncasecmp(ct, "application/octet-stream", 24) != 0) {
        ESP_LOGW(TAG, "POST %s rejected: Content-Type '%s'", req->uri, ct);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Expected Content-Type: application/octet-stream");
        return false;
    }
    return true;
}

// Linker symbols for embedded console.html
extern const uint8_t console_html_start[] asm("_binary_console_html_gz_start");
extern const uint8_t console_html_end[]   asm("_binary_console_html_gz_end");
// Home-screen icon, embedded rather than fetched: the setup access point has no internet, so
// anything the page needs must come from the device itself.
extern const uint8_t app_icon_png_start[] asm("_binary_app_icon_png_start");
extern const uint8_t app_icon_png_end[]   asm("_binary_app_icon_png_end");

// Global telemetry variables defined in main.cpp
extern uint32_t global_btn_noise;
extern float global_voltage;
extern "C" bool smart_meteo_on_usb(void);   // bench_screens.cpp: the USB_DETECT pin
extern float global_soc, global_soc_raw;
extern float global_crate;
extern uint16_t global_co2;
extern float global_s_temp;
extern float global_s_hum;
extern float global_b_temp;
extern float global_b_hum;
extern float global_b_pres;
extern float global_b_gas;
extern float global_iaq;
extern float global_static_iaq;
extern float global_gas_percentage;
extern uint8_t global_iaq_accuracy;
extern float global_stab_status;
extern float global_run_in_status;
extern float global_dewpoint;
extern int global_pollution_class;
extern std::string global_pollution_text;
extern uint32_t global_boot_count;
extern uint32_t global_battery_cycles;
extern int global_bat_state;
extern float global_bat_ttf_h;
extern bool global_bat_low;
extern uint8_t global_bat_alerts;
extern bool global_on_usb;
extern float global_out_temp, global_out_uv, global_out_dew;
extern float global_surface_rh;
extern float global_p_sea; extern float global_baro_k; extern uint8_t global_baro_src;
extern int global_out_cloud;
extern std::string global_out_desc;
extern std::string global_out_city;
extern std::string global_out_icon;
extern int global_plan_profile;
extern bool global_plan_night, global_plan_live_clock;
extern uint32_t global_plan_meas_min, global_plan_radio_min;
extern uint32_t global_plan_last_meas, global_plan_last_co2;
extern volatile uint32_t global_temp_seq;
extern int global_portal_remain; // seconds until portal timeout; -1 = not in portal / disabled
extern uint32_t global_last_http_ms; // bumped on every request: keeps the portal alive while the page is open
extern bool global_portal_keep;  // user asked to keep the portal open longer
extern volatile int global_calib_req;
extern volatile int global_calib_result;
extern volatile int global_calib_correction;
extern volatile int global_calib_samples;
extern volatile int global_calib_error;
extern volatile int global_calib_lo;
extern volatile int global_calib_hi;
extern volatile int global_calib_avg;
extern volatile int global_calib_confirm;
extern volatile int global_calib_unsettled;
extern volatile int global_calib_settled;
extern "C" void smart_meteo_ota_handoff(void);   // bench_screens.cpp
extern "C" void smart_meteo_security_note(const char *text);  // bench_screens.cpp: queued for Telegram
extern volatile bool global_weather_dirty;
extern volatile bool global_hub_dirty;
extern volatile bool global_screen_dirty;
extern volatile bool global_portal_close;
extern volatile bool global_factory_req;
extern volatile bool global_reboot_req;
extern volatile bool global_close_req;
extern volatile bool global_poweroff_req;
extern volatile int global_wifi_connect_req;
extern volatile int global_wifi_connect_state;
extern std::string global_wx_state;

WebServer& WebServer::getInstance() {
    static WebServer instance;
    return instance;
}

static int recv_body(httpd_req_t *req, char *buf, size_t cap);   // below, with postSaveHandler
static esp_err_t body_failed(httpd_req_t *req, int r);

// ---- Access control: the console asks for the device's own password -----------------------------
//
// Until 2026-09-18 there was none, and the code said so — it treated "the console only exists for
// three minutes after somebody pressed the button" as the control. That argument is available under
// EN 18031-1 (REDCA TGN 35 A.3 rejects "it is inside a home" on its own but accepts a physical
// button press as part of a broader measure), and it is written up in the technical file. It is not
// enough on its own: the same window is served on the HOME network, advertised by mDNS, where every
// other device on the LAN can reach it, and it hands out settings, credentials, a factory reset and
// a firmware slot.
//
// The password is the one already drawn per device, printed on the setup screen and encoded in its
// QR — nothing new for the owner to learn, nothing shared between units.
//
// The password itself never crosses the wire. The page asks for a nonce, hashes it with the
// password, and sends only the digest; the device compares against its own hash and returns a
// session token in a cookie. Plain HTTP is acceptable for a token that dies with the window; it
// would not be acceptable for the password, which also opens the access point.
//
// IT CANNOT BE SWITCHED OFF, and that is a legal constraint rather than a preference: Implementing
// Decision (EU) 2025/138 withdraws the presumption of conformity from a product that lets the user
// operate without setting a password, and REDCA TGN 35 Annex A.2 reads letting the user remove it
// later as the same defect. No "skip", no "continue without a password", no way to clear it.
#define AUTH_FAIL_LIMIT     5           // wrong answers before the door shuts for a while
#define AUTH_LOCK_US        (60LL * 1000000)
#define AUTH_NONCE_LIFE_US  (60LL * 1000000)

static char    s_session[33]     = {0};   // hex; empty means nobody is signed in
static char    s_nonce[33]       = {0};   // hex; single use
static int64_t s_nonce_at        = 0;
static int     s_auth_fails      = 0;
static int64_t s_auth_lock_until = 0;

// Radio is always up by the time the console runs, so esp_fill_random() is the true source here —
// the bootloader entropy path (wifi_manager.cpp) is for the pre-radio case and must not be used.
static void rand_hex(char *out, size_t hex_len) {
    uint8_t raw[16];
    const size_t n = (hex_len / 2 > sizeof(raw)) ? sizeof(raw) : hex_len / 2;
    esp_fill_random(raw, n);
    for (size_t i = 0; i < n; i++) snprintf(out + i * 2, 3, "%02x", raw[i]);
    out[n * 2] = '\0';
}

static void sha256_hex(const char *s, char out[65]) {
    uint8_t d[32] = {0};
    mbedtls_sha256((const uint8_t *)s, strlen(s), d, 0);
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
    out[64] = '\0';
}

// Compares every byte whatever happens: a compare that returns early leaks, through its timing,
// how much of a token was guessed correctly.
static bool equal_ct(const char *a, const char *b, size_t n) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff = (uint8_t)(diff | ((uint8_t)a[i] ^ (uint8_t)b[i]));
    return diff == 0;
}

static bool auth_locked() { return s_auth_lock_until && esp_timer_get_time() < s_auth_lock_until; }

static bool s_ap_noted = false;   // see served_on_own_ap(); reset with the rest of the window state

// Every window starts with nobody signed in: a token from a previous session, on a device that has
// changed hands or simply been put down, is not a credential.
void web_auth_reset() {
    s_session[0] = '\0';
    s_nonce[0]   = '\0';
    s_auth_fails = 0;
    s_auth_lock_until = 0;
    s_ap_noted   = false;
}

// ---- the device's own access point IS the answer to the password -------------------------------
// Reaching this page over the device's OWN network already required the password: that access point
// is WPA2 and its key is the same eight characters the console would ask for. The QR on the setup
// screen carries WIFI:S:...;P:<password>;; — scanning it JOINS the phone with that password, and
// the page then asked for it again, to be copied off the glass a second time. Owner's decision
// 2026-09-30: one proof, not two.
//
// This does not make the password optional and does not delete it. It still exists, still cannot be
// removed, and still guards the HOME-NETWORK path — there WPA2 belongs to the router and everyone
// already inside it is past that door. The set of people who can open the console over the access
// point is unchanged by this: it is exactly "whoever knows the eight characters", before and after.
//
// Decided from the LOCAL address of the socket — the address the client connected TO — never from
// the peer's. The local one is picked by lwIP's routing rather than by whoever is calling: a host on
// the home LAN cannot make its request arrive on 192.168.4.1, because the reply to any such
// handshake leaves through the access-point interface. And it is answered PER REQUEST, never once
// per window: during a setup window the device can be on both at once, since startAP() brings the
// interface up as APSTA and connectSTAKeepAP() joins the home network without dropping the AP.
static bool served_on_own_ap(httpd_req_t *req) {
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap || !esp_netif_is_netif_up(ap)) return false;   // no access point of ours, no shortcut
    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(ap, &ip) != ESP_OK || ip.ip.addr == 0) return false;

    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return false;
    struct sockaddr_storage ss = {};
    socklen_t len = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) return false;

    uint32_t local = 0;
    if (ss.ss_family == AF_INET) {
        local = ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
    } else if (ss.ss_family == AF_INET6) {
        // A dual-stack lwIP hands IPv4 sockets back as IPv4-mapped IPv6. Anything else on this
        // socket is a real IPv6 address, which our access point does not serve — so, not ours.
        const uint8_t *b = (const uint8_t *)&((struct sockaddr_in6 *)&ss)->sin6_addr;
        static const uint8_t v4map[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        if (memcmp(b, v4map, sizeof(v4map)) != 0) return false;
        memcpy(&local, b + 12, 4);
    } else {
        return false;
    }
    if (local != ip.ip.addr) return false;

    if (!s_ap_noted) {          // once per window: the log records who got in, not how often they ask
        s_ap_noted = true;
        EvtLog::getInstance().event(EVT_SEC, SEC_SIGNIN_OK | 0x200u);   // 0x200: the device's own AP
        ESP_LOGW(TAG, "console: opened over the device's own access point, whose WPA2 key is this password");
    }
    return true;
}


// ---- remembered browsers -------------------------------------------------------------------
// A console window opens only after somebody holds the button on the device for three seconds,
// and that is the physical gate the whole design rests on. Typing twelve random characters at
// every window on top of that is friction without a matching gain: whoever is at the device has
// already proved presence, and a browser that answered the password once can be recognised again.
//
// So a successful sign-in also hands the browser a second, long-lived cookie. The device keeps
// only the SHA-256 of it — a flash read gives a thief the hashes, not a usable token — and only
// four, so a household's phones fit and a lost one ages out. The password stays the root secret:
// changing it forgets every browser, and so does a factory reset.
//
// What this does NOT do is open a door: the trust cookie is worthless until somebody presses the
// button, because until then there is no web server listening at all.
#define TRUST_SLOTS 4
static uint8_t s_trust[TRUST_SLOTS][32];
static uint8_t s_trust_next = 0;
static bool    s_trust_read = false;

static void trust_load() {
    if (s_trust_read) return;
    s_trust_read = true;
    size_t len = sizeof(s_trust);
    if (!NVSManager::getInstance().getBlob("trust", (uint8_t *)s_trust, len) || len != sizeof(s_trust))
        memset(s_trust, 0, sizeof(s_trust));
    uint32_t nx = 0; NVSManager::getInstance().getU32("trust_n", nx, 0);
    s_trust_next = (uint8_t)(nx % TRUST_SLOTS);
}

static void trust_save() {
    NVSManager::getInstance().setBlob("trust", (const uint8_t *)s_trust, sizeof(s_trust));
    NVSManager::getInstance().setU32("trust_n", s_trust_next);
}

// Every remembered browser forgotten at once. Called when the password changes — the password is
// what the trust was derived from — and from the console's own button.
static void trust_forget_all() {
    trust_load();
    memset(s_trust, 0, sizeof(s_trust));
    s_trust_next = 0;
    trust_save();
}

static int trust_count() {
    trust_load();
    int n = 0;
    for (int i = 0; i < TRUST_SLOTS; i++)
        for (int j = 0; j < 32; j++) if (s_trust[i][j]) { n++; break; }
    return n;
}

// The token itself is never stored; its hash goes into the next slot, round robin.
static void trust_remember(const char *token) {
    trust_load();
    uint8_t d[32] = {0};
    mbedtls_sha256((const uint8_t *)token, strlen(token), d, 0);
    memcpy(s_trust[s_trust_next], d, 32);
    s_trust_next = (uint8_t)((s_trust_next + 1) % TRUST_SLOTS);
    trust_save();
}

static bool trust_known(const char *token) {
    if (!token || strlen(token) < 32) return false;
    trust_load();
    uint8_t d[32] = {0};
    mbedtls_sha256((const uint8_t *)token, strlen(token), d, 0);
    bool ok = false;
    for (int i = 0; i < TRUST_SLOTS; i++) {
        // Every slot compared, every byte compared: an early exit would say, in its timing, how
        // much of a token matched.
        volatile uint8_t diff = 0;
        for (int j = 0; j < 32; j++) diff = (uint8_t)(diff | (s_trust[i][j] ^ d[j]));
        bool empty = true;
        for (int j = 0; j < 32; j++) if (s_trust[i][j]) { empty = false; break; }
        if (!empty && diff == 0) ok = true;
    }
    return ok;
}

// One cookie out of the header, by name, without mistaking a name that merely ends the same way.
static bool cookie_value(httpd_req_t *req, const char *name, char *out, size_t cap) {
    char cookie[256] = {0};
    out[0] = '\0';
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return false;
    const size_t nl = strlen(name);
    for (const char *p = cookie; p && *p; ) {
        while (*p == ' ') p++;
        if (strncmp(p, name, nl) == 0 && p[nl] == '=') {
            const char *v = p + nl + 1;
            size_t k = 0;
            while (v[k] && v[k] != ';' && k + 1 < cap) { out[k] = v[k]; k++; }
            out[k] = '\0';
            return k > 0;
        }
        p = strchr(p, ';');
        if (p) p++;
    }
    return false;
}

// STATIC buffers, and that is not laziness: httpd_resp_set_hdr() stores the POINTER it is given
// (httpd_txrx.c) and reads it when the response is sent, so a cookie built on the stack of a helper
// would be read after that stack is gone. The server runs its requests in one task, so one buffer
// per cookie cannot be overwritten under a response that has not left yet.
static char s_ck_session[128];
static char s_ck_trust[160];

static void set_session_cookie(httpd_req_t *req) {
    snprintf(s_ck_session, sizeof(s_ck_session),
             "sm_auth=%s; Path=/; Max-Age=3600; HttpOnly; SameSite=Strict", s_session);
    httpd_resp_set_hdr(req, "Set-Cookie", s_ck_session);
}

static bool require_auth(httpd_req_t *req) {
    // Asked before the cookies: over the device's own access point WPA2 has already demanded this
    // very password, and nothing a cookie could add would be a stronger answer than that.
    if (served_on_own_ap(req)) return true;

    char cookie[192] = {0};
    if (s_session[0] &&
        httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) == ESP_OK) {
        // Walked cookie by cookie rather than searched for the substring: "sm_auth=" appearing
        // inside some other cookie's value must not be mistaken for ours.
        for (const char *p = cookie; p && *p; ) {
            while (*p == ' ') p++;
            if (strncmp(p, "sm_auth=", 8) == 0) {
                if (strlen(p + 8) >= 32 && equal_ct(p + 8, s_session, 32)) return true;
                break;
            }
            p = strchr(p, ';');
            if (p) p++;
        }
    }
    // No session — but perhaps this browser was here before. A remembered token is not a password
    // guess, so a stale or forged one costs nothing and counts towards no lockout: it simply falls
    // through to the sign-in screen below.
    {
        char tok[80] = {0};
        if (cookie_value(req, "sm_trust", tok, sizeof(tok)) && trust_known(tok)) {
            rand_hex(s_session, 32);
            set_session_cookie(req);
            EvtLog::getInstance().event(EVT_SEC, SEC_SIGNIN_OK | 0x100u);   // 0x100: remembered browser
            ESP_LOGW(TAG, "console: signed in by a remembered browser");
            return true;
        }
    }

    // 401 with a body the page can recognise: every fetch in the console treats this as "show the
    // sign-in screen" rather than "the device stopped answering".
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, "{\"auth\":\"required\"}", HTTPD_RESP_USE_STRLEN);
    return false;
}

// Change the device password. Signed in already — the session is proof of knowing the current one —
// so this only takes the new value, and the rules live in WiFiManager::setApPassword().
//
// EN 18031 AUM-4 asks for exactly this: an authenticator the owner can change. Note what it changes:
// ONE secret opens the console, the setup Wi-Fi and the QR code, and it is the one the screen prints.
// Two secrets would mean two things to lose.
static esp_err_t postPasswordHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();

    char body[128] = {0};
    const int r = recv_body(req, body, sizeof(body));
    if (r < 0) return body_failed(req, r);
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_OK;
    }
    const std::string np = doc["new"] | "";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!WiFiManager::setApPassword(np.c_str())) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"rules\"}");
        return ESP_OK;
    }
    // The session stays valid: the owner is standing there and has just proved it twice. Every
    // REMEMBERED browser is forgotten, though — the trust was granted on the strength of the old
    // password, and whoever changes the password may be doing it precisely because that one got out.
    trust_forget_all();
    EvtLog::getInstance().event(EVT_SEC, SEC_PASSWORD_SET);
    smart_meteo_security_note("the device password was changed from the console. "
                              "If this was not you, hold the button for 3 seconds — the new password "
                              "is printed on the screen — and change it again.");
    ESP_LOGW(TAG, "device password changed from the console");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// Forget every remembered browser. Offered because "this device is remembered" is a promise the
// owner must be able to take back — a phone sold, lent or lost — without changing the password and
// without a factory reset.
static esp_err_t postTrustForgetHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();
    const int had = trust_count();
    trust_forget_all();
    EvtLog::getInstance().event(EVT_SEC, SEC_PASSWORD_SET | 0x100u);   // 0x100: browsers forgotten
    ESP_LOGW(TAG, "forgot %d remembered browser(s)", had);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char out[48]; snprintf(out, sizeof(out), "{\"ok\":true,\"forgot\":%d}", had);
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t postAuthChallengeHandler(httpd_req_t *req) {
    if (auth_locked()) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"locked\":1}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    // A pending challenge is handed out again rather than replaced. Asking for one is something
    // anybody on the network can do, and minting a fresh nonce every time would let them wipe the
    // one the owner's browser is holding — locking the owner out of the console without ever
    // knowing the password. Reuse is safe: the nonce is spent the moment it is answered, and a
    // stale one dies on its own after AUTH_NONCE_LIFE_US.
    if (!s_nonce[0] || (esp_timer_get_time() - s_nonce_at) >= AUTH_NONCE_LIFE_US) {
        rand_hex(s_nonce, 32);
        s_nonce_at = esp_timer_get_time();
    }
    char out[64];
    snprintf(out, sizeof(out), "{\"nonce\":\"%s\"}", s_nonce);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t postAuthHandler(httpd_req_t *req) {
    if (!require_json(req)) return ESP_OK;
    if (auth_locked()) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"locked\":1}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char body[192] = {0};
    const int r = recv_body(req, body, sizeof(body));
    if (r < 0) return body_failed(req, r);

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_OK;
    }
    const std::string nonce = doc["nonce"] | "";
    const std::string resp  = doc["resp"]  | "";

    // "fresh" means this answer really was an attempt at the current challenge. Everything below
    // keys off it, because an answer that quotes no valid nonce is not a password guess: it cannot
    // test a password, so it must not spend the challenge and must not count towards the lockout.
    // Otherwise anyone on the network could keep the owner signed out with a stream of nonsense.
    const bool fresh = s_nonce[0] && nonce.size() == 32 && equal_ct(nonce.c_str(), s_nonce, 32) &&
                       (esp_timer_get_time() - s_nonce_at) < AUTH_NONCE_LIFE_US;
    char expect[65] = {0};
    if (fresh) {
        char material[96];
        snprintf(material, sizeof(material), "%s:%s", s_nonce, WiFiManager::apPassword());
        sha256_hex(material, expect);
        // Spent on the first answer that used it, right or wrong: one challenge must not be
        // guessed against indefinitely.
        s_nonce[0] = '\0';
    }

    if (fresh && resp.size() == 64 && equal_ct(resp.c_str(), expect, 64)) {
        rand_hex(s_session, 32);
        s_auth_fails = 0;
        // HttpOnly so a scripting bug in the page cannot read the token; SameSite=Strict so another
        // site cannot ride the session. No Secure flag: this is plain HTTP on a local network.
        set_session_cookie(req);
        // ...and a second cookie that outlives this window, so the next one does not ask again.
        // Thirty days in the browser; on the device it lasts until the password changes, the owner
        // forgets it, or three other browsers have signed in after it.
        {
            char tok[33]; rand_hex(tok, 32);
            trust_remember(tok);
            // A second Set-Cookie, not Set-Cookie2 — that header is RFC 2965 and no browser has
            // honoured it for twenty years. esp_http_server appends headers without merging names,
            // so two of them arrive as two.
            snprintf(s_ck_trust, sizeof(s_ck_trust),
                     "sm_trust=%s; Path=/; Max-Age=2592000; HttpOnly; SameSite=Strict", tok);
            httpd_resp_set_hdr(req, "Set-Cookie", s_ck_trust);
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, "{\"ok\":1}", HTTPD_RESP_USE_STRLEN);
        global_last_http_ms = esp_log_timestamp();
        EvtLog::getInstance().event(EVT_SEC, SEC_SIGNIN_OK);
        ESP_LOGW(TAG, "console: signed in");
        return ESP_OK;
    }

    if (!fresh) {
        // No challenge behind it: nothing was tried, nothing is counted.
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":0,\"stale\":1}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (++s_auth_fails >= AUTH_FAIL_LIMIT) {
        s_auth_lock_until = esp_timer_get_time() + AUTH_LOCK_US;
        EvtLog::getInstance().event(EVT_SEC, SEC_SIGNIN_LOCKED);
        s_auth_fails = 0;
        ESP_LOGW(TAG, "console: %d wrong passwords — locked for %d s",
                 AUTH_FAIL_LIMIT, (int)(AUTH_LOCK_US / 1000000));
    } else {
        EvtLog::getInstance().event(EVT_SEC, SEC_SIGNIN_FAIL | ((uint32_t)s_auth_fails << 8));
        ESP_LOGW(TAG, "console: wrong password (%d/%d)", s_auth_fails, AUTH_FAIL_LIMIT);
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":0}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---- Matter: the setup code and leaving the smart home -----------------------------------------
// The console serves this only while it is open, and it is open only after somebody held the device's
// button or joined its access point: the code is printed on the device's own screen in Matter mode
// anyway. Wi-Fi is up whenever this runs, so the codes may be generated here (true random).
static esp_err_t getMatterHandler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    // The setup code is the key to joining this device to a smart home, and it stops being anybody's
    // business the moment it is already in one. That rule used to live in console.html — which means
    // it did not exist: the API answered anyone who asked it directly, in any mode. Note also that
    // codes(..., true) CREATES and stores the code on first call, so a plain GET used to write NVS.
    const int fabrics = MatterMode::fabrics();
    MatterMode::Codes c;
    const bool ok = (fabrics == 0) && MatterMode::codes(c, true);
    JsonDocument doc;
    doc["selected"] = MatterMode::selected();
    doc["fabrics"]  = fabrics;
    doc["forget_pending"] = MatterMode::forget_requested();
    doc["ok"]       = ok;
    if (ok) {
        doc["qr"]     = c.qr;
        doc["manual"] = c.manual_fmt;
    }
    std::string out;
    serializeJson(doc, out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, out.c_str(), out.length());
    return ESP_OK;
}

static esp_err_t getMatterQrHandler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    std::string svg;
    // Same rule as /api/matter: a device already in a smart home has no code to show.
    if (MatterMode::fabrics() > 0 || !MatterMode::qr_svg(svg, true)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no setup code");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, svg.c_str(), svg.length());
    return ESP_OK;
}

// Carried out by the next Matter start (run_matter_mode): the stack is not running in this session.
// {"cancel":true} withdraws a request that has not been carried out yet.
static esp_err_t postMatterForgetHandler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();
    char body[64];
    const int got = recv_body(req, body, sizeof(body));
    if (got < 0) return body_failed(req, got);
    bool cancel = false;
    if (got > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, body, got) == DeserializationError::Ok) cancel = doc["cancel"] | false;
    }
    MatterMode::request_forget(!cancel);
    if (!cancel) EvtLog::getInstance().event(EVT_SEC, SEC_MATTER_FORGET);
    ESP_LOGW(TAG, "%s", cancel ? "removal from the smart home withdrawn"
                               : "the smart-home pairing will be erased at the next Matter start");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, cancel ? "{\"ok\":true,\"forget_pending\":false}"
                                   : "{\"ok\":true,\"forget_pending\":true}");
    return ESP_OK;
}

bool WebServer::start() {
    if (server_handle != nullptr) {
        return true;
    }

    ESP_LOGI(TAG, "Starting HTTP Server on port 80...");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    // Must be at least the number of httpd_register_uri_handler() calls below, with room to spare.
    // The function returns an error when the table is full and NOTHING here checks it, so the last
    // endpoint registered simply does not exist: its URL answers 404 while the code that serves it
    // sits there looking correct. That is how /api/ota/install came to do nothing at all — added
    // without touching this number, which was exactly one short. Raise it when adding endpoints;
    // the cost is a few bytes of table.
    config.max_uri_handlers = 36;
    config.uri_match_fn = httpd_uri_match_wildcard;
    // As ESP-IDF's captive_portal example does: when every session is taken, close the least recently
    // used one instead of refusing new connections while phones hold idle keep-alive sockets. httpd
    // reserves three sockets for itself, the captive DNS holds one, and handlers open outbound clients
    // (weather test, geocode, MQTT test) — CONFIG_LWIP_MAX_SOCKETS is 16 against 10 sessions here.
    config.max_open_sockets = 10;
    config.lru_purge_enable = true;

    // A window is a session: whoever signed in last time does not stay signed in.
    web_auth_reset();

    if (httpd_start(&server_handle, &config) == ESP_OK) {
        // Register URI handlers
        httpd_uri_t get_root = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = getRootHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &get_root);

        httpd_uri_t get_icon = {
            .uri      = "/icon.png",
            .method   = HTTP_GET,
            .handler  = getIconHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &get_icon);

        httpd_uri_t get_manifest = {
            .uri      = "/manifest.webmanifest",
            .method   = HTTP_GET,
            .handler  = getManifestHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &get_manifest);

        httpd_uri_t get_status = {
            .uri      = "/api/status",
            .method   = HTTP_GET,
            .handler  = getStatusHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &get_status);

        httpd_uri_t get_scan = {
            .uri      = "/api/scan",
            .method   = HTTP_GET,
            .handler  = getScanHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &get_scan);

        httpd_uri_t post_save = {
            .uri      = "/api/save",
            .method   = HTTP_POST,
            .handler  = postSaveHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_save);

        httpd_uri_t post_reboot = {
            .uri      = "/api/reboot",
            .method   = HTTP_POST,
            .handler  = postRebootHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_reboot);

        httpd_uri_t post_poweroff = {
            .uri      = "/api/poweroff",
            .method   = HTTP_POST,
            .handler  = postPowerOffHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_poweroff);


        httpd_uri_t post_ota_check = {
            .uri      = "/api/ota/check",
            .method   = HTTP_POST,
            .handler  = postOtaCheckHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_ota_check);

        httpd_uri_t post_ota_install = {
            .uri      = "/api/ota/install",
            .method   = HTTP_POST,
            .handler  = postOtaInstallHandler,
            .user_ctx = NULL
        };
        // Checked, unlike the twenty-three around it, because this is the one that caught us. If
        // the table ever fills again the log says so instead of an endpoint quietly vanishing.
        if (httpd_register_uri_handler(server_handle, &post_ota_install) != ESP_OK)
            ESP_LOGE(TAG, "could not register /api/ota/install — raise max_uri_handlers");

        httpd_uri_t post_ota_revert = {
            .uri      = "/api/ota/revert",
            .method   = HTTP_POST,
            .handler  = postOtaRevertHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_ota_revert);

        httpd_uri_t post_ota = {
            .uri      = "/api/ota",
            .method   = HTTP_POST,
            .handler  = postOtaHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_ota);

        httpd_uri_t post_factory = {
            .uri      = "/api/factory",
            .method   = HTTP_POST,
            .handler  = postFactoryHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_factory);

        httpd_uri_t post_portal = {
            .uri      = "/api/portal",
            .method   = HTTP_POST,
            .handler  = postPortalHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_portal);

        httpd_uri_t post_calibrate = {
            .uri      = "/api/calibrate",
            .method   = HTTP_POST,
            .handler  = postCalibrateHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_calibrate);

        httpd_uri_t post_tempcal = {
            .uri      = "/api/tempcal",
            .method   = HTTP_POST,
            .handler  = postTempCalHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_tempcal);

        httpd_uri_t post_close = {
            .uri      = "/api/close",
            .method   = HTTP_POST,
            .handler  = postCloseHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_close);

        httpd_uri_t post_clock = {
            .uri      = "/api/clock",
            .method   = HTTP_POST,
            .handler  = postClockHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_clock);

        httpd_uri_t get_history = {
            .uri      = "/api/history",
            .method   = HTTP_GET,
            .handler  = getHistoryHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &get_history);

        httpd_uri_t get_events = {
            .uri      = "/api/events.bin",
            .method   = HTTP_GET,
            .handler  = getEventsHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &get_events);

        httpd_uri_t get_geocode = {
            .uri      = "/api/geocode",
            .method   = HTTP_GET,
            .handler  = getGeocodeHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &get_geocode);

        httpd_uri_t post_wxtest = {
            .uri      = "/api/weather/test",
            .method   = HTTP_POST,
            .handler  = postWeatherTestHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_wxtest);

        httpd_uri_t post_mqtest = {
            .uri      = "/api/mqtt/test",
            .method   = HTTP_POST,
            .handler  = postMqttTestHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_mqtest);

        httpd_uri_t post_tgtest = {
            .uri      = "/api/telegram/test",
            .method   = HTTP_POST,
            .handler  = postTelegramTestHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_tgtest);

        // Captive-portal detection endpoints: answering these with a redirect
        // makes phones pop the "sign in to Wi-Fi network" sheet automatically
        // right after joining the setup AP (Android probes /generate_204,
        // Apple probes /hotspot-detect.html; everything else falls into 404).
        httpd_uri_t get_captive = {
            .uri      = "/generate_204",
            .method   = HTTP_GET,
            .handler  = getCaptiveHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &get_captive);

        httpd_uri_t get_captive_apple = {
            .uri      = "/hotspot-detect.html",
            .method   = HTTP_GET,
            .handler  = getCaptiveHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &get_captive_apple);

        httpd_uri_t get_matter = {
            .uri      = "/api/matter",
            .method   = HTTP_GET,
            .handler  = getMatterHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &get_matter);

        httpd_uri_t get_matter_qr = {
            .uri      = "/api/matter/qr.svg",
            .method   = HTTP_GET,
            .handler  = getMatterQrHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &get_matter_qr);

        httpd_uri_t post_matter_forget = {
            .uri      = "/api/matter/forget",
            .method   = HTTP_POST,
            .handler  = postMatterForgetHandler,
            .user_ctx = NULL
        };
        if (httpd_register_uri_handler(server_handle, &post_matter_forget) != ESP_OK)
            ESP_LOGE(TAG, "could not register the Matter handlers — raise max_uri_handlers");

        httpd_uri_t post_trust_forget = {
            .uri      = "/api/trust/forget",
            .method   = HTTP_POST,
            .handler  = postTrustForgetHandler,
            .user_ctx = NULL
        };
        if (httpd_register_uri_handler(server_handle, &post_trust_forget) != ESP_OK)
            ESP_LOGE(TAG, "could not register /api/trust/forget — raise max_uri_handlers");

        httpd_uri_t post_password = {
            .uri      = "/api/password",
            .method   = HTTP_POST,
            .handler  = postPasswordHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_password);

        // Sign-in. These two are the only API endpoints without require_auth, for the obvious reason.
        httpd_uri_t post_auth_challenge = {
            .uri      = "/api/auth/challenge",
            .method   = HTTP_POST,
            .handler  = postAuthChallengeHandler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &post_auth_challenge);

        httpd_uri_t post_auth = {
            .uri      = "/api/auth",
            .method   = HTTP_POST,
            .handler  = postAuthHandler,
            .user_ctx = NULL
        };
        if (httpd_register_uri_handler(server_handle, &post_auth) != ESP_OK)
            ESP_LOGE(TAG, "could not register the sign-in handlers — raise max_uri_handlers");

        httpd_register_err_handler(server_handle, HTTPD_404_NOT_FOUND, redirect404Handler);

        ESP_LOGI(TAG, "HTTP Server started successfully!");
        return true;
    }

    ESP_LOGE(TAG, "Failed to start HTTP Server!");
    return false;
}

void WebServer::stop() {
    if (server_handle) {
        httpd_stop(server_handle);
        server_handle = nullptr;
        ESP_LOGI(TAG, "HTTP Server stopped.");
    }
}

// As ESP-IDF's captive_portal example: 303 to "/" with a body — "iOS requires content in the response to
// detect a captive portal, simply redirecting is not sufficient." A relative Location also works when the
// console is served on the home network, where 192.168.4.1 does not exist.
static esp_err_t redirect_to_console(httpd_req_t *req) {
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t WebServer::getCaptiveHandler(httpd_req_t *req) {
    return redirect_to_console(req);
}

esp_err_t WebServer::redirect404Handler(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    return redirect_to_console(req);
}

esp_err_t WebServer::getRootHandler(httpd_req_t *req) {
    size_t size = console_html_end - console_html_start;
    httpd_resp_set_type(req, "text/html");
    // The page and the firmware are one program split across two machines, and only one half gets
    // updated by a reflash. Cached, an old page keeps talking to a new API: buttons stop working
    // with no error anywhere, because the page is doing exactly what it was written to do. That is
    // how the Content-Type guard first appeared to be broken. Costs a re-download of a page the
    // device serves a few minutes a month — cheap next to a control that silently does nothing.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, must-revalidate");
    // Everything this page needs is inside it — no external script, style, font or image — so the
    // policy can be as narrow as it gets: nothing loads from anywhere, nobody frames it, and a form
    // cannot post it off the device. 'unsafe-inline' is unavoidable and honest: the whole console IS
    // one inline script and one inline stylesheet, and the guide's pictures are inline data URIs.
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                       "img-src 'self' data:; connect-src 'self'; manifest-src 'self'; "
                       "form-action 'none'; frame-ancestors 'none'; base-uri 'none'");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    // Asking for the page IS somebody being there, and it is the strongest evidence of it we
    // ever get. Without this line a RELOAD closed the console: the goodbye beacon started the
    // countdown, and the only thing that cancels it is activity — which nothing counted while
    // the browser spent those seconds downloading a hundred kilobytes it may not cache.
    global_last_http_ms = esp_log_timestamp();
    // Tell the page up front whether this device still needs setting up. The device already
    // knows; without saying so the page has to render something, ask /api/status and only then
    // switch — which showed a flash of the dashboard before the wizard on every first run.
    // A tiny prelude costs no extra request and lets the page open on the right screen at once.
    // "Configured" means a network we actually JOINED, not merely a string someone typed. A
    // mistyped name used to be saved and the device then stopped offering setup entirely.
    // The very question the cycle asks (device_configured()), not a copy of it. Two places answering
    // it with different rules once told a device set up without a network that it still needed
    // setting up, so the console opened the mandatory wizard every time — whose first step is to join
    // a network, the one thing that owner cannot do.
    const bool needs_setup = !device_configured();
    // Handed over as a cookie rather than as a <script> prepended to the body. The body is a gzip
    // stream now, and plain text cannot be glued to the front of one — the encoding applies to the
    // whole response. A cookie carries the same one bit, arrives with the same request, and the
    // page degrades to its old behaviour if it is ever missing: an absent flag already means
    // "decide from /api/status", which is where this started.
    httpd_resp_set_hdr(req, "Set-Cookie",
                       needs_setup ? "sm_setup=1; Path=/; Max-Age=120"
                                   : "sm_setup=0; Path=/; Max-Age=120");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char*)console_html_start, size);
    return ESP_OK;
}

// Consumption, measured rather than modelled: power_history.cpp reads the ring, power_estimate.cpp
// does the arithmetic. The hub asks the same function, so the card and Home Assistant agree.
static PowerEstimate::Result estimatePower() { return PowerHistory::estimate(); }

// The two files a browser needs before it will offer "Add to Home Screen". Both are cacheable for
// a long time — they change only with the firmware.
esp_err_t WebServer::getIconHandler(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=604800");
    return httpd_resp_send(req, (const char *)app_icon_png_start,
                           app_icon_png_end - app_icon_png_start);
}

esp_err_t WebServer::getManifestHandler(httpd_req_t *req) {
    // start_url is relative on purpose: the console lives at the router's address on the home
    // network and at 192.168.4.1 in setup mode, so an absolute one would be wrong half the time.
    static const char MANIFEST[] =
        "{\"name\":\"Smart Meteo\",\"short_name\":\"Smart Meteo\","
        "\"start_url\":\"./\",\"scope\":\"./\",\"display\":\"standalone\","
        "\"background_color\":\"#0B1220\",\"theme_color\":\"#0EA5A4\","
        "\"icons\":[{\"src\":\"icon.png\",\"sizes\":\"512x512\",\"type\":\"image/png\"},"
        "{\"src\":\"icon.png\",\"sizes\":\"512x512\",\"type\":\"image/png\","
        "\"purpose\":\"maskable\"}]}";
    httpd_resp_set_type(req, "application/manifest+json");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=604800");
    return httpd_resp_send(req, MANIFEST, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebServer::getStatusHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    // NOT counted as activity: the page polls this by itself every few seconds, so
    // treating it as "the user is doing something" made the 3-minute window immortal
    // and its countdown a fiction — it jumped back to 3:00 whenever a backgrounded
    // tab woke up and flushed its queued polls. Only deliberate actions extend it.
    httpd_resp_set_type(req, "application/json");

    // Settings as stored, or their defaults when they never were.
    NVSManager &nvs = NVSManager::getInstance();
    auto nvs_u8  = [&nvs](const char *k, uint8_t d)  { uint8_t v = d;  nvs.getU8(k, v, d);    return v; };
    auto nvs_u32 = [&nvs](const char *k, uint32_t d) { uint32_t v = d; nvs.getU32(k, v, d);   return v; };
    auto nvs_f   = [&nvs](const char *k, float d)    { float v = d;    nvs.getFloat(k, v, d); return v; };

    const uint8_t wl_mode = nvs_u8("wl_mode", 0), profile = nvs_u8("profile", 1),
                  live_clk = nvs_u8("live_clk", 1);
    std::string ssid_str;
    nvs.getString("ssid", ssid_str, "");

    // Use ArduinoJson to dynamically build status JSON string
    JsonDocument doc;
    
    esp_chip_info_t ci; esp_chip_info(&ci);
    char chipbuf[24];
    snprintf(chipbuf, sizeof(chipbuf), "ESP32-C6 rev%d", ci.revision);
    uint32_t flash_sz = 0; esp_flash_get_size(NULL, &flash_sz);
    const esp_partition_t* running = esp_ota_get_running_partition();

    JsonObject sys = doc["sys"].to<JsonObject>();
    sys["uptime"] = esp_log_timestamp();
    sys["boots"] = global_boot_count;
    // What is running, in both forms: the words for the owner and the integer the update manifest
    // compares. Shown behind the button-gated window like everything else here — an attacker who
    // can read this can already reach far more interesting endpoints beside it, and a device whose
    // owner cannot find out what firmware it has is not one they can be asked to report a bug from.
    {
        const esp_app_desc_t *ad = esp_app_get_description();
        sys["ver"] = ad ? ad->version : "?";
        sys["seq"] = FW_SEQ;
        // How many browsers will not be asked for the password next time. Shown so the promise is
        // visible and can be taken back; the tokens themselves never leave the device.
        sys["trusted"] = trust_count();
        // What the last check found. Published so the owner can see the channel is alive even when
        // there is nothing to install — silence is otherwise indistinguishable from a broken one.
        JsonObject o = sys["ota"].to<JsonObject>();
        // From flash, not from memory: the check that produced this ran on some earlier wake, and
        // every wake here is a cold boot. Reading the globals showed "never checked" almost always.
        OtaCheck::restore();
        o["checked"] = (uint32_t)OtaCheck::last_checked;
        o["avail"]   = OtaCheck::available_ver;
        o["seq"]     = OtaCheck::available_seq;
        o["error"]   = OtaCheck::last_error;
        o["auto"] = (int)nvs_u8("ota_auto", 1);
        // Whether going back is even possible. Offering a button that cannot work is worse than
        // offering none: the owner presses it in the moment they most need it to do something.
        {
            const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
            esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
            o["can_revert"] = (other && esp_ota_get_state_partition(other, &st) == ESP_OK &&
                               st == ESP_OTA_IMG_VALID);
        }
        // What the installer is doing right now, so the console can draw a bar instead of guessing.
        o["cur"]   = (uint32_t)FW_SEQ;                // what is running, so the page can compare
        o["state"] = (int)OtaInstall::state;          // 0 idle, 1 running, 2 done, 3 failed
        if (OtaInstall::total)   o["total"] = (uint32_t)OtaInstall::total;
        if (OtaInstall::fetched) o["got"]   = (uint32_t)OtaInstall::fetched;
        if (OtaInstall::why[0])  o["why"]   = OtaInstall::why;
        // What it is doing right now, in words. `state` says only "running" through the two
        // multi-second stretches where no counter moves, and a bar that stops without a word
        // reads as a hang — which is exactly how it was reported.
        if (OtaInstall::phase[0]) o["phase"] = OtaInstall::phase;
        // Reported only while it can still matter. A version OLDER than the running one can never
        // be offered again anyway, so saying it failed is a permanent notice about nothing — and a
        // card that keeps a red line about ancient history teaches its owner to stop reading it.
        { const uint32_t bad = nvs_u32("ota_bad", 0); if (bad > (uint32_t)FW_SEQ) o["bad"] = bad; }
        { std::string wy; NVSManager::getInstance().getString("ota_fail_why", wy, ""); if (!wy.empty()) o["fail"] = wy.c_str(); }
    }
    sys["btn_noise"] = global_btn_noise;
    // The same trailing-24h summary the hub gets, so the answer is reachable from the console too
    // — including on a device that is not in MQTT mode at all, where the hub path does not exist.
    {
        const EvtLog::Health h = EvtLog::getInstance().health();
        JsonObject hh = sys["health"].to<JsonObject>();
        hh["span_h"] = (int)(h.span_s / 360) / 10.0;   // how much this is actually based on
        if (h.valid) {
            hh["wakes_day"]  = (uint32_t)((uint64_t)h.wakes * 86400 / h.span_s);
            hh["screen_day"] = (uint32_t)((uint64_t)h.draws * 86400 / h.span_s);
            hh["full_day"]   = (uint32_t)((uint64_t)h.fulls * 86400 / h.span_s);
            hh["edges_day"]  = (uint32_t)((uint64_t)h.edges * 86400 / h.span_s);
            hh["duty_pct"]   = (int)((double)h.awake_ms / (double)h.span_s + 0.5) / 10.0;
        }
    }
    sys["reset"] = resetReasonStr();
    sys["chip"] = chipbuf;
    sys["cpu_mhz"] = esp_clk_cpu_freq() / 1000000;
    sys["chip_temp"] = readChipTemp();
    sys["heap_free"] = (uint32_t)esp_get_free_heap_size();
    sys["heap_total"] = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    sys["heap_min"] = (uint32_t)esp_get_minimum_free_heap_size();
    sys["flash_kb"] = flash_sz / 1024;
    sys["app_kb"] = running ? running->size / 1024 : 0;
    // On-flash history ring usage (records written / total slots). ~288 records/day.
    HistStore::getInstance().init();
    sys["hist_recs"] = HistStore::getInstance().count();
    sys["hist_cap"]  = HistStore::getInstance().capacity();
    sys["idf"] = esp_get_idf_version();
    sys["epoch"] = (uint32_t)time(NULL);   // device UTC clock (0 until synced)
    sys["portal_remain"] = global_portal_remain;
    // The page used to remember this by itself, in a variable that reset on every reload —
    // and phones reload freely. It then showed a countdown for a window the device was
    // holding open, or the reverse. The device knows; let it say so.
    sys["portal_keep"] = global_portal_keep;
    // The schedule actually in force this wake, which is not always the one on file: night halves
    // the cadence and silences the radio, Ultra Save drops the live clock, and before the first
    // time sync nothing can be scheduled at all.
    {
        JsonObject pl = sys["plan"].to<JsonObject>();
        pl["profile"]   = global_plan_profile;
        pl["night"]     = global_plan_night;
        pl["meas_min"]  = global_plan_meas_min;
        pl["radio_min"] = global_plan_radio_min;
        pl["live"]      = global_plan_live_clock;
        pl["last_meas"] = global_plan_last_meas;
        pl["last_co2"]  = global_plan_last_co2;
    }

    bool sta = WiFiManager::getInstance().isConnected();
    JsonObject net = doc["net"].to<JsonObject>();
    net["ssid"] = sta ? ssid_str.c_str() : PORTAL_AP_SSID;
    net["ip"] = WiFiManager::getInstance().getIPAddress().c_str();
    net["mode"] = sta ? "STA" : "AP";
    net["online"] = sta;                              // device has internet (STA up)
    net["conn_state"] = global_wifi_connect_state;    // portal APSTA: 0 idle,1 connecting,2 ok,-1 failed
    // По какой сети открыта страница. Нужно ровно одному: оговорке под ссылкой на гид, что у своей
    // точки доступа прибора интернета нет. Вне первой настройки эта фраза — постоянный шум под
    // строкой, которую и так видят при каждом открытии. Решается ПЕР ЗАПРОС и по локальному адресу
    // сокета (см. served_on_own_ap): во время окна прибор бывает в обеих сетях сразу.
    net["own_ap"] = served_on_own_ap(req);
    if (sta) {
        WiFiDetails wd = WiFiManager::getInstance().getDetails();
        net["mac"] = wd.mac.c_str();
        net["rssi"] = wd.rssi;
        net["ch"] = wd.channel;
        net["gw"] = wd.gw.c_str();
        net["dns"] = wd.dns.c_str();
    } else {
        net["mac"] = "--";
        net["rssi"] = 0;
        net["ch"] = 0;
        net["gw"] = "192.168.4.1";
        net["dns"] = "--";
    }

    JsonObject sensor = doc["sensor"].to<JsonObject>();
    sensor["co2"] = global_co2;
    // A CO2 calibration running or just finished. postCalibrateHandler starts it, the setup-window
    // loop runs it, and the console follows it here.
    sensor["co2cal"] = global_calib_result == 2 ? "running"
                     : global_calib_result == 1 ? "done"
                     : global_calib_result == -1 ? "failed" : "idle";
    sensor["co2cal_n"]    = (int)global_calib_samples;
    sensor["co2cal_need"] = SCD41::FRC_RUNUP_SHOTS;
    sensor["co2cal_corr"] = (int)global_calib_correction;
    sensor["co2cal_err"]  = (int)global_calib_error;
    // What the run-up breathed. The page quotes these back when the device asks whether that was
    // really outdoor air, so the owner answers about numbers rather than about a warning.
    sensor["co2cal_lo"]   = (int)global_calib_lo;
    sensor["co2cal_hi"]   = (int)global_calib_hi;
    sensor["co2cal_avg"]  = (int)global_calib_avg;
    sensor["co2cal_unsettled"] = (int)global_calib_unsettled;
    sensor["co2cal_settled"]   = (int)global_calib_settled;
    // Climate = SCD41 (accurate; no gas-heater self-heating bias). BME690 only as fallback.
    sensor["temp"] = global_s_temp != 0 ? global_s_temp : global_b_temp;
    sensor["hum"] = global_s_hum != 0 ? global_s_hum : global_b_hum;
    sensor["pres"] = global_b_pres * 0.00750062f; // mmHg
    // Both climates, side by side. `temp`/`hum` above are what the product shows; these two are
    // the diagnostic pair. They disagree because the BME690's gas heater runs at 320 C right next to
    // its own sensing element, so it reads warm and dry.
    //
    // HOW FAR APART depends entirely on how often that heater runs, and the old figure here (5.3 C)
    // was measured when gas was sampled every 3 seconds. It does not describe this firmware: BSEC's
    // ULP profile heats once every 300 s, and on the owner's device on 2026-09-16 the pair read
    // 21.71 / 23.25 C — 1.54 C apart, one reading. Treat that as an order of magnitude, not a
    // constant: nothing here has been compared against a reference thermometer yet, so which of the
    // two is closer to the truth is still unknown. The room climate comes from the SCD41 (section 9).
    sensor["t_offset"] = nvs_f("scd_toff", 4.0f);   // 4.0 = factory default, i.e. never calibrated here
    sensor["s_temp"] = global_s_temp;   // SCD41 — the displayed room climate
    sensor["s_hum"]  = global_s_hum;
    sensor["b_temp"] = global_b_temp;   // BME690 via BSEC — heater-biased, diagnostics only
    sensor["b_hum"]  = global_b_hum;
    sensor["iaq"] = global_iaq;
    sensor["static_iaq"] = global_static_iaq;
    sensor["iaq_acc"] = global_iaq_accuracy;
    // false in a build without Bosch's BSEC (bsec_manager_none.cpp): there is no index to wait for,
    // and the console says so instead of showing «warming up» for ever.
    sensor["iaq_built"] = BSECManager::builtIn();
    // Where this boot's gas calibration came from. Published because the flash-restore
    // branch is the one every firmware update puts a customer through, and it had never
    // been observed to run — a question that cannot be answered from a serial port that
    // does not exist on battery.
    sensor["gas_src"] = BSECManager::getInstance().lastLoadSource();
    // The bench gate of 4.44, finally readable. `gas_src` answers for the LATEST wake, and by the
    // time anyone opens the console that is always "RTC" — the update boot saved its state there
    // and every ordinary wake since has loaded it back. So the one boot that matters is invisible
    // in it, which is why the gate went unobserved through ten updates that all exercised it.
    //
    // These two fields are frozen at the boot that followed a firmware change or a power loss and
    // are not touched again. Gate passes when `gas_gate` says "NVS backup" AND `gas_gate_len` is a
    // real blob size (~197 B): the fallback ran and applied genuine state. A zero length means the
    // path ran and found nothing to restore, which is the failure this gate exists to catch.
    sensor["gas_gate"]     = BSECManager::getInstance().gateSource();
    sensor["gas_gate_len"] = BSECManager::getInstance().gateBlobLen();
    // The alert machine, so that "nothing arrived" can be told apart from "nothing was decided".
    {
        extern int alert_cand_class(); extern int alert_cand_count(); extern int alert_notified();
        extern int alert_confirmed();  extern int alert_tier();       extern int alert_tries_left();
        extern long alert_last_notify(); extern int alert_co2_latched(); extern int alert_damp_latched();
        JsonObject a = sensor["alert"].to<JsonObject>();
        a["cand"]      = alert_cand_class();   // class currently being considered
        a["count"]     = alert_cand_count();   // ticks it has held; 2 confirms it
        a["confirmed"] = alert_confirmed();    // what the device stands behind
        a["notified"]  = alert_notified();     // what the owner was last told
        a["tier"]      = alert_tier();         // 0 clear 1 advisory 2 warning 3 critical
        a["tries"]     = alert_tries_left();   // delivery attempts still owed; 0 = nothing pending
        a["last"]      = (uint32_t)alert_last_notify();
        a["co2_latch"] = alert_co2_latched();
        a["damp_latch"]= alert_damp_latched();
    }
    // What the flash backup is worth and how old it is. Reported so the bench gate (4.44) can
    // check its own precondition BEFORE the cell is pulled: a restore that fails because nothing
    // was ever stored looks exactly like a restore that is broken, and telling those two apart
    // afterwards costs a whole hardware session.
    {
        const uint8_t  bacc  = nvs_u8("bsec_acc", 255);
        const uint32_t bwall = nvs_u32("bsec_wall", 0);
        JsonObject gb = sensor["gas_backup"].to<JsonObject>();
        gb["acc"] = (bacc == 255) ? -1 : (int)bacc;          // -1 = nothing stored
        const time_t nw = time(NULL);
        gb["age_min"] = (bwall && nw > (time_t)bwall) ? (int)((nw - (time_t)bwall) / 60) : -1;
        // The latched verdict of the update boot, which is what the gate actually asks about.
        gb["gate"]     = BSECManager::getInstance().gateSource();
        gb["gate_len"] = (int)BSECManager::getInstance().gateBlobLen();
    }
    sensor["gas_pct"] = global_gas_percentage;
    sensor["gas_res"] = global_b_gas;
    sensor["stab"] = global_stab_status;
    sensor["run_in"] = global_run_in_status;
    sensor["pollution_class"] = global_pollution_class;
    sensor["pollution_text"] = global_pollution_text.c_str();
    sensor["wx_state"] = global_wx_state.c_str();  // off / ok / err (deferred weather status)
    // Battery telemetry (real values from the MAX17048-based BatteryMonitor)
    const uint32_t bat_mah = BATTERY_CAPACITY_MAH;
    sensor["bat_v"] = global_voltage;
    sensor["bat_p"] = global_soc;          // what the glass shows (battery_display.h)
    sensor["bat_p_meas"] = global_soc_raw; // what the cell measures — the two part only after a charge
    sensor["charging"] = (global_bat_state == 1);
    sensor["bat_state"] = global_bat_state;      // -1 discharging / 0 idle / 1 charging
    sensor["bat_crate"] = global_crate;
    sensor["bat_cycles"] = global_battery_cycles;
    sensor["bat_ttf_h"] = global_bat_ttf_h;
    sensor["bat_low"] = global_bat_low;
    sensor["bat_alerts"] = global_bat_alerts;    // MAX17048 STATUS flags bitmask
    // Which board this is, and what its cell reading is being multiplied by. The console needs the
    // first to decide whether to offer calibration at all — a v1 board has a fuel gauge with its own
    // voltage reference and must not be trimmed by hand — and the second so the number on the page
    // is the one the device is actually using, rather than a copy the page keeps for itself.
    {
        const BatterySource::Rev rev =
            BatterySource::revision(I2CManager::getInstance().getBusHandle());
        sensor["bat_src"] = (rev == BatterySource::RevGaugeV1)   ? "gauge"
                          : (rev == BatterySource::RevDividerV2) ? "divider"
                                                                 : "unknown";
        sensor["bat_cal"] = BatterySource::calibration();
    }
    {
        const PowerEstimate::Result pe = estimatePower();
        // The card's first three facts come from the same fit as "Measured draw", withheld until
        // the drop is past the gauge's accuracy (power_estimate.h). There used to be a second
        // slope behind them, and the two told the owner different things on the same card.
        const PowerEstimate::Card pc = PowerEstimate::card(pe, global_soc, global_on_usb);
        sensor["bat_daily_p"]   = pc.pct_day;
        sensor["bat_daily_mah"] = pc.mah_day;
        sensor["bat_tte_h"]     = pc.tte_h;
        sensor["pwr_ok"]     = pe.valid;
        sensor["pwr_ua"]     = pe.ua;
        sensor["pwr_hours"]  = pe.hours;
        sensor["pwr_drop"]   = pe.drop;
        sensor["pwr_mv"]     = pe.mv;
        sensor["pwr_months"] = pe.months;
        sensor["pwr_conf"]   = pe.conf;
    }
    sensor["on_usb"] = global_on_usb;            // USB-powered: sleep disabled
    sensor["error"] = false;
    sensor["out_temp"] = global_out_temp;
    // The strip's extra cells, with their sentinels intact rather than zeroed: this is how the owner
    // tells "the hub does not send uv_index" from "the cell is broken". Both look like dashes.
    sensor["out_uv"]    = global_out_uv;
    sensor["out_dew"]   = global_out_dew;
    // What «Damp» was decided on. Null, not a number, when there is no current outdoor reading:
    // the room number alone was used, and saying so is the point.
    if (std::isfinite(global_surface_rh)) sensor["surface_rh"] = roundf(global_surface_rh * 10) / 10;
    else                                  sensor["surface_rh"] = nullptr;
    // What the barometric plaque judges the level by. Null means the device cannot judge at all,
    // and `baro_src` says which half is missing.
    if (std::isfinite(global_p_sea))  sensor["p_sea"]  = roundf(global_p_sea * 10) / 10;
    else                              sensor["p_sea"]  = nullptr;
    if (std::isfinite(global_baro_k)) sensor["baro_k"] = roundf(global_baro_k * 10000) / 10000;
    else                              sensor["baro_k"] = nullptr;
    sensor["baro_src"] = global_baro_src == 1 ? "measured" : global_baro_src == 2 ? "city" : "none";
    sensor["out_cloud"] = global_out_cloud;
    sensor["out_desc"] = global_out_desc.empty() ? "Not Available" : global_out_desc.c_str();
    sensor["out_city"] = global_out_city.c_str();
    sensor["out_icon"] = global_out_icon.c_str();

    JsonObject cfg = doc["cfg"].to<JsonObject>();
    cfg["wl_mode"] = wl_mode;
    cfg["profile"] = profile;
    cfg["live_clk"] = live_clk;
    cfg["ssid"] = ssid_str.c_str();
    // The networks the device remembers, newest first — NAMES ONLY, never the passwords: this
    // endpoint is behind the console password, but a stored credential has no business leaving the
    // device at all, and the console has never needed one to show a list (wifi_slots.h).
    {
        WifiSlots::Net nets[WifiSlots::COUNT];
        const int n = WifiSlots::load(nets);
        auto arr = cfg["nets"].to<JsonArray>();
        for (int i = 0; i < n; i++) arr.add(nets[i].ssid);
    }
    // Whether a password is stored — never the password. The field in the console is empty because
    // the value has never left the device and never will, but an empty box on its own reads as
    // "nothing saved", which is a different and untrue statement. This is the same flag the OWM key
    // and the bot token already have; Wi-Fi, the one people actually worry about, did not.
    {
        std::string pass_str;
        NVSManager::getInstance().getString("pass", pass_str, "");
        cfg["pass_set"] = !pass_str.empty();
    }

    // The schedule settings, with the defaults readPlan() in bench_screens.cpp uses.
    static const uint32_t scd_def[3] = {5, 15, 30};
    static const uint32_t wifi_def[3] = {15, 30, 0};
    const uint8_t p = (profile <= 2) ? profile : 1;
    const uint32_t s_int = nvs_u32("s_int", scd_def[p]);
    cfg["s_int"] = s_int;
    cfg["s_int_n"] = nvs_u32("s_int_n", s_int * 2);
    cfg["w_int"] = nvs_u32("w_int", wifi_def[p]);
    cfg["nm_en"] = nvs_u8("nm_en", 1);
    cfg["nm_sh"] = nvs_u8("nm_sh", 23);
    cfg["nm_eh"] = nvs_u8("nm_eh", 7);
    // Zero, like every other reader of this key in the firmware. Reporting +3 meant the Clock
    // card confidently showed a timezone the device was not using, on exactly the devices
    // that had never been told one — the ones where the owner most needed the truth.
    cfg["tz_h"] = nvs_f("tz_h", 0.0f);

    // Battery settings, echoed so the console shows the current values
    // Reported, not settable: the console's life model divides by these, and a page that cannot
    // see them would have to keep its own copy — which is exactly how the two drift apart.
    cfg["lowbat_v"] = BATTERY_CUTOFF_V;
    cfg["bat_mah"] = bat_mah;

    // Outdoor weather config (API key never echoed back — just whether it's set)
    std::string city_cfg = "", owm_cfg = "";
    NVSManager::getInstance().getString("city", city_cfg, "");
    NVSManager::getInstance().getString("owm", owm_cfg, "");
    cfg["city"] = city_cfg.c_str();
    cfg["wx_src"] = nvs_u8("wx_src", 0);         // 0 MET Norway, 1 OpenWeatherMap
    {
        const float la = nvs_f("wx_lat", 0.0f), lo = nvs_f("wx_lon", 0.0f);
        cfg["wx_loc"] = (la != 0.0f || lo != 0.0f);
    }
    {
        std::string rule;
        nvs.getString("tz_rule", rule, "");
        cfg["tz_rule"] = rule.c_str();
    }
    cfg["owm_set"] = !owm_cfg.empty();
    // Last 4 chars only, for a masked UI preview (never the full key). Low-risk: an OWM key
    // can't be reconstructed from its tail, and it lets the user see WHICH key is saved.
    if (owm_cfg.size() >= 4) cfg["owm_last"] = owm_cfg.substr(owm_cfg.size() - 4).c_str();

    // Telegram config (bot token never echoed — only whether it's set)
    std::string tg_tok_cfg, tg_chat_cfg;
    nvs.getString("tg_tok", tg_tok_cfg, "");
    nvs.getString("tg_chat", tg_chat_cfg, "");
    cfg["tg_en"] = nvs_u8("tg_en", 0);
    cfg["tg_set"] = !tg_tok_cfg.empty();
    if (tg_tok_cfg.size() >= 4) cfg["tg_last"] = tg_tok_cfg.substr(tg_tok_cfg.size() - 4).c_str();
    cfg["tg_chat"] = tg_chat_cfg.c_str();
    // What this device calls itself in notifications. Not a secret, so unlike the token it
    // goes back out: the settings page has to show what is currently stored.
    std::string dev_name_cfg;
    NVSManager::getInstance().getString("dev_name", dev_name_cfg, "");
    cfg["dev_name"] = dev_name_cfg.c_str();

    // Air-alert settings
    cfg["al_co2"] = nvs_u32("al_co2", CO2_ALERT_DEFAULT);
    cfg["al_co2en"] = nvs_u8("al_co2en", 1);
    cfg["al_voc"] = nvs_u8("al_voc", 1);
    cfg["al_hum"] = nvs_u8("al_hum", 1);

    // Display units (0 = Celsius / hPa defaults; display-only, API values stay degC/mmHg)
    cfg["u_temp"] = nvs_u8("u_temp", UNIT_TEMP_DEFAULT);
    cfg["u_pres"] = nvs_u8("u_pres", UNIT_PRES_DEFAULT);
    cfg["g_span"] = nvs_u8("g_span", 24);
    cfg["no_wifi"] = nvs_u8("no_wifi", 0);
    cfg["mt_fab"] = nvs_u8("mt_fab", 0);        // smart homes the device is in, as last seen in Matter mode
    cfg["h_step"] = nvs_u8("h_step", 15);      // minutes between history records
    // Bottom strip: three option ids packed one per byte, low byte = left cell. The default
    // 0x040302 is Humidity / Wind / Feels — what the screen has always shown, so a device that has
    // never been told otherwise keeps exactly the layout its owner already knows.
    cfg["strip"] = nvs_u32("strip", STRIP_DEFAULT);

    // MQTT settings (password never echoed)
    std::string mq_host, mq_topic, mq_user;
    nvs.getString("mq_host", mq_host, "");
    nvs.getString("mq_topic", mq_topic, "smartmeteo");
    nvs.getString("mq_user", mq_user, "");
    cfg["mq_host"] = mq_host.c_str();
    cfg["mq_port"] = nvs_u32("mq_port", 1883);
    cfg["mq_topic"] = mq_topic.c_str();
    // What this device is called in Home Assistant's device registry; the console writes it into the
    // automation it hands out, so the automation finds this device by id and not by a renameable name.
    const std::string ha_id = MqttPublisher::deviceId();
    cfg["ha_id"] = ha_id.c_str();
    cfg["mq_user"] = mq_user.c_str();
    // Like pass_set / owm_set / tg_set. Without it the page cannot tell "no password" from "a
    // password it must not show", and the field's own promise ("Leave blank to keep") is unverifiable.
    {
        std::string mq_pass_str;
        NVSManager::getInstance().getString("mq_pass", mq_pass_str, "");
        cfg["mq_pass_set"] = !mq_pass_str.empty();
    }
    cfg["mq_tls"] = nvs_u8("mq_tls", 0);

    std::string json_str;
    serializeJson(doc, json_str);

    httpd_resp_send(req, json_str.c_str(), json_str.length());
    return ESP_OK;
}

// A POSIX TZ rule from the console ("STD-1DST,M3.5.0,M10.5.0/3"), which follow_tz_rule() in the cycle
// hands to newlib. Checked only for shape: printable, no spaces, starting like a zone name. An empty
// string removes the rule, and tz_h then stays as it is.
static void saveTzRule(const std::string &rule)
{
    bool ok = rule.size() < 64;
    for (char c : rule) if (c <= ' ' || c > '~') ok = false;
    if (ok && !rule.empty() && !(isalpha((unsigned char)rule[0]) || rule[0] == '<')) ok = false;
    if (!ok) { ESP_LOGW(TAG, "tz_rule is not a POSIX zone rule — not saved"); return; }
    NVSManager::getInstance().setString("tz_rule", rule);
    ESP_LOGI(TAG, "zone rule: %s", rule.empty() ? "(none)" : rule.c_str());
}

// The whole request body, NUL-terminated. One httpd_req_recv() may return only part of it, and the
// caller "may have to make multiple calls" (esp_http_server.h); on a socket error the handler must
// return an error. Returns the length, -1 on a socket error, -2 when the body does not fit.
static int recv_body(httpd_req_t *req, char *buf, size_t cap) {
    const int total = req->content_len;
    if (total < 0 || (size_t)total >= cap) return -2;
    int got = 0, stalls = 0;
    while (got < total) {
        const int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < 10) continue;
            return -1;
        }
        stalls = 0;
        got += r;
    }
    buf[got] = '\0';
    return got;
}
static esp_err_t body_failed(httpd_req_t *req, int r) {
    if (r == -2) httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
    return ESP_FAIL;
}

esp_err_t WebServer::postSaveHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();
    char buf[640]; // room for SSID + password + a batch of settings in one POST
    // recv_body() bounds the retries: a client that announces a Content-Length and stops sending would
    // otherwise hold the single httpd worker, and the setup window would run out underneath it.
    const int received = recv_body(req, buf, sizeof(buf));
    if (received < 0) return body_failed(req, received);

    // Parse JSON configurations
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, buf);
    if (error) {
        ESP_LOGE(TAG, "JSON parse failed: %s", error.c_str());
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Names only, never values. This used to print the whole body, and the whole body is where the
    // secrets are: the owner's home Wi-Fi password, the weather API key, the Telegram bot token,
    // the broker password — in clear text, over USB, to anyone who plugs the device in. Every other
    // path guards these (/api/status returns owm_set and tg_set, never the keys themselves); the
    // log was the one place that handed them out. Knowing WHICH settings arrived is all the
    // diagnostics ever needed, and that is what this prints.
    {
        char keys[192]; int n = 0;
        for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
            int w = snprintf(keys + n, sizeof(keys) - n, "%s%s", n ? "," : "", kv.key().c_str());
            if (w <= 0 || n + w >= (int)sizeof(keys)) { snprintf(keys + n, sizeof(keys) - n, "…"); break; }
            n += w;
        }
        ESP_LOGI(TAG, "save: %d B, fields: %s", received, keys);
    }

    // Save values to NVS
    // Settings with a fixed set of meanings are stored only when they are one of them. Read as long:
    // ArduinoJson turns an integer that does not fit uint8_t into 0, which would pass as valid.
    auto saveU8In = [](const char *key, JsonVariantConst v, long lo, long hi) {
        const long n = v.as<long>();
        if (n < lo || n > hi) {
            ESP_LOGW(TAG, "%s = %ld is outside %ld..%ld — not saved", key, n, lo, hi);
            return false;
        }
        NVSManager::getInstance().setU8(key, (uint8_t)n);
        return true;
    };
    // Connectivity: 0 Wi-Fi, 1 Wi-Fi + MQTT hub, 2 Matter over Thread (owner, 2026-09-16). Matter takes
    // effect on the next start; the console restarts the device right after choosing it.
    // Drop one remembered network. Separate from saving one because it is the only destructive
    // thing this endpoint does to the list, and it must not need the password to say which.
    if (!doc["forget_net"].isNull()) {
        const std::string gone = doc["forget_net"].as<std::string>();
        ESP_LOGW(TAG, "forget network \"%s\": %s", gone.c_str(),
                 WifiSlots::forget(gone) ? "done" : "not remembered");
    }
    if (!doc["wl_mode"].isNull() && saveU8In("wl_mode", doc["wl_mode"], 0, 2)) {
        const long wl = doc["wl_mode"].as<long>();
        if (wl == 2) NVSManager::getInstance().setU8("provisioned", 1);   // a past worth keeping starts here
        ESP_LOGW(TAG, "connectivity: %s", wl == 2 ? "Matter over Thread from the next start"
                                        : wl == 1 ? "Wi-Fi with an MQTT hub" : "Wi-Fi");
        global_screen_dirty = true;
    }
    // Minutes between history records: 5, 10 or 15. Nothing longer — the barometer looks for a record
    // within 15 min of "three hours ago" (bench_screens.cpp, measure()).
    if (!doc["h_step"].isNull()) {
        const long st = doc["h_step"].as<long>();
        if (st == 5 || st == 10 || st == 15) NVSManager::getInstance().setU8("h_step", (uint8_t)st);
        else ESP_LOGW(TAG, "h_step = %ld is not 5, 10 or 15 — not saved", st);
    }
    if (!doc["strip"].isNull()) {
        // Validated byte by byte rather than as a whole: a single bad id must not be able to carry
        // two good ones out of range with it, and the screen indexes its table with these directly.
        const uint32_t v = doc["strip"].as<uint32_t>();
        bool ok = (v >> 24) == 0;
        for (int i = 0; ok && i < 3; i++) ok = ((v >> (8 * i)) & 0xFF) < STRIP_OPT_COUNT;
        if (ok) {
            NVSManager::getInstance().setU32("strip", v);
            global_screen_dirty = true;           // the strip is on the glass; repaint now
        } else {
            ESP_LOGW(TAG, "strip = 0x%06lx holds an unknown cell id - not saved", (unsigned long)v);
        }
    }
    if (!doc["profile"].isNull() && saveU8In("profile", doc["profile"], 0, 3)) {
        global_screen_dirty = true;   // the mode changes what the screen shows; repaint now
    }
    if (!doc["live_clk"].isNull()) {
        global_screen_dirty = true;
        NVSManager::getInstance().setU8("live_clk", doc["live_clk"].as<uint8_t>());
    }
    // "I will run this without Wi-Fi." Chosen in the wizard after joining has failed, and cleared
    // the moment a network is saved again, so it can never linger and silence a device that has
    // since been given a network.
    if (!doc["no_wifi"].isNull()) {
        const bool off = doc["no_wifi"].as<uint8_t>() != 0;
        NVSManager::getInstance().setU8("no_wifi", off ? 1 : 0);
        if (off) {
            NVSManager::getInstance().setU8("provisioned", 1);   // a past worth keeping starts here
            ESP_LOGW(TAG, "set up to run without a network — radio stays off");
        }
    }
    if (!doc["ssid"].isNull()) {
        // "Verified" is cleared only when the network actually CHANGES. It used to be cleared by
        // the join attempt failing, which conflated two different facts: "this owner has finished
        // setting the device up" and "the last connection attempt succeeded". Re-running the
        // wizard on a working device and having one join miss its window then demoted a device
        // that had been running for months — and being unconfigured is what erases the log.
        //
        // Saving the same name again is not a change of network, so it says nothing about whether
        // that network works, and must not un-verify anything.
        const std::string new_ssid = doc["ssid"].as<std::string>();
        std::string old_ssid;
        NVSManager::getInstance().getString("ssid", old_ssid, "");
        if (new_ssid != old_ssid) {
            NVSManager::getInstance().setU8("wifi_ok", 0);   // a name nobody has joined yet
            ESP_LOGW(TAG, "network changed to \"%s\" — unverified until it joins", new_ssid.c_str());
        }
        // Through the remembered list, not straight at the key: the network goes to the front and
        // the other two stay behind it (wifi_slots.h). A password given is a password stored; none
        // given keeps the one we had, which is what re-saving a known network must do — the console
        // never sends a password back, so writing an empty one would erase the working credential.
        if (new_ssid.empty())            WifiSlots::clear();
        else if (!doc["pass"].isNull())  WifiSlots::remember(new_ssid, doc["pass"].as<std::string>());
        else                             WifiSlots::promote(new_ssid);
        if (!new_ssid.empty()) NVSManager::getInstance().setU8("no_wifi", 0);   // a network again
        // Ask the portal loop to bring the STA online (keeping the AP up) so the
        // user can validate weather / geocode cities without leaving the portal.
        // Only the portal loop services this flag; harmless outside the portal.
        global_wifi_connect_req = 1;
    } else if (!doc["pass"].isNull()) {
        NVSManager::getInstance().setString("pass", doc["pass"].as<std::string>());
    }
    // Profile cadences: s_int/w_int are MINUTES (SCD41 / radio intervals).
    // The base deep-sleep interval (NVS "interval", seconds) is fixed by the
    // BSEC ULP rhythm and is intentionally NOT settable from the UI.
    // Cadences in minutes. These decide how often the device wakes its sensors and raises its
    // radio, which is to say they decide the months of battery life the product is sold on -- and
    // nothing on the screen shows what they are set to. A saved radio interval of 1 means a Wi-Fi
    // join on every minute tick against the ~48 sessions a day the design budgets. The realistic
    // way in is a buggy client or a mistyped Custom interval rather than an attacker, since anyone
    // who can reach this endpoint can already rewrite the network credentials next to it -- but on
    // a product with no OTA an unvalidated physical parameter is not worth shipping either way.
    // Zero is preserved on the radio interval only: it is the encoding for "radio off", which both
    // the scheduler and the offline screen layout depend on.
    // `lo` is the smallest value the SCHEDULER will actually honour, and the clamp has to match it
    // or the stored setting is a claim the device does not keep. readPlan() raises any radio
    // interval below five minutes to five — a sensible floor, since a Wi-Fi session on every other
    // tick is minutes of transmitting per hour — but the API accepted 1 and 2, the console then
    // displayed them, and the device quietly ran at 5. The owner was shown a number that was not
    // the schedule. Clamp where it is written, so what is stored is what runs.
    auto saveMinutes = [](const char *key, uint32_t v, bool zero_means_off, uint32_t lo = 1) {
        if (zero_means_off && v == 0) { NVSManager::getInstance().setU32(key, 0); return; }
        if (v < lo || v > 1440) {
            ESP_LOGW(TAG, "%s = %u min is out of range [%u..1440] — clamped", key,
                     (unsigned)v, (unsigned)lo);
            v = (v < lo) ? lo : 1440;
        }
        NVSManager::getInstance().setU32(key, v);
    };
    if (!doc["s_int"].isNull()) {
        global_screen_dirty = true;
        saveMinutes("s_int", doc["s_int"].as<uint32_t>(), false);
    }
    if (!doc["s_int_n"].isNull()) {
        global_screen_dirty = true;
        saveMinutes("s_int_n", doc["s_int_n"].as<uint32_t>(), false);
    }
    if (!doc["w_int"].isNull()) {
        global_screen_dirty = true;
        saveMinutes("w_int", doc["w_int"].as<uint32_t>(), true, 5);   // readPlan's floor
    }
    if (!doc["nm_en"].isNull()) {
        global_screen_dirty = true;
        NVSManager::getInstance().setU8("nm_en", doc["nm_en"].as<uint8_t>());
    }
    if (!doc["nm_sh"].isNull()) saveU8In("nm_sh", doc["nm_sh"], 0, 23);
    if (!doc["nm_eh"].isNull()) saveU8In("nm_eh", doc["nm_eh"], 0, 23);
    if (!doc["tz_rule"].isNull()) saveTzRule(doc["tz_rule"].as<std::string>());
    if (!doc["tz_h"].isNull()) {
        // Used as (time_t)(tz * 3600) all over the cycle; a non-finite or huge value there is undefined.
        const float tz = doc["tz_h"].as<float>();
        if (std::isfinite(tz) && tz >= -12.0f && tz <= 14.0f) NVSManager::getInstance().setFloat("tz_h", tz);
        else ESP_LOGW(TAG, "tz_h is not a UTC offset (-12..+14 h) — not saved");
    }
    if (!doc["ota_auto"].isNull()) {
        NVSManager::getInstance().setU8("ota_auto", doc["ota_auto"].as<uint8_t>() ? 1 : 0);
    }
    // Cell capacity and the low-battery cutoff used to be saved here. They are built in now
    // (battery_spec.h, owner's decision 2026-09-27) and this endpoint IGNORES them, deliberately
    // and loudly. Accepting them with the field gone was the worst of both: a value written
    // out of band had no UI left to correct it in, and a cutoff of 4.5 V latches the lockout on a
    // full cell — release needs threshold + 0.15 V — so the device draws LOW BATTERY once and then
    // sleeps an hour at a time for ever, with the check running before sensors, radio and screen,
    // so not even the button can reach the console on battery.
    if (!doc["bat_mah"].isNull() || !doc["lowbat_v"].isNull())
        ESP_LOGW(TAG, "bat_mah/lowbat_v are built into the firmware and cannot be set — ignored");
    // Changing either invalidates the fetch throttle: a device that was just told where it lives
    // should show the weather at once, not after the next half-hourly window.
    if (!doc["city"].isNull()) {
        const std::string city = doc["city"].as<std::string>();
        std::string old_city;
        NVSManager::getInstance().getString("city", old_city, "");
        NVSManager::getInstance().setString("city", city);
        // Coordinates belong to the city they were found for. A city picked from the list brings its
        // own (below); a new one typed by hand is located by the device on its next weather fetch.
        if (city != old_city && doc["wx_lat"].isNull()) {
            NVSManager::getInstance().setFloat("wx_lat", 0.0f);
            NVSManager::getInstance().setFloat("wx_lon", 0.0f);
        }
        global_weather_dirty = true;
    }
    if (!doc["wx_lat"].isNull() && !doc["wx_lon"].isNull()) {
        const float lat = doc["wx_lat"].as<float>(), lon = doc["wx_lon"].as<float>();
        if (std::isfinite(lat) && std::isfinite(lon) && lat >= -90.0f && lat <= 90.0f &&
            lon >= -180.0f && lon <= 180.0f) {
            NVSManager::getInstance().setFloat("wx_lat", lat);
            NVSManager::getInstance().setFloat("wx_lon", lon);
            global_weather_dirty = true;
        } else {
            ESP_LOGW(TAG, "wx_lat/wx_lon are not coordinates — not saved");
        }
    }
    // Altitude of the place, metres. Arrives with the city from our own geo service, or typed by
    // hand. The barometric plaque needs it to tell a high from a low: this unit reads 1006 hPa on
    // an ordinary anticyclone, and only the altitude says that is 1023 at sea level. Sent as null
    // to forget it again.
    if (doc.containsKey("wx_alt")) {
        if (doc["wx_alt"].isNull()) {
            NVSManager::getInstance().setFloat("wx_alt", NAN);
            ESP_LOGI(TAG, "altitude forgotten");
        } else {
            const float alt = doc["wx_alt"].as<float>();
            if (std::isfinite(alt) && alt > -500.0f && alt < 9000.0f) {
                NVSManager::getInstance().setFloat("wx_alt", alt);
                ESP_LOGI(TAG, "altitude %.0f m", alt);
            } else {
                ESP_LOGW(TAG, "wx_alt %.1f is not a height on this planet — not saved", alt);
            }
        }
    }
    // Weather source: 0 MET Norway (keyless, the default), 1 OpenWeatherMap with the owner's key.
    if (!doc["wx_src"].isNull() && saveU8In("wx_src", doc["wx_src"], 0, 1)) global_weather_dirty = true;
    if (!doc["owm"].isNull()) {
        NVSManager::getInstance().setString("owm", doc["owm"].as<std::string>());
        global_weather_dirty = true;
    }
    // Telegram alerts (Wi-Fi standalone mode). Token is write-only (never echoed
    // back); blank token = keep the saved one. Chat ID isn't secret.
    if (!doc["tg_en"].isNull()) {
        NVSManager::getInstance().setU8("tg_en", doc["tg_en"].as<uint8_t>());
    }
    if (!doc["tg_tok"].isNull() && !doc["tg_tok"].as<std::string>().empty()) {
        NVSManager::getInstance().setString("tg_tok", doc["tg_tok"].as<std::string>());
        NVSManager::getInstance().setU32("tg_off", 0);   // new bot -> reset the getUpdates offset
    }
    if (!doc["tg_chat"].isNull()) {
        NVSManager::getInstance().setString("tg_chat", doc["tg_chat"].as<std::string>());
    }
    // Room name. Capped here as well as in the page: the field is one of the few places a user
    // types free text, and it ends up inside fixed message buffers.
    if (!doc["dev_name"].isNull()) {
        std::string dn = doc["dev_name"].as<std::string>();
        // The page counts 24 characters; this counted 24 bytes. A name in Cyrillic or with an
        // umlaut is two bytes a letter in UTF-8, so a name the page accepted was cut here — and cut
        // wherever the 24th byte landed, which is usually the middle of a character. The result is
        // a broken sequence that arrives in a Telegram message and a hub entity name as a
        // replacement glyph. Trim back to the start of the character instead: a continuation byte
        // is 10xxxxxx, so walk back off any of them.
        if (dn.size() > 24) {
            size_t cut = 24;
            while (cut > 0 && ((unsigned char)dn[cut] & 0xC0) == 0x80) cut--;
            dn.resize(cut);
        }
        NVSManager::getInstance().setString("dev_name", dn);
    }
    // Air-alert tuning: CO2 level (clamped to the safe preset band) + category
    // switches. Smoke/gas is a safety alert and has no switch.
    if (!doc["al_co2"].isNull()) {
        uint32_t lvl = doc["al_co2"].as<uint32_t>();
        if (lvl < CO2_ALERT_MIN) lvl = CO2_ALERT_MIN;
        if (lvl > CO2_ALERT_MAX) lvl = CO2_ALERT_MAX;
        NVSManager::getInstance().setU32("al_co2", lvl);
    }
    if (!doc["al_co2en"].isNull()) NVSManager::getInstance().setU8("al_co2en", doc["al_co2en"].as<uint8_t>());
    if (!doc["al_voc"].isNull())   NVSManager::getInstance().setU8("al_voc",   doc["al_voc"].as<uint8_t>());
    if (!doc["al_hum"].isNull())   NVSManager::getInstance().setU8("al_hum",   doc["al_hum"].as<uint8_t>());
    // Display units: 0/1 only (temp: C/F, pressure: hPa/mmHg).
    // The braces are not decoration. Without them the write ran on EVERY save, whatever it was
    // about — and a request that carries no unit field reads as zero, so saving a Wi-Fi password
    // quietly put the display back to Celsius. It looked like a setting that would not stick.
    if (!doc["u_temp"].isNull()) {
        global_screen_dirty = true;
        NVSManager::getInstance().setU8("u_temp", doc["u_temp"].as<uint8_t>() ? 1 : 0);
    }
    if (!doc["u_pres"].isNull()) {
        global_screen_dirty = true;
        NVSManager::getInstance().setU8("u_pres", doc["u_pres"].as<uint8_t>() ? 1 : 0);
    }
    // How many hours the graph on the glass covers. Clamped rather than trusted: it becomes a time
    // window, and a zero would ask for an empty one.
    if (!doc["g_span"].isNull()) {
        uint32_t gs = doc["g_span"].as<uint32_t>();
        if (gs < 1)   gs = 1;
        if (gs > 168) gs = 168;
        NVSManager::getInstance().setU8("g_span", (uint8_t)gs);
        global_screen_dirty = true;
    }
    // MQTT (Home Assistant) settings
    if (!doc["mq_tls"].isNull()) {
        NVSManager::getInstance().setU8("mq_tls", doc["mq_tls"].as<uint8_t>() ? 1 : 0);
    }
    if (!doc["mq_host"].isNull()) {
        // A stored broker password belongs to the broker it was typed for. Host, user and password
        // were three independent optional writes, so naming a new host while omitting the other two
        // left the credential in place and pointed the client at the new address -- and the client
        // puts that password in the CONNECT packet at the next radio window, every window after,
        // with nothing on the screen or in the console saying where it now goes. TLS does not help:
        // the bundle verifies the certificate of whatever host was named. This is the only place in
        // the firmware where write access to a setting converts into read access to a secret; the
        // Wi-Fi password, weather key and bot token are all write-only against fixed hosts.
        //
        // Same rule as the network name eleven lines up: a value that did not change is not a
        // change, and a value that did invalidates what was bound to the old one.
        const std::string new_host = doc["mq_host"].as<std::string>();
        std::string old_host;
        NVSManager::getInstance().getString("mq_host", old_host, "");
        if (new_host != old_host) {
            global_hub_dirty = true;            // a different hub has never been told what this is
            NVSManager::getInstance().setString("mq_user", "");
            NVSManager::getInstance().setString("mq_pass", "");
            ESP_LOGW(TAG, "broker changed to \"%s\" — stored credentials cleared, re-enter them",
                     new_host.c_str());
        }
        NVSManager::getInstance().setString("mq_host", new_host);
    }
    // Asking for a hub retracts "I will run this without Wi-Fi": MQTT is meaningless without a
    // network, and until 2026-09-30 the flag survived every mode switch. It is written by one
    // button in the wizard and was cleared by exactly one thing — saving a non-empty SSID — while
    // all three mode-switch buttons send wl_mode alone. A device set up offline and later given a
    // broker therefore kept its radio down for ever: go_online() returns on its first line, and
    // readPlan() even skips the hub's hourly floor. No weather, no publishing, no Telegram, no NTP
    // and no update check, while the console said "publishes on the next online update".
    //
    // Only when an SSID is on file, and that guard is not caution — device_configured() reads
    // `(ssid && (wifi_ok || provisioned)) || no_wifi || wl_mode == 2`, so clearing the flag on a
    // device with no network would make it UNCONFIGURED: back to the logo, waiting for a button.
    // That would be a worse fault than the one being fixed.
    {
        uint8_t off = 0;
        NVSManager::getInstance().getU8("no_wifi", off, 0);
        const bool wants_hub = (!doc["wl_mode"].isNull() && doc["wl_mode"].as<int>() == 1) ||
                               (!doc["mq_host"].isNull() &&
                                !doc["mq_host"].as<std::string>().empty());
        if (off && wants_hub) {
            std::string have_ssid;
            NVSManager::getInstance().getString("ssid", have_ssid, "");
            if (!have_ssid.empty()) {
                NVSManager::getInstance().setU8("no_wifi", 0);
                ESP_LOGW(TAG, "a hub was asked for, so \"no network\" is retracted — radio back on");
            } else {
                ESP_LOGE(TAG, "a hub was asked for but this device has no network saved and is set "
                              "up to run without one — nothing will be published");
            }
        }
    }
    if (!doc["mq_port"].isNull()) {
        const long port = doc["mq_port"].as<long>();
        if (port >= 1 && port <= 65535) NVSManager::getInstance().setU32("mq_port", (uint32_t)port);
        else ESP_LOGW(TAG, "mq_port %ld is not a TCP port — not saved", port);
    }
    if (!doc["mq_topic"].isNull()) {
        // The topic is half of every entity Home Assistant registered for this device. Change it
        // without re-announcing and the retained discovery configs still point at the old prefix:
        // the existing entities go unavailable, no new ones appear, and nothing recovers for up to
        // a day — discovery was sent "once every 24 h, and after a restart or a settings change",
        // except the last two were never implemented. The console asks owners to give each device
        // its own prefix, so this is a path people are told to take.
        std::string old_topic;
        NVSManager::getInstance().getString("mq_topic", old_topic, "smartmeteo");
        const std::string new_topic = doc["mq_topic"].as<std::string>();
        if (new_topic != old_topic) global_hub_dirty = true;
        NVSManager::getInstance().setString("mq_topic", new_topic);
    }
    if (!doc["mq_user"].isNull()) {
        NVSManager::getInstance().setString("mq_user", doc["mq_user"].as<std::string>());
    }
    if (!doc["mq_pass"].isNull()) {
        NVSManager::getInstance().setString("mq_pass", doc["mq_pass"].as<std::string>());
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    
    ESP_LOGI(TAG, "Settings saved successfully.");
    return ESP_OK;
}

esp_err_t WebServer::postPortalHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    // The activity stamp goes at the END of this handler, not here. The cycle watches the stamp and
    // recomputes the window the moment it changes — so stamping first, then blocking in recv() to
    // read the body, let it see "the user did something" while keep-open was still false, and hand
    // out three minutes instead of fifteen. The page had already said "Kept open"; the device quietly
    // disagreed. A phone sends the body in its own packet, so that gap is normally longer than the
    // cycle's fifty-millisecond step, which is why it happened almost every time rather than rarely.
    char buf[64];
    const int r = recv_body(req, buf, sizeof(buf));
    if (r < 0) return body_failed(req, r);
    if (r > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, buf) == DeserializationError::Ok) {
            if (!doc["keep"].isNull()) {
                global_portal_keep = doc["keep"].as<int>() != 0;
                ESP_LOGI(TAG, "Portal keep-open = %d", (int)global_portal_keep);
            }
            // "close" means the page is going away — the tab was shut, or the user pressed the
            // button that says so. Ending the window now takes the radio down with it rather than
            // leaving it up for the remainder of the three minutes, which is its entire cost.
            if (doc["close"].as<int>() != 0) {
                global_portal_close = true;
                ESP_LOGW(TAG, "page asked to close the setup window");
            }
        }
    }
    global_last_http_ms = esp_log_timestamp();   // last: keep/close are decided by now
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", -1);
    return ESP_OK;
}

esp_err_t WebServer::postCalibrateHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();
    char buf[64];
    int ref = 420;    // default fresh-air reference (current outdoor CO2 ~420 ppm)
    int answer = 0;   // the page answering "that did not read like outdoor air": 1 go on, -1 stop
    const int rr = recv_body(req, buf, sizeof(buf));
    if (rr < 0) return body_failed(req, rr);
    if (rr > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, buf) == DeserializationError::Ok) {
            if (!doc["ref"].isNull()) ref = doc["ref"].as<int>();
            // A field of its own rather than a second Calibrate press: "go on" must not be mistaken
            // for a fresh request and start the six-minute run-up over again.
            if (doc["confirm"].as<bool>())     answer = 1;
            else if (doc["cancel"].as<bool>()) answer = -1;
        }
    }
    // Hardening: fresh outdoor air is ~420 ppm worldwide. The UI no longer lets
    // the user edit this; clamp hard to a realistic fresh-air band so even a
    // hand-crafted API call can't anchor the baseline to a wrong value.
    if (ref < 400) ref = 400;
    if (ref > 450) ref = 450;

    // Sensirion's run-up before a forced recalibration is readings a minute apart for five minutes
    // (SCD41::FRC_RUNUP_SHOTS), far longer than a request can wait. So the request goes to the
    // setup-window loop, which owns the sensor, and is answered at once; the page follows progress
    // in /api/status (sensor.co2cal*). A request while one is already running just reports it.
    if (answer != 0) {
        // Only Stop is meaningful here now. The device no longer judges the air and so has nothing
        // to ask: the owner said where it stands by starting this, and yes has nobody to answer.
        // Stop stays available for the whole six-minute run-up, which is what a mis-tap costs.
        if (global_calib_result == 2 && answer == -1) global_calib_confirm = answer;
    } else if (global_calib_result != 2) {
        global_calib_correction = 0;
        global_calib_samples = 0;
        global_calib_error = 0;
        global_calib_unsettled = 0;
        global_calib_settled = 0;
        // One statement each: chaining them reads the volatile back, which C++20 deprecates.
        global_calib_lo = 0;
        global_calib_hi = 0;
        global_calib_avg = 0;
        global_calib_confirm = 0;
        global_calib_req = ref;
        global_calib_result = 2;
    }
    JsonDocument out;
    out["ok"] = true;
    out["pending"] = true;
    out["samples"] = (int)global_calib_samples;
    out["needed"] = SCD41::FRC_RUNUP_SHOTS;
    std::string js; serializeJson(out, js);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, js.c_str(), js.length());
    return ESP_OK;
}

struct HistCtx { httpd_req_t *req; bool first; bool failed; };
static void histEmit(const HistRec &r, void *ctx) {
    HistCtx *c = (HistCtx*)ctx;
    char buf[192];
    int acc = r.flags & 0x3, cls = (r.flags >> 2) & 0x7;
    int n = snprintf(buf, sizeof(buf),
        "%s{\"t\":%u,\"co2\":%u,\"temp\":%.1f,\"hum\":%u,\"pres\":%.1f,\"iaq\":%u,\"siaq\":%u,"
        "\"gas\":%u,\"soc\":%.2f,\"mv\":%u,\"acc\":%d,\"cls\":%d,\"bt\":%.1f,\"bh\":%u}",
        c->first ? "" : ",", (unsigned)r.t, r.co2, r.temp_d / 10.0f, r.hum,
        r.pres_d / 10.0f, r.iaq, r.siaq, r.gas,
        // Prefer the precise value; records written before it existed carry 0 there and fall back
        // to the whole-percent field, so an old log still charts, just coarsely.
        r.soc_d ? r.soc_d / 100.0f : (float)r.soc, r.vcell_mv, acc, cls,
        // The BME690's climate; 0 on every record older than 0.9.60 and on every wake that took no
        // gas sample, which the consumer reads as "not recorded" exactly as it does for pressure.
        r.b_temp_d / 10.0f, r.b_hum);
    // Stop at the first failed chunk, as ESP-IDF's file_server example does. A client that stopped
    // reading otherwise keeps the only server task busy, up to send_wait_timeout per record.
    if (c->failed) return;
    if (httpd_resp_send_chunk(c->req, buf, n) != ESP_OK) { c->failed = true; return; }
    c->first = false;
}
// The flight recorder, raw. Deliberately NOT JSON: the ring holds tens of thousands of 32-byte
// records, and rendering them as objects would inflate a 768 KB partition into several megabytes
// of text that has to be produced inside a three-minute console window and then parsed again on the
// other side. The host script decodes the bytes; the device just hands them over.
//
// `since` makes repeat visits cheap — pass the highest sequence number already held and only newer
// records come back. Ordering is by that counter and never by the clock, for the same reason the
// log itself is: a record written before the clock was set has no timestamp at all.
esp_err_t WebServer::getEventsHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    EvtLog &log = EvtLog::getInstance();
    if (!log.init()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no events partition");
        return ESP_FAIL;
    }
    uint32_t since = 0;
    {
        char q[64];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            char v[24];
            if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK)
                since = (uint32_t)strtoul(v, NULL, 10);
        }
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    // A 20-byte preamble so the file is self-describing: a host script that reads it a year from
    // now does not have to be told the record size or where the head was.
    struct __attribute__((packed)) Hdr {
        char     magic[4];      // "SMEV"
        uint16_t ver;
        uint16_t rec;           // sizeof(EvtRec)
        uint32_t cap;
        uint32_t head;
        uint32_t next_seq;
    } h;
    memcpy(h.magic, "SMEV", 4);
    h.ver = 1; h.rec = (uint16_t)log.recordSize();
    h.cap = log.capacity(); h.head = log.head(); h.next_seq = log.nextSeq();
    if (httpd_resp_send_chunk(req, (const char *)&h, sizeof(h)) != ESP_OK) return ESP_FAIL;

    // Walk oldest-first from the head so the stream is already in order and the host does not have
    // to sort a wrapped ring.
    const uint32_t cap = log.capacity();
    static EvtRec blk[16];
    uint32_t out = 0, sent = 0;
    for (uint32_t i = 0; i < cap; i++) {
        EvtRec r;
        if (!log.read((log.head() + i) % cap, r)) break;
        if (r.seq == 0xFFFFFFFFu || r.seq < since) continue;
        blk[out++] = r;
        if (out == 16) {
            if (httpd_resp_send_chunk(req, (const char *)blk, sizeof(blk)) != ESP_OK) return ESP_FAIL;
            sent += out; out = 0;
        }
    }
    if (out && httpd_resp_send_chunk(req, (const char *)blk, out * sizeof(EvtRec)) != ESP_OK)
        return ESP_FAIL;
    sent += out;
    ESP_LOGI(TAG, "flight recorder: sent %u record(s) since seq %u", (unsigned)sent, (unsigned)since);
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t WebServer::getHistoryHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    // NOT counted as activity, for the same reason as /api/status: the page fetches the log once
    // by itself when it loads, with nobody asking for it. Counting it made the countdown restart
    // whenever a phone reloaded the page — which phones do on their own, every time a backgrounded
    // tab is evicted and reopened. The window then never expired and the timer on screen was a
    // fiction. Changing the graph's range does not come back here; the log is already in the page.
    uint32_t from = 0, to = 0;
    char q[80];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(q, "from", v, sizeof(v)) == ESP_OK) from = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "to", v, sizeof(v)) == ESP_OK) to = (uint32_t)strtoul(v, NULL, 10);
    }
    HistStore::getInstance().init();
    httpd_resp_set_type(req, "application/json");
    if (httpd_resp_send_chunk(req, "[", 1) != ESP_OK) return ESP_FAIL;
    HistCtx ctx{ req, true, false };
    HistStore::getInstance().query(from, to, 1500, histEmit, &ctx);
    if (ctx.failed) return ESP_FAIL;
    if (httpd_resp_send_chunk(req, "]", 1) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);   // finish response
}

// Calibrate the room temperature against a reference thermometer. The user supplies the true
// temperature; the correction goes into the SENSOR's own offset (applied on the next measurement),
// because the SCD41 derives humidity from the same temperature — a software correction would fix
// the degrees and leave the percentage wrong.
//   new_offset = current_offset + (what we read - what it really is)
esp_err_t WebServer::postTempCalHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();
    httpd_resp_set_type(req, "application/json");
    char buf[128];
    const int len = recv_body(req, buf, sizeof(buf));
    if (len < 0) return body_failed(req, len);
    if (len == 0) { httpd_resp_send(req, "{\"ok\":false}", -1); return ESP_OK; }

    JsonDocument doc;
    if (deserializeJson(doc, buf)) { httpd_resp_send(req, "{\"ok\":false}", -1); return ESP_OK; }
    const float ref = doc["ref"] | 0.0f;
    // Taken once. The cycle task publishes this between single shots while this handler runs in
    // httpd's, so reading it again further down could mix two measurements into one answer: the
    // offset worked out from one reading, the figure reported from another.
    const float read = global_s_temp;
    if (ref < -10.0f || ref > 50.0f || read == 0.0f) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"range\"}", -1);
        return ESP_OK;
    }

    // One calibration per reading. The correction is the gap between what the device shows and
    // what the owner's thermometer says, and "what the device shows" only changes when the sensor
    // is read again — every few minutes, per the profile. Pressing the button twice in that gap
    // subtracted the same gap twice and left the device reading colder than the reference by as
    // much as it had been reading warmer. Nothing in the response said so, because the offset had
    // genuinely moved both times.
    //
    // "A new reading" means one taken with the offset now on file (global_temp_seq): a reading that
    // started before the last calibration reached the sensor still carries the old offset, and
    // measuring against it subtracts the same gap twice.
    // Zero means no reading of this boot is known to carry the offset on file yet (the console's first
    // reading comes within seconds), so there is nothing to measure against.
    static uint32_t cal_of_seq = UINT32_MAX;
    if (global_temp_seq == 0 || global_temp_seq == cal_of_seq) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"stale\"}", -1);
        ESP_LOGW(TAG, "temperature calibration refused: no new reading since the last one");
        return ESP_OK;
    }
    cal_of_seq = global_temp_seq;

    float cur = 4.0f;
    NVSManager::getInstance().getFloat("scd_toff", cur, 4.0f);
    float next = cur + (read - ref);
    if (next < 0.0f)  next = 0.0f;      // the sensor only ever subtracts
    if (next > 20.0f) next = 20.0f;
    NVSManager::getInstance().setFloat("scd_toff", next);

    char out[160];
    // The delta REPORTED is the one actually applied, which is not the one asked for when the
    // offset hits an end stop: the sensor can only subtract, so a request to warm the reading past
    // zero does nothing, and the page used to confirm a correction that had not happened.
    const float applied = next - cur;
    const float wanted  = ref - read;
    // `read` is the reading the correction was measured against, and the page must quote it. The
    // live "Device reads" line moves on to the next single shot — a minute later, already corrected
    // — so without this the owner was shown one number, told a delta worked out from another, and
    // left to decide which to believe: typed 20.5 against a displayed 20.4 and told «shifted by
    // -0.2», because at the moment of the press the device had been reading 20.7.
    snprintf(out, sizeof(out), "{\"ok\":true,\"offset\":%.2f,\"was\":%.2f,\"delta\":%.2f,\"read\":%.2f,\"clamped\":%s}",
             next, cur, -applied, read, (fabsf(applied + wanted) > 0.05f) ? "true" : "false");
    httpd_resp_send(req, out, -1);
    ESP_LOGW(TAG, "temperature calibrated: read %.2f, reference %.2f -> offset %.2f -> %.2f",
             read, ref, cur, next);
    return ESP_OK;
}

// Calibrate the cell voltage against a reference meter, on a v2 board. The owner (or whoever is
// accepting the board) puts a meter on the cell and types what it says; the device works out its own
// gain from what its divider reads at that same moment.
//
// The factor is measured against the RAW divider reading, never against the corrected one, so the
// operation is idempotent — pressing it twice with the same meter reading stores the same number,
// instead of squaring the correction. That is why there is no guard here of the kind /api/tempcal
// needs.
//
// A v1 board is refused rather than quietly accepted: its MAX17048 carries its own voltage
// reference, and trimming it by hand would make a calibrated part worse.
// End the console session now. Like /api/reboot it does not act — it raises a flag and the loop
// task closes its own window, because that task owns the panel and the radio and puts them down in
// order. Without this the only ways out were the device's button and the idle timeout, and waiting
// out three minutes keeps the radio up for all of them.
esp_err_t WebServer::postCloseHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();
    httpd_resp_set_type(req, "application/json");
    global_close_req = true;
    ESP_LOGW(TAG, "console asked to close the session");
    httpd_resp_send(req, "{\"ok\":true}", -1);
    return ESP_OK;
}

esp_err_t WebServer::postClockHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();
    char buf[192];
    const int r = recv_body(req, buf, sizeof(buf));
    if (r < 0) return body_failed(req, r);
    bool ok = false;
    if (r > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, buf) == DeserializationError::Ok && !doc["epoch"].isNull()) {
            // The zone's rule travels with the offset, so daylight saving follows by itself.
            if (!doc["tz_rule"].isNull()) saveTzRule(doc["tz_rule"].as<std::string>());
            // Set the device UTC clock from the phone's time; store its timezone.
            //
            // Both values are checked here now. /api/save has always clamped tz_h to -12..14 and
            // rejected anything not finite, but this endpoint writes the same key and checked
            // nothing — and the clock it sets drives the update schedule, the NTP backoff, alert
            // timestamps, history records and the barometric forecast. The floor is 2020-01-01,
            // matching drop_impossible_stamps() in the cycle; the ceiling is far enough out to
            // outlive the product and near enough to catch a phone with a broken clock.
            const uint32_t epoch_raw = doc["epoch"].as<uint32_t>();
            if (epoch_raw < 1577836800u || epoch_raw > 2524608000u) {   // 2020-01-01 .. 2050-01-01
                ESP_LOGW(TAG, "clock refused: epoch %lu out of range", (unsigned long)epoch_raw);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"ok\":false,\"error\":\"epoch out of range\"}", -1);
                return ESP_OK;
            }
            time_t epoch = (time_t)epoch_raw;
            struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            if (!doc["tz"].isNull()) {
                const float tz = doc["tz"].as<float>();
                if (std::isfinite(tz) && tz >= -12.0f && tz <= 14.0f)
                    NVSManager::getInstance().setFloat("tz_h", tz);
                else
                    ESP_LOGW(TAG, "clock: timezone %.2f ignored, outside -12..14", (double)tz);
            }
            ESP_LOGI(TAG, "Clock set from phone: epoch %ld, tz %.1f",
                     (long)epoch, doc["tz"] | 0.0f);
            ok = true;
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", -1);
    return ESP_OK;
}

// GET /api/geocode?q=<partial> — keyless city autocomplete, proxied through the
// device's internet (APSTA) so it works without the phone having cellular data.
esp_err_t WebServer::getGeocodeHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    global_last_http_ms = esp_log_timestamp();
    httpd_resp_set_type(req, "application/json");

    // Needs the device to be online (STA up). 503 -> the UI silently degrades
    // to manual "City,CC" entry.
    if (!WiFiManager::getInstance().isConnected()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "{\"results\":[],\"offline\":true}", -1);
        return ESP_OK;
    }

    // 192, not 64. The value arrives percent-encoded, and encodeURIComponent() spends SIX bytes on
    // one Cyrillic letter (two UTF-8 bytes, each written as %XX) — so 64 bytes held ten of them and
    // httpd_query_key_value() answered ESP_ERR_HTTPD_RESULT_TRUNC, which this code read as "no query"
    // and returned an empty list with nothing said. "Екатеринбург" is twelve letters, 72 bytes: the
    // city search simply appeared to know no Russian city with a long name, and the city database
    // does carry those names. 192 bytes is 32 Cyrillic letters or 64 accented Latin ones.
    char q[192] = {0};
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    std::string query;
    if (qlen > 1 && qlen < 512) {
        char *qbuf = (char*)malloc(qlen);
        if (qbuf && httpd_req_get_url_query_str(req, qbuf, qlen) == ESP_OK) {
            // TRUNC is still possible — somebody can always type more — and it is said out loud
            // rather than silently becoming "found nothing".
            const esp_err_t qerr = httpd_query_key_value(qbuf, "q", q, sizeof(q));
            if (qerr == ESP_ERR_HTTPD_RESULT_TRUNC) {
                ESP_LOGW(TAG, "city search: query longer than %u bytes — ignored", (unsigned)sizeof(q));
                q[0] = '\0';
            }
            if (qerr == ESP_OK) {
                // Decode %XX and '+'. The comment here has always said this; the code never did it,
                // and the server's own header is explicit that it will not do it for us — "the
                // components of URL query string (keys and values) are not URLdecoded". The page
                // sends encodeURIComponent(), so every city name with a space or an accent arrived
                // as literal %20 and %C3%BC and matched nothing: Frankfurt am Main, München,
                // Garmisch-Partenkirchen. Autocomplete simply appeared to find nothing, in most of
                // Europe, with no error to explain it.
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                for (size_t i = 0; q[i]; i++) {
                    if (q[i] == '+') { query += ' '; continue; }
                    if (q[i] == '%' && q[i+1] && q[i+2]) {
                        const int hi = hex(q[i+1]), lo = hex(q[i+2]);
                        if (hi >= 0 && lo >= 0) { query += (char)((hi << 4) | lo); i += 2; continue; }
                    }
                    query += q[i];      // a stray '%' is passed through rather than swallowed
                }
            }
        }
        free(qbuf);
    }

    std::vector<GeoCity> cities;
    const char *source = "";
    const bool answered = Weather::geocode(query, cities, 6, nullptr, &source);

    JsonDocument doc;
    // Neither our city search nor the fallback answered: the page falls back to typing "City,CC" and,
    // failing that, to coordinates.
    if (!answered) doc["unavailable"] = true;
    doc["source"] = source;
    JsonArray arr = doc["results"].to<JsonArray>();
    for (auto &c : cities) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = c.name;
        o["country"] = c.country;
        o["state"] = c.state;
        o["lat"] = c.lat;
        o["lon"] = c.lon;
        o["tz"] = c.tz;
    }
    std::string out;
    serializeJson(doc, out);
    httpd_resp_send(req, out.c_str(), out.size());
    return ESP_OK;
}

// POST /api/weather/test {city, key?} — validate the OWM key + city and return
// the live temp, or a typed error, so the portal can guide the user.
esp_err_t WebServer::postWeatherTestHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();
    httpd_resp_set_type(req, "application/json");

    char buf[256];
    const int r = recv_body(req, buf, sizeof(buf));
    if (r < 0) return body_failed(req, r);
    std::string city, key;
    uint8_t src = WX_SRC_MET;
    NVSManager::getInstance().getU8("wx_src", src, WX_SRC_MET);
    float lat = 0, lon = 0;
    if (r > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, buf) == DeserializationError::Ok) {
            city = doc["city"] | "";
            key  = doc["key"]  | "";
            if (!doc["src"].isNull()) src = (doc["src"].as<long>() == WX_SRC_OWM) ? WX_SRC_OWM : WX_SRC_MET;
            lat = doc["lat"] | 0.0f;
            lon = doc["lon"] | 0.0f;
        }
    }
    // Fall back to the saved key when the field was left blank (key not echoed).
    if (key.empty()) NVSManager::getInstance().getString("owm", key, "");

    if (!WiFiManager::getInstance().isConnected()) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"offline\"}", -1);
        return ESP_OK;
    }

    float temp = -999.0f; std::string desc, name;
    WxResult res = WxResult::NetErr;
    if (src == WX_SRC_OWM) {
        res = Weather::validate(city, key, temp, desc, name);
    } else if (city.empty()) {
        res = WxResult::CityBad;
    } else {
        // MET needs a place, not a name: the one the page picked, or the first match for the name.
        name = city.substr(0, city.find(','));
        bool located = (lat != 0.0f || lon != 0.0f) && std::isfinite(lat) && std::isfinite(lon);
        if (!located) {
            GeoCity g;
            located = Weather::locate(city, g);
            if (located) { lat = g.lat; lon = g.lon; name = g.name; }
        }
        if (!located) {
            res = WxResult::CityBad;
        } else {
            WxNow now; float lo = 999.0f, hi = -999.0f; WxDay days[1]; int n = 0; time_t exp = 0; int st = -1;
            float tz_h = 0; NVSManager::getInstance().getFloat("tz_h", tz_h, 0.0f);
            if (Weather::met(lat, lon, (int)lroundf(tz_h * 3600.0f), now, lo, hi, days, 1, n, exp, st)) {
                res = WxResult::Ok; temp = now.temp; desc = now.desc;
            }
        }
    }

    JsonDocument out;
    if (res == WxResult::Ok) {
        out["ok"] = true;
        out["temp"] = temp;
        out["desc"] = desc;
        out["name"] = name;
    } else {
        out["ok"] = false;
        out["error"] = (res == WxResult::KeyBad)  ? "key"
                     : (res == WxResult::CityBad) ? "city" : "net";
    }
    std::string s;
    serializeJson(out, s);
    httpd_resp_send(req, s.c_str(), s.size());
    return ESP_OK;
}

// POST /api/telegram/test {token?, chat?} — send a test message so the user can
// confirm the bot works before relying on alerts. Blank token uses the saved one.
esp_err_t WebServer::postMqttTestHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();
    httpd_resp_set_type(req, "application/json");

    // Settings are saved before the test is offered, so this reads them from NVS rather than from
    // the request: testing a broker the device has not stored would prove nothing about what it
    // will actually do at the next window.
    std::string host;
    NVSManager::getInstance().getString("mq_host", host, "");
    if (host.empty()) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"empty\"}", -1);
        return ESP_OK;
    }
    if (!WiFiManager::getInstance().isConnected()) {
        // The setup access point has no route to a broker on the home network.
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"offline\"}", -1);
        return ESP_OK;
    }

    const std::string payload = MqttPublisher::buildState();
    const auto r = MqttPublisher::publishState(payload);
    JsonDocument out;
    out["ok"] = r.ok;
    if (!r.ok) out["error"] = r.error ? r.error : "publish";
    else {
        std::string topic;
        NVSManager::getInstance().getString("mq_topic", topic, "smartmeteo");
        if (topic.empty()) topic = "smartmeteo";
        out["topic"] = (topic + "/state").c_str();
        out["bytes"] = (int)payload.size();
    }
    std::string body; serializeJson(out, body);
    httpd_resp_send(req, body.c_str(), body.size());
    return ESP_OK;
}

esp_err_t WebServer::postTelegramTestHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();
    httpd_resp_set_type(req, "application/json");

    char buf[256];
    const int r = recv_body(req, buf, sizeof(buf));
    if (r < 0) return body_failed(req, r);
    std::string token, chat;
    if (r > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, buf) == DeserializationError::Ok) {
            token = doc["token"] | "";
            chat  = doc["chat"]  | "";
        }
    }
    // The stored token may be used with a chat id from the request, and that is deliberate: the
    // console cannot do otherwise. A saved token is never sent back to the page, so its field is
    // empty on every reload, while the chat id is visible and gets posted back — "test what is
    // saved" and "test a new chat id with the saved bot" are the same request from here.
    //
    // Before 2026-09-18 that combination was reachable by anyone on the network, which turned the
    // owner's bot into a message relay to a chat of the caller's choosing. What closes it is the
    // sign-in in front of every endpoint, not a rule here; all this adds is that a request with no
    // chat at all cannot fall back to storage and send something.
    if (token.empty()) NVSManager::getInstance().getString("tg_tok", token, "");
    if (chat.empty()) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"empty\"}", -1);
        return ESP_OK;
    }

    if (token.empty() || chat.empty()) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"empty\"}", -1);
        return ESP_OK;
    }
    if (!WiFiManager::getInstance().isConnected()) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"offline\"}", -1);
        return ESP_OK;
    }

    // Named like every other message, and says what will arrive, which is more than air quality.
    std::string dev;
    NVSManager::getInstance().getString("dev_name", dev, "");
    if (dev.empty()) dev = "Smart Meteo";
    const std::string text = "\xE2\x9C\x85 " + dev + " \xC2\xB7 Telegram connected\n"
                             "Alerts arrive here: stuffy or polluted air, damp and a low battery.\n"
                             "Commands: /status, /co2, /air, /battery.";
    bool ok = Telegram::send(token, chat, text);
    httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"send\"}", -1);
    return ESP_OK;
}

// Switching off, asked for from the console. Same shape as the restart below it and for the same
// reason: this task shares the panel with the cycle and cannot know whether a frame is half sent,
// so it only raises the flag. The window loop closes and its caller does the work.
esp_err_t WebServer::postPowerOffHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;
    if (!require_json(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Switching off...\"}", -1);
    global_last_http_ms = esp_log_timestamp();
    global_poweroff_req = true;
    ESP_LOGW(TAG, "switch-off requested from the console");
    return ESP_OK;
}

esp_err_t WebServer::postRebootHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Rebooting...\"}", -1);
    // Not restarted from here. This task shares the panel with the cycle and has no way to know it is
    // mid-frame — a mode change repaints the glass, and the console asks for the restart a moment
    // later — and a restart from this task skips the panel's own shutdown hook. The window loop does it.
    global_last_http_ms = esp_log_timestamp();
    global_reboot_req = true;
    ESP_LOGW(TAG, "restart requested from the console");
    return ESP_OK;
}

// Install firmware handed over the local network, inside the setup window. This is the path
// that remains when the remote one cannot be used: the server gone, DNS wrong, a certificate
// chain rotated out from under a device that has not updated in years, or simply no internet
// where the device lives. There is no factory partition on this layout, so without it the only
// remedy left is a cable and a disassembled enclosure.
//
// It is not a way IN. The bytes are refused unless they carry a valid signature, which
// esp_ota_end() checks because SECURE_SIGNED_ON_UPDATE is set — so this moves BYTES, not code,
// and only inside a window that already required a physical button press.
// Ask now instead of waiting for this device's own daily hour. Deliberately does NOT touch the
// schedule: a manual check must not push the automatic one a day into the future, or a curious
// owner pressing this every evening would quietly disable the thing it exists to exercise.
// Start an install and answer at once. The transfer is 1.7 MB over TLS and takes tens of seconds:
// doing it inside the handler would hold one of the server's few worker threads for all of it, and
// the page asking for it would sit on a dead socket with nothing to show. The worker publishes its
// progress into /api/status instead, which the page is already polling.
esp_err_t WebServer::postOtaInstallHandler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();

    const bool started = OtaInstall::start();
    JsonDocument d;
    d["ok"] = started;
    // Says WHY not, in the device's own words. The previous version guessed among three canned
    // strings and landed on "not allowed now" for everything it had not thought of — which is what
    // it told the owner when the real reason was that a switch about AUTOMATIC updates was gating
    // a manual one. A refusal that does not name its cause sends someone hunting in the wrong file.
    if (!started) {
        const char *r = (OtaInstall::state == OtaInstall::StRunning)
                      ? "an install is already running"
                      : OtaInstall::blockedReason(false);
        d["error"] = r ? r : "could not start";
    }
    std::string out; serializeJson(d, out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out.c_str());
    return ESP_OK;
}

esp_err_t WebServer::postOtaCheckHandler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();

    uint32_t seen_before = 0;
    NVSManager::getInstance().getU32("ota_seen", seen_before, 0);
    OtaCheck::run();
    NVSManager::getInstance().setU32("ota_seen", seen_before);   // put the schedule back

    JsonDocument d;
    d["ok"]    = (OtaCheck::last_error[0] == '\0');
    d["ver"]   = OtaCheck::available_ver;
    d["seq"]   = OtaCheck::available_seq;
    d["error"] = OtaCheck::last_error;
    std::string out; serializeJson(d, out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out.c_str());
    return ESP_OK;
}

// Back to the image that was running before this one.
//
// This calls mark_app_invalid_rollback_and_reboot and NOT set_boot_partition(other), and the
// difference matters: the second would put the known-good image back through probation, so it would
// have to prove itself all over again — and revert to the broken one if anything went wrong while
// it did. The first is the operation that says "this image is the bad one".
//
// It cannot brick the device. IDF checks esp_ota_check_rollback_is_possible() first and returns an
// error WITHOUT rebooting when there is no suitable app to go back to (verified in esp_ota_ops.c),
// which is exactly the state every device is in today, before a first update has ever installed.
esp_err_t WebServer::postOtaRevertHandler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();

    // An explicit confirmation, because this is not "switch slots and carry on": the running image is
    // marked INVALID for good on this device, and the updater then refuses that version for ever
    // (last_invalid_version() in ota_install.cpp). Until 2026-09-18 a bare POST with no body at all
    // did it. The console now explains the cost and sends {"confirm":true}.
    char body[96] = {0};
    const int r = recv_body(req, body, sizeof(body));
    if (r < 0) return body_failed(req, r);
    JsonDocument cdoc;
    if (deserializeJson(cdoc, body) != DeserializationError::Ok || !(cdoc["confirm"] | false)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"confirmation required\"}");
        return ESP_OK;
    }

    EvtLog::getInstance().event(EVT_SEC, SEC_OTA_REVERT);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();   // returns only on refusal
    ESP_LOGW(TAG, "revert refused: %s", esp_err_to_name(e));
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"nothing to revert to\"}");
    return ESP_OK;
}

esp_err_t WebServer::postOtaHandler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_octet_stream(req)) return ESP_OK;
    global_last_http_ms = esp_log_timestamp();
    // One writer per slot: esp_ota_begin() has no lock of its own, so an install already running from
    // the manifest would be corrupted by this one (esp_ota_end would then reject both).
    if (OtaInstall::state == OtaInstall::StRunning) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "an update is already installing");
        return ESP_OK;
    }

    const int len = req->content_len;
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no update slot"); return ESP_OK; }

    // Bounds before erasing anything. An image larger than the slot cannot be written and a tiny
    // one cannot be firmware; rejecting both costs nothing and saves eight seconds of flash erase.
    if (len < 256 * 1024 || (size_t)len > dst->size) {   // the same floor as ota_install.cpp
        ESP_LOGW(TAG, "OTA upload refused: %d bytes against a %u byte slot", len, (unsigned)dst->size);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "implausible image size");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "OTA upload starting: %d bytes into %s", len, dst->label);
    // Set for the whole of this handler, cleared however it leaves — six return paths below, and a
    // flag left standing would refuse every later install and hold the setup window open for ever.
    struct SlotClaim {
        SlotClaim()  { OtaInstall::local_upload = true;  }
        ~SlotClaim() { OtaInstall::local_upload = false; }
    } claim;

    esp_ota_handle_t h = 0;
    // The REAL size, never OTA_SIZE_UNKNOWN: that erases the whole 3072 KB slot instead of the
    // 1.7 MB actually needed, which is some eight seconds of flash work for nothing.
    esp_err_t err = esp_ota_begin(dst, (size_t)len, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not start the update");
        return ESP_OK;
    }

    static uint8_t buf[2048];
    int got = 0, stalls = 0;
    while (got < len) {
        const int n = httpd_req_recv(req, (char *)buf, sizeof(buf));
        // A receive timeout is not the end of the upload — ESP-IDF's file_server example retries it.
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < 20) continue;
        if (n <= 0) {
            ESP_LOGE(TAG, "OTA upload broke off after %d of %d bytes", got, len);
            esp_ota_abort(h);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload interrupted");
            return ESP_OK;
        }
        if ((err = esp_ota_write(h, buf, n)) != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %d: %s", got, esp_err_to_name(err));
            esp_ota_abort(h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
            return ESP_OK;
        }
        got += n;
        stalls = 0;
        // Keep the window open. It closes on inactivity, and one POST carrying 1.7 MB looks exactly
        // like inactivity from outside — the countdown would expire in the middle of writing
        // firmware, and the cycle would shut the radio down and sleep while another task flashed.
        global_last_http_ms = esp_log_timestamp();
        // No esp_task_wdt_reset() here: this is the httpd task, which is not subscribed to the task
        // watchdog, and a reset from an unsubscribed task only logs an error (wdts.rst).
    }

    // Everything that can refuse this image refuses it here — a truncated write, a corrupted body,
    // and the one that matters: a signature missing or not ours. otadata is still untouched, so a
    // refusal leaves the running image exactly where it was.
    if ((err = esp_ota_end(h)) != ESP_OK) {
        ESP_LOGE(TAG, "image rejected: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, err == ESP_ERR_OTA_VALIDATE_FAILED
                                ? "{\"ok\":false,\"error\":\"signature\"}"
                                : "{\"ok\":false,\"error\":\"invalid\"}");
        return ESP_OK;
    }
    if ((err = esp_ota_set_boot_partition(dst)) != ESP_OK) {
        ESP_LOGE(TAG, "could not select the new image: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not select the image");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "new image accepted — restarting into it");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restart\":true}");
    // RESTART, never sleep. A wake out of deep sleep fast-boots the old partition straight from RTC
    // retain memory without consulting otadata, so sleeping here would leave the switch to happen
    // at some later, unrelated reset with nobody able to connect the two.
    // Hand the gas calibration to the new image, as the manifest install does before its restart.
    smart_meteo_ota_handoff();
    vTaskDelay(pdMS_TO_TICKS(400));      // let the response actually leave
    esp_restart();
    return ESP_OK;
}

esp_err_t WebServer::postFactoryHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    if (!require_json(req)) return ESP_OK;
    // Asked for here, carried out by the cycle. This handler used to wipe NVS itself, from the web
    // server's task, and restart a second later. Meanwhile the setup loop kept running on the other
    // task against a store that no longer existed, and the other stores were left to whatever the
    // next boot happened to decide: the history, the flight recorder and the crash dump. Now the
    // cycle records the request where a power cut cannot lose it, restarts, and erases all of them
    // before anything reads the old state (factory_wipe() in bench_screens.cpp). The CO2 sensor's
    // own calibration is kept.
    EvtLog::getInstance().event(EVT_SEC, SEC_FACTORY_REQ);
    // Sent before the wipe, because after it there is no token to send with.
    smart_meteo_security_note("a factory reset was requested. Wi-Fi, settings, history and the "
                              "smart-home pairing are being erased, and the setup Wi-Fi gets a new "
                              "password, shown on the screen.");
    global_factory_req = true;
    ESP_LOGW(TAG, "Factory reset requested via web portal — handing it to the cycle");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Erasing everything. The device will show its "
                            "logo and sleep until its button is pressed.\"}");
    return ESP_OK;
}

esp_err_t WebServer::getScanHandler(httpd_req_t *req) {
    if (!require_auth(req)) return ESP_OK;   // the console signs in first (see require_auth)
    // Counted as activity, unlike the page's own polling. Scanning for networks is somebody
    // standing at the device with a phone, and it may be the only thing they do for a minute while
    // they find the right network and type a password. The setup window's own comment already
    // named scanning as a deliberate action; this is the line that makes that true.
    global_last_http_ms = esp_log_timestamp();
    httpd_resp_set_type(req, "application/json");

    std::vector<WiFiNetwork> nets = WiFiManager::getInstance().scanNetworks();
    
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    
    for (const auto &net : nets) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"] = net.ssid.c_str();
        obj["rssi"] = net.rssi;
        obj["enc"] = net.secure;
        obj["auth"] = net.auth;   // "open" | "psk" | "enterprise" — see WiFiNetwork
    }
    
    std::string json_str;
    serializeJson(doc, json_str);
    
    httpd_resp_send(req, json_str.c_str(), json_str.length());
    return ESP_OK;
}
