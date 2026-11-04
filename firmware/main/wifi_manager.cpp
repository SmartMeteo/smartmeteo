#include "wifi_manager.h"
#include "wifi_slots.h"
#include "nvs_manager.h"
#include "matter_mode.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "bootloader_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char* TAG = "WIFI_MANAGER";

// Fast-connect cache: skipping the full channel scan cuts the connection from
// ~2-4 s to ~0.3-1 s — a large share of the whole radio session's energy.
// Survives deep sleep in RTC memory; invalidated whenever a connect fails.
RTC_DATA_ATTR static uint8_t rtc_ap_bssid[6] = {0};
RTC_DATA_ATTR static uint8_t rtc_ap_channel = 0;
RTC_DATA_ATTR static bool    rtc_ap_cache_valid = false;
// Which network the cache belongs to. Without it a changed SSID kept pinning the OLD router's
// BSSID and channel against the new name: the whole 3 s fast-connect budget was spent on an
// access point that cannot answer for it, and only then did the 10 s all-channel scan begin.
// A change of network saves no restart (the console's state-3 path), so RTC_DATA survives it.
RTC_DATA_ATTR static uint32_t rtc_ap_ssid_tag = 0;

// Not a hash for secrecy — just enough to notice a different name. FNV-1a over the SSID.
static uint32_t ssid_tag(const std::string &s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h ? h : 1u;                 // 0 means "nothing cached"
}

static constexpr int MAX_EVENT_RETRIES = 2; // extra connect attempts inside one session

WiFiManager& WiFiManager::getInstance() {
    static WiFiManager instance;
    return instance;
}

// ---- the access point's own password --------------------------------------------------------------
static char s_ap_pass[PORTAL_AP_PASS_LEN + 1];

// ONE rule, asked in both directions: what the device accepts back from NVS and what it lets the
// owner set. They were two rules, and they disagreed — this one demanded exactly PORTAL_AP_PASS_LEN
// while setApPassword() accepted from PORTAL_AP_PASS_MIN up. So an owner who chose an eight-character
// password had it thrown away as invalid on the next boot: the device drew itself a new one and the
// password they had written down no longer opened the console or the access point. Nothing said so.
static bool ap_pass_valid(const char *s, size_t len)
{
    if (len < PORTAL_AP_PASS_MIN || len > PORTAL_AP_PASS_LEN) return false;
    for (size_t i = 0; i < len; i++)
        if (!strchr(PORTAL_AP_PASS_ALPHABET, s[i]) || s[i] == '\0') return false;
    return true;
}

void WiFiManager::prepareApPassword()
{
    if (s_ap_pass[0]) return;

    NVSManager &nvs = NVSManager::getInstance();
    const bool nvs_ok = nvs.init();
    std::string stored;
    if (nvs_ok && nvs.getString("ap_pass", stored, "") && ap_pass_valid(stored.c_str(), stored.size())) {
        strlcpy(s_ap_pass, stored.c_str(), sizeof(s_ap_pass));
        return;
    }

    // A radio already running feeds the RNG itself; the internal source may only be switched on
    // while none is (ESP-IDF "Random Number Generation", bootloader_random.h).
    const bool rf_active = getInstance().wifi_inited || MatterMode::running();
    if (rf_active) ESP_LOGW(TAG, "AP password drawn with a radio already up");
    else           bootloader_random_enable();

    // The GEN alphabet, not the full one: the owner may type a character they cannot tell from
    // another, but the device choosing one would only make the glass harder to read, which is the
    // one job that screen has.
    const size_t n = sizeof(PORTAL_AP_PASS_GEN_ALPHABET) - 1;
    const uint8_t limit = (uint8_t)(256 / n * n);   // rejection keeps every symbol equally likely
    size_t len = 0;
    while (len < PORTAL_AP_PASS_LEN) {
        uint8_t buf[16];
        esp_fill_random(buf, sizeof(buf));
        for (size_t i = 0; i < sizeof(buf) && len < PORTAL_AP_PASS_LEN; i++)
            if (buf[i] < limit) s_ap_pass[len++] = PORTAL_AP_PASS_GEN_ALPHABET[buf[i] % n];
    }
    s_ap_pass[len] = '\0';

    if (!rf_active) bootloader_random_disable();

    if (nvs_ok && nvs.setString("ap_pass", s_ap_pass))
        ESP_LOGW(TAG, "new access-point password drawn and stored");
    else
        ESP_LOGE(TAG, "new access-point password drawn, NOT stored — valid for this boot only");
}

const char *WiFiManager::apPassword()
{
    prepareApPassword();
    return s_ap_pass;
}

bool WiFiManager::setApPassword(const char *pass)
{
    if (!pass) return false;
    const size_t len = strlen(pass);
    if (!ap_pass_valid(pass, len)) return false;

    memcpy(s_ap_pass, pass, len);
    s_ap_pass[len] = '\0';

    // Kept even if NVS refuses, exactly as a drawn one is: the screen, the QR and the console all
    // read the same variable, so the device stays consistent with what the owner was just told.
    NVSManager &nvs = NVSManager::getInstance();
    if (nvs.init() && nvs.setString("ap_pass", s_ap_pass))
        ESP_LOGW(TAG, "access-point password replaced by the owner");
    else
        ESP_LOGE(TAG, "owner's access-point password NOT stored — valid for this boot only");
    return true;
}

std::string WiFiManager::getIPAddress() const {
    char copy[sizeof(ip_buf)];
    portENTER_CRITICAL(&ip_lock);
    memcpy(copy, ip_buf, sizeof(copy));
    portEXIT_CRITICAL(&ip_lock);
    copy[sizeof(copy) - 1] = '\0';
    return std::string(copy);
}

void WiFiManager::setIP(const char *ip) {
    portENTER_CRITICAL(&ip_lock);
    strlcpy(ip_buf, ip, sizeof(ip_buf));
    portEXIT_CRITICAL(&ip_lock);
}

bool WiFiManager::init() {
    // The order of Espressif's station example (examples/wifi/getting_started/station): esp_netif_init, the
    // default event loop, the default netifs, esp_wifi_init, then the event handlers. The netifs, the loop
    // and the handlers are made once per boot; the driver is loaded here and unloaded by shutdown(), so a
    // second radio window in the same wake loads it again — before, a flag set once per boot skipped
    // esp_wifi_init() after a deinit.
    // Nothing here aborts any more. The realistic failure is ESP_ERR_NO_MEM out of
    // esp_wifi_init(): the driver wants some ninety kilobytes, and the wakes where it might not
    // get them are exactly the ones with a console window open. ESP_ERROR_CHECK turned that into
    // a panic, a reboot, and a tick on the unfinished-boot counter — five of those and the device
    // rolls the image back or goes to safe mode. For a climate monitor the right answer is to skip
    // the radio window: the readings, the schedule and the glass are unaffected, and the next
    // window tries again.
    //
    // Each step is written to survive being entered again after a failure part-way through, which
    // is the part that is easy to get wrong: esp_netif_init() and esp_event_loop_create_default()
    // report ESP_ERR_INVALID_STATE when they have already run, and creating a default netif twice
    // is a genuine leak rather than a no-op.
    static bool stack_done = false, handlers_done = false;
    if (stack_done && handlers_done && wifi_inited) return true;

    auto ok_or_done = [](esp_err_t e) { return e == ESP_OK || e == ESP_ERR_INVALID_STATE; };

    if (!stack_done) {
        ESP_LOGI(TAG, "Initializing Wi-Fi TCP/IP Stack...");
        esp_err_t e = esp_netif_init();
        if (!ok_or_done(e)) { ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(e)); return false; }
        e = esp_event_loop_create_default();
        if (!ok_or_done(e)) { ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(e)); return false; }
        if (!sta_netif) sta_netif = esp_netif_create_default_wifi_sta();
        if (!ap_netif)  ap_netif  = esp_netif_create_default_wifi_ap();
        if (!sta_netif || !ap_netif) { ESP_LOGE(TAG, "netif creation failed"); return false; }
        stack_done = true;
    }

    if (!wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t e = esp_wifi_init(&cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init: %s — skipping this radio window", esp_err_to_name(e));
            return false;
        }
    }

    if (!handlers_done) {
        esp_err_t e = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          &WiFiManager::eventHandler, this, NULL);
        if (e != ESP_OK) { ESP_LOGE(TAG, "wifi handler: %s", esp_err_to_name(e)); return false; }
        e = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &WiFiManager::eventHandler, this, NULL);
        if (e != ESP_OK) { ESP_LOGE(TAG, "ip handler: %s", esp_err_to_name(e)); return false; }
        handlers_done = true;
    }

    if (!wifi_inited) {
        esp_err_t e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (e != ESP_OK) { ESP_LOGE(TAG, "set_storage: %s", esp_err_to_name(e)); return false; }
        wifi_inited = true;
    }
    return true;
}

// Espressif's Wi-Fi deinit phase (api-guides/wifi.rst): esp_wifi_disconnect(), esp_wifi_stop(),
// esp_wifi_deinit(). give_up is set first so the disconnect event does not start a reconnect.
void WiFiManager::shutdown() {
    if (!wifi_inited) return;
    give_up = true;
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    wifi_inited = false;
    connected = false;
    setIP("0.0.0.0");
}

bool WiFiManager::waitConnected(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (!connected && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    return connected;
}

bool WiFiManager::connectSTA(const std::string &ssid, const std::string &password, Attempt how) {
    init();

    give_up = false;
    retry_count = 0;

    wifi_config_t wifi_config = {};
    // Copied by length, not by strncpy: an SSID that fills all 32 bytes leaves no terminator, and
    // password[64] is the next member of the same struct. Nothing here reads the field as a string
    // today, so this is tidiness rather than a fix — but it is the kind of tidiness that stops being
    // optional the moment somebody logs the SSID. The struct is zeroed above, so a short name is
    // already NUL-padded; the names are also length-checked when they are saved (/api/save).
    const size_t sl = ssid.size()     < sizeof(wifi_config.sta.ssid)     ? ssid.size()     : sizeof(wifi_config.sta.ssid);
    const size_t pl = password.size() < sizeof(wifi_config.sta.password) ? password.size() : sizeof(wifi_config.sta.password);
    memcpy(wifi_config.sta.ssid,     ssid.data(),     sl);
    memcpy(wifi_config.sta.password, password.data(), pl);
    // Accept any auth the AP offers (incl. WPA3-SAE phone hotspots); PMF capable
    wifi_config.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    const uint32_t tag = ssid_tag(ssid);
    if (rtc_ap_cache_valid && rtc_ap_ssid_tag != tag) {
        rtc_ap_cache_valid = false;
        ESP_LOGI(TAG, "cached access point belongs to another network — full scan");
    }
    bool try_fast = rtc_ap_cache_valid && how != Attempt::ScanOnly;
    // FastOnly exists to be cheap or to decline. With no cache for this network there is nothing
    // cheap to attempt, and falling through to a full scan is exactly what the caller is avoiding.
    if (how == Attempt::FastOnly && !try_fast) return false;
    if (try_fast) {
        memcpy(wifi_config.sta.bssid, rtc_ap_bssid, 6);
        wifi_config.sta.bssid_set = true;
        wifi_config.sta.channel = rtc_ap_channel;
        wifi_config.sta.scan_method = WIFI_FAST_SCAN;
        ESP_LOGI(TAG, "Fast-connect to %s (ch %d, cached BSSID)...", ssid.c_str(), rtc_ap_channel);
    } else {
        ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s (full scan)...", ssid.c_str());
    }

    // The clock for the two numbers the log could not tell apart before. Started here rather than
    // at the top of the function so it measures the radio, not the NVS reads above it.
    connect_t0 = esp_timer_get_time();
    assoc_ms = 0;
    dhcp_ms  = 0;
    fast_used = try_fast;
    fast_ok = false;

    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (e == ESP_OK) e = esp_wifi_start();
    if (e != ESP_OK) { ESP_LOGE(TAG, "station start: %s", esp_err_to_name(e)); return false; }
    esp_wifi_set_max_tx_power(60); // 15 dBm: plenty indoors, saves TX energy

    if (try_fast) {
        if (waitConnected(3000)) {
            fast_ok = true;
            return true;
        }
        // Cached AP is gone/moved channel — fall back to a full scan
        rtc_ap_cache_valid = false;
        if (how == Attempt::FastOnly) {
            // The caller has other networks to consider and will scan once for all of them. Leave
            // the radio as a failed attempt leaves it, so that scan starts from a known state.
            ESP_LOGW(TAG, "Fast-connect to %s failed — leaving the choice to the scan", ssid.c_str());
            give_up = true;
            esp_wifi_disconnect();
            esp_wifi_stop();
            return false;
        }
        ESP_LOGW(TAG, "Fast-connect failed, falling back to full scan...");
        give_up = true; // silence handler retries during reconfiguration
        esp_wifi_disconnect();
        wifi_config.sta.bssid_set = false;
        wifi_config.sta.channel = 0;
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        esp_err_t ec = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (ec != ESP_OK) { ESP_LOGE(TAG, "station config: %s", esp_err_to_name(ec)); return false; }
        give_up = false;
        retry_count = 0;
        esp_wifi_connect();
    }

    if (waitConnected(10000)) {
        return true;
    }

    // Give up cleanly: radio OFF, no background retries. The profile
    // dispatcher applies a backoff before the next attempt.
    ESP_LOGW(TAG, "Wi-Fi connect FAILED (budget exhausted). Radio off.");
    give_up = true;
    rtc_ap_cache_valid = false;
    esp_wifi_disconnect();          // wifi.rst deinit order: disconnect, then stop
    esp_wifi_stop();
    return false;
}

// One scan, then an answer: which remembered network is in this building, and is it the one we were
// on last time. Returns an index into `nets`, or -1 when none of them is here.
//
// The station has to be running to scan, and `give_up` is raised across it because the STA_START
// handler would otherwise fire esp_wifi_connect() at whatever configuration is loaded and fight the
// scan for the radio. The station is stopped again afterwards so that connectSTA() gets its own
// clean start — it is the STA_START event that triggers the connection, and a station left running
// would never raise one, leaving the attempt to time out against a radio doing nothing.
int WiFiManager::pickFromAir(const WifiSlots::Net *nets, int n)
{
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return -1;
    if (esp_wifi_start() != ESP_OK) return -1;
    esp_wifi_set_max_tx_power(60);
    give_up = true;
    const auto air = scanNetworks();
    esp_wifi_stop();
    give_up = false;
    if (air.empty()) { ESP_LOGW(TAG, "scan found nothing at all"); return -1; }

    // The one that worked last time wins if it is here AT ALL, strength notwithstanding. Choosing
    // the loudest instead would make a device sitting between a router and somebody's hotspot change
    // its mind whenever the signal did, and every change of mind costs a scan.
    for (const auto &a : air) if (a.ssid == nets[0].ssid) {
        ESP_LOGW(TAG, "still at \"%s\"", nets[0].ssid.c_str());
        return 0;
    }
    int best = -1, best_rssi = -127;
    for (int i = 1; i < n; i++)
        for (const auto &a : air)
            if (a.ssid == nets[i].ssid && a.rssi > best_rssi) { best = i; best_rssi = a.rssi; }
    if (best < 0) ESP_LOGW(TAG, "none of the %d remembered networks is here", n);
    else          ESP_LOGW(TAG, "moved: \"%s\" is here (%d dBm)", nets[best].ssid.c_str(), best_rssi);
    return best;
}

bool WiFiManager::connectBest()
{
    WifiSlots::Net nets[WifiSlots::COUNT];
    const int n = WifiSlots::load(nets);
    if (n == 0) return false;
    // One network: the path this device has always taken, byte for byte. Nothing below may make the
    // common case cost more than it did, and the cheapest way to promise that is not to enter it.
    if (n == 1) return connectSTA(nets[0].ssid, nets[0].pass);

    // Still where we were. ssid_tag() means the cache belongs to exactly one remembered network, so
    // this is a 3 s attempt at the right one — and on an ordinary day it is the whole story.
    if (rtc_ap_cache_valid) {
        for (int i = 0; i < n; i++) {
            if (ssid_tag(nets[i].ssid) != rtc_ap_ssid_tag) continue;
            if (connectSTA(nets[i].ssid, nets[i].pass, Attempt::FastOnly)) {
                WifiSlots::promote(nets[i].ssid);
                return true;
            }
            break;
        }
    }

    const int idx = pickFromAir(nets, n);
    if (idx < 0) return false;
    // ScanOnly: the scan above already said this network is here, and the cache — if there even is
    // one — belongs to a different place. Spending three more seconds on it would be spending them
    // on an answer we have.
    if (!connectSTA(nets[idx].ssid, nets[idx].pass, Attempt::ScanOnly)) return false;
    WifiSlots::promote(nets[idx].ssid);
    return true;
}

bool WiFiManager::connectSTAKeepAP(const std::string &ssid, const std::string &password) {
    // The portal already put us in APSTA mode (startAP). Configure + connect the
    // STA interface only — do NOT change the mode or stop Wi-Fi, so the AP (and
    // the phone's portal session) stay alive throughout.
    if (ssid.empty()) return false;
    ESP_LOGI(TAG, "Portal: connecting STA to %s while keeping AP up...", ssid.c_str());

    connected = false;
    give_up = false;
    retry_count = 0;

    wifi_config_t sta = {};
    // By length, for the reason given in connectSTA() above.
    const size_t sl2 = ssid.size()     < sizeof(sta.sta.ssid)     ? ssid.size()     : sizeof(sta.sta.ssid);
    const size_t pl2 = password.size() < sizeof(sta.sta.password) ? password.size() : sizeof(sta.sta.password);
    memcpy(sta.sta.ssid,     ssid.data(),     sl2);
    memcpy(sta.sta.password, password.data(), pl2);
    sta.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    if (esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK) return false;
    esp_wifi_disconnect();          // drop any half-open association first
    esp_wifi_connect();

    if (waitConnected(12000)) {
        ESP_LOGI(TAG, "Portal STA connected: %s (AP still up)", getIPAddress().c_str());
        return true;
    }
    // Failure: keep the AP running (no esp_wifi_stop). Silence handler retries.
    give_up = true;
    esp_wifi_disconnect();
    ESP_LOGW(TAG, "Portal STA connect failed — AP kept up for retry.");
    return false;
}

bool WiFiManager::startAP() {
    // Normally prepared at boot, before any radio. If a caller got here first with the driver already
    // loaded, the password is drawn without the extra entropy source (logged) rather than not at all.
    prepareApPassword();
    init();
    give_up = true; // no STA reconnect attempts while hosting the portal
    ESP_LOGI(TAG, "Starting configuration Access Point: %s ...", PORTAL_AP_SSID);

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, PORTAL_AP_SSID, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(PORTAL_AP_SSID);
    strncpy((char*)wifi_config.ap.password, apPassword(), sizeof(wifi_config.ap.password));
    wifi_config.ap.max_connection = 4;
    // WPA2 (not open): required for the WIFI: QR code to auto-join phones
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    // Protected management frames offered to phones that support them. ESP-IDF's softAP example makes
    // PMF required together with WPA3; this AP stays WPA2 for the QR code, so PMF is capable, not required.
    wifi_config.ap.pmf_cfg.capable = true;
    wifi_config.ap.pmf_cfg.required = false;

    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (e == ESP_OK) e = esp_wifi_start();
    if (e != ESP_OK) { ESP_LOGE(TAG, "access point start: %s", esp_err_to_name(e)); return false; }

    char ip_str[16];
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif, &ip_info);
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));

    setIP(ip_str);
    connected = false;

    // DHCP option 114, the captive portal URI (RFC 8910), set as Espressif's captive portal example does
    // (dhcp_set_captiveportal_url): stop the DHCP server, set the option, start it again. esp_netif keeps
    // the pointer rather than a copy (esp_netif.h), so the string is static.
    static char portal_uri[32];
    snprintf(portal_uri, sizeof(portal_uri), "http://%s", ip_str);
    esp_netif_dhcps_stop(ap_netif);
    if (esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                               portal_uri, strlen(portal_uri)) != ESP_OK)
        ESP_LOGW(TAG, "DHCP captive portal option not set");
    esp_netif_dhcps_start(ap_netif);

    ESP_LOGI(TAG, "Access Point \"%s\" started. IP: %s", PORTAL_AP_SSID, ip_str);
    return true;
}

// WIFI_AUTH_WPA2_ENTERPRISE is the SAME value as WIFI_AUTH_ENTERPRISE, so only one of them may
// appear here; the other three are separate. Anything not named is treated as a shared password,
// which is the safe default: the device will try, and a try is what it does today anyway.
static const char *auth_word(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:
    case WIFI_AUTH_OWE:                  return "open";
    // WIFI_AUTH_WPA2_ENTERPRISE is this same value and must not appear beside it.
    case WIFI_AUTH_ENTERPRISE:
    case WIFI_AUTH_WPA_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:         return "enterprise";
    default:                             return "psk";
    }
}

std::vector<WiFiNetwork> WiFiManager::scanNetworks() {
    std::vector<WiFiNetwork> results;

    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    ESP_LOGI(TAG, "Starting Wi-Fi network scan...");
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(err));
        return results;
    }

    uint16_t ap_count = 15;
    wifi_ap_record_t ap_info[15];
    err = esp_wifi_scan_get_ap_records(&ap_count, ap_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP records: %s", esp_err_to_name(err));
        return results;
    }

    ESP_LOGI(TAG, "Scan finished. Found %d APs.", ap_count);
    for (int i = 0; i < ap_count; i++) {
        WiFiNetwork net;
        net.ssid = std::string((char*)ap_info[i].ssid);
        net.rssi = ap_info[i].rssi;
        net.secure = (ap_info[i].authmode != WIFI_AUTH_OPEN);
        net.auth   = auth_word(ap_info[i].authmode);
        results.push_back(net);
    }

    return results;
}

WiFiDetails WiFiManager::getDetails() {
    WiFiDetails d;
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        char b[18];
        snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        d.mac = b;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        d.rssi = ap.rssi;
        d.channel = ap.primary;
    }
    if (sta_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK) {
            char b[16]; esp_ip4addr_ntoa(&ip.gw, b, sizeof(b)); d.gw = b;
        }
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            char b[16]; esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, b, sizeof(b)); d.dns = b;
        }
    }
    return d;
}

void WiFiManager::eventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    WiFiManager* self = (WiFiManager*)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Espressif's station example connects on STA_START. The station is also started by the setup
        // access point (APSTA, no network chosen yet), and startAP() sets give_up first: no connect then.
        if (!self->give_up) esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        // Associated, but not yet addressed. Everything after this point is DHCP.
        if (self->connect_t0) {
            const int64_t d = (esp_timer_get_time() - self->connect_t0) / 1000;
            self->assoc_ms = (uint16_t)(d < 0 ? 0 : (d > 65535 ? 65535 : d));
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        self->connected = false;
        self->setIP("0.0.0.0");
        if (!self->give_up && self->retry_count < MAX_EVENT_RETRIES) {
            self->retry_count++;
            ESP_LOGI(TAG, "STA disconnected, retry %d/%d...", self->retry_count, MAX_EVENT_RETRIES);
            esp_wifi_connect();
        }
        // Otherwise: stay silent. connectSTA()'s time budget decides failure.
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (self->connect_t0) {
            const int64_t d = (esp_timer_get_time() - self->connect_t0) / 1000 - self->assoc_ms;
            self->dhcp_ms = (uint16_t)(d < 0 ? 0 : (d > 65535 ? 65535 : d));
        }
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        self->connected = true;
        self->retry_count = 0;
        self->setIP(ip_str);

        // Cache the AP for next wake's fast-connect
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(rtc_ap_bssid, ap.bssid, 6);
            rtc_ap_channel = ap.primary;
            wifi_config_t cur = {};
            if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK)
                rtc_ap_ssid_tag = ssid_tag(std::string((const char *)cur.sta.ssid));
            rtc_ap_cache_valid = true;
        }
        ESP_LOGI(TAG, "Wi-Fi STA Got IP: %s (ch %d cached for fast-connect)", ip_str, rtc_ap_channel);
    }
}
