#pragma once

#include <vector>
#include <string>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "ap_password.h"

// Config portal access point. The name is shared; the password is this device's own (see
// WiFiManager::prepareApPassword) and both are drawn on the setup screen with their QR code.
#define PORTAL_AP_SSID "Smart Meteo"
// The access-point password rules live in their own header — see ap_password.h.


namespace WifiSlots { struct Net; }   // wifi_slots.h — forward-declared, only a pointer is needed

struct WiFiNetwork {
    std::string ssid;
    int rssi;
    bool secure;
    // What kind of door the network has, in the only vocabulary anyone needs: "open", "psk",
    // "enterprise". The FIRMWARE decides, because the firmware owns esp_wifi's enum — a console
    // mapping those numbers itself would go wrong the first time ESP-IDF adds one, and it already
    // has five enterprise values where a reader would expect one.
    //
    // It exists because "enterprise" is the one answer that means "this device cannot join this
    // network at all" (there is no EAP anywhere in this firmware, and connectSTA asks for
    // WIFI_AUTH_WPA_WPA2_PSK). Until 2026-10-01 the scan collapsed all of this into `secure`, so a
    // company network looked exactly like a home one and failed with "Could not join" — the same
    // words a mistyped password gives, with nothing to tell the two apart.
    const char *auth;
};

struct WiFiDetails {
    std::string mac = "--";
    std::string gw = "--";
    std::string dns = "--";
    int rssi = 0;
    int channel = 0;
};

class WiFiManager {
public:
    static WiFiManager& getInstance();

    bool init();          // false = the driver did not come up; skip the window, do not panic
    // Espressif's deinit order: disconnect, stop, deinit. init() loads the driver again afterwards.
    void shutdown();

    // Blocking STA connect with a hard time budget (~12 s worst case):
    // 1) fast-connect using the RTC-cached channel+BSSID (~0.3-1 s), then
    // 2) full-scan connect fallback (up to ~10 s).
    // Returns true when an IP is obtained. On failure the radio is stopped —
    // there is NO endless background reconnect (battery safety); the caller
    // decides when to try again (profile backoff).
    // Which halves of that budget to spend. Auto is what a device with one saved network has always
    // done. With several, the 10 s fallback is the wrong move — ten seconds hunting for a network
    // that is not in this building, once per network — so connectBest() asks for the halves apart:
    // the cheap cached attempt first, and after one scan has NAMED the network, the scan half only.
    enum class Attempt { Auto, FastOnly, ScanOnly };
    bool connectSTA(const std::string &ssid, const std::string &password, Attempt how = Attempt::Auto);

    // Join whichever REMEMBERED network is actually in the air (wifi_slots.h).
    //
    // The shape of this is the whole point, because the obvious shape is ruinous: trying each saved
    // network in turn costs up to 10 s of all-channel scanning EACH, against a radio window that
    // today runs 2.3 s and is already the second-largest item in the power budget. So:
    //   1. one saved network  — exactly the old path, not a millisecond added;
    //   2. still where we were — the cached BSSID answers in ~270 ms and nothing is scanned at all
    //      (the cache is tagged with the SSID, so it can only belong to one of them);
    //   3. we moved          — ONE scan decides, then a single connect to a network known to be here.
    // The everyday cost is therefore zero, and the cost of travelling is one scan.
    bool connectBest();

    // Connect the STA interface WITHOUT tearing down a running portal AP
    // (the mode is already APSTA from startAP). Used in the config portal so
    // the device gets internet — for city geocoding and OWM key validation —
    // while the phone stays on the portal page. On failure the AP is kept up
    // (no esp_wifi_stop). Returns true when an IP is obtained.
    bool connectSTAKeepAP(const std::string &ssid, const std::string &password);

    bool startAP();       // false = the access point did not come up

    // The access point's password. A single password printed on every unit is exactly the default
    // credential EN 18031 rules out, so each device draws its own from the hardware RNG, keeps it in
    // NVS ("ap_pass") and gets a new one with a factory reset. Call prepareApPassword() while no radio
    // has been started in this boot: without a radio the RNG is only true-random inside
    // bootloader_random_enable()/disable(), and that entropy source must not be on together with the
    // RF subsystem (ESP-IDF "Random Number Generation"). If NVS cannot keep it, the password lives for
    // this boot only — still random, still the one on the glass.
    static void prepareApPassword();
    static const char *apPassword();   // prepares on first use if the boot did not

    // Replace it with one the owner chose. Same alphabet and length limit as a drawn one, and for a
    // concrete reason rather than taste: this string is printed on the e-ink setup screen from
    // pre-rendered glyph cells and carried inside its QR code (portal_screen.cpp). A character with
    // no cell makes compose() refuse, and the screen then loses both the code and the password —
    // locking the owner out of the very thing the screen exists to tell them. Rejects anything
    // outside PORTAL_AP_PASS_ALPHABET or not exactly PORTAL_AP_PASS_LEN characters long, a length
    // the generator has already proved fits the line in its widest case.
    static bool setApPassword(const char *pass);

    std::vector<WiFiNetwork> scanNetworks();
    bool isConnected() const { return connected; }

    // Where a radio window's connect time actually goes. Association and DHCP are separate
    // questions with separate answers: a slow association says the cached channel missed and the
    // scan ran, a slow DHCP says the lease is worth remembering across sleep. Measured from
    // esp_wifi_start(); zero when that step did not happen. The log carries both, because until
    // 2026-09-20 the only number recorded was the whole window — connect and payload together —
    // and no decision could be made from it.
    uint16_t lastAssocMs()  const { return assoc_ms; }
    uint16_t lastDhcpMs()   const { return dhcp_ms;  }
    bool     lastFastUsed() const { return fast_used; }
    bool     lastFastOk()   const { return fast_ok;  }
    std::string getIPAddress() const;
    WiFiDetails getDetails();  // real MAC/RSSI/channel/gateway/DNS when connected

private:
    WiFiManager() : connected(false), give_up(false), retry_count(0),
                    sta_netif(nullptr), ap_netif(nullptr), wifi_inited(false) {}
    ~WiFiManager() = default;
    WiFiManager(const WiFiManager&) = delete;
    WiFiManager& operator=(const WiFiManager&) = delete;

    static void eventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    bool waitConnected(uint32_t timeout_ms);

    // Set by connectSTA(), filled by the event handler on the event task. Plain integers written
    // once and read after the wait returns, so no lock: the reader runs only after waitConnected().
    int64_t  connect_t0 = 0;
    volatile uint16_t assoc_ms = 0, dhcp_ms = 0;
    volatile bool fast_used = false, fast_ok = false;

    volatile bool connected;
    volatile bool give_up;      // stop auto-reconnect (set while backing off / in AP mode)

    // Member rather than a free function because it has to raise give_up across the scan: the
    // STA_START handler would otherwise fire esp_wifi_connect() at whatever configuration is
    // loaded and fight the scan for the radio.
    int pickFromAir(const WifiSlots::Net *nets, int n);
    int retry_count;
    // Written by the Wi-Fi event handler (event task), read by the cycle and the httpd task: a plain
    // buffer copied under a spinlock, not a std::string assigned and copied at the same time.
    char ip_buf[16] = "0.0.0.0";
    mutable portMUX_TYPE ip_lock = portMUX_INITIALIZER_UNLOCKED;
    void setIP(const char *ip);
    esp_netif_t* sta_netif;
    esp_netif_t* ap_netif;
    bool wifi_inited;           // esp_wifi_init() done and not yet undone by shutdown()
};
