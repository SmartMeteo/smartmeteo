#include "wifi_slots.h"
#include "nvs_manager.h"
#include "esp_log.h"

static const char *TAG = "WIFI_SLOTS";

namespace WifiSlots {

const char *ssidKey(int i) { static const char *k[COUNT] = {"ssid", "ssid2", "ssid3"}; return k[i]; }
const char *passKey(int i) { static const char *k[COUNT] = {"pass", "pass2", "pass3"}; return k[i]; }

int load(Net out[COUNT])
{
    NVSManager &nv = NVSManager::getInstance();
    int n = 0;
    for (int i = 0; i < COUNT; i++) {
        std::string s, p;
        nv.getString(ssidKey(i), s, "");
        if (s.empty()) continue;           // a gap must not hide what is behind it
        nv.getString(passKey(i), p, "");
        out[n].ssid = s; out[n].pass = p; n++;
    }
    for (int i = n; i < COUNT; i++) { out[i].ssid.clear(); out[i].pass.clear(); }
    return n;
}

// The one write path. Everything else is a wrapper, so the list can only ever be rewritten whole —
// a half-written rotation is how a device loses the network it is standing in.
static void store(const Net *list, int n)
{
    NVSManager &nv = NVSManager::getInstance();
    for (int i = 0; i < COUNT; i++) {
        if (i < n) { nv.setString(ssidKey(i), list[i].ssid); nv.setString(passKey(i), list[i].pass); }
        else       { nv.setString(ssidKey(i), "");           nv.setString(passKey(i), "");           }
    }
}

// Move `ssid` to the front. `pass` is used when it is given (a new network, or a changed password)
// and the stored one is kept when it is not — the console never sends a password back, so a promote
// after a successful connection must not wipe the very credential that worked.
static void front(const std::string &ssid, const std::string *pass)
{
    if (ssid.empty()) return;
    Net cur[COUNT]; const int n = load(cur);
    Net next[COUNT];
    next[0].ssid = ssid;
    next[0].pass = pass ? *pass : std::string();
    int m = 1;
    for (int i = 0; i < n && m < COUNT; i++) {
        if (cur[i].ssid == ssid) {
            if (!pass) next[0].pass = cur[i].pass;   // keep what we had
            continue;                                // and never list it twice
        }
        next[m++] = cur[i];
    }
    store(next, m);
    ESP_LOGW(TAG, "remembered networks: %d, newest \"%s\"", m, ssid.c_str());
}

void remember(const std::string &ssid, const std::string &pass) { front(ssid, &pass); }
void promote(const std::string &ssid)                           { front(ssid, nullptr); }

bool forget(const std::string &ssid)
{
    if (ssid.empty()) return false;
    Net cur[COUNT]; const int n = load(cur);
    Net next[COUNT]; int m = 0; bool hit = false;
    for (int i = 0; i < n; i++) {
        if (cur[i].ssid == ssid) { hit = true; continue; }
        next[m++] = cur[i];
    }
    if (hit) { store(next, m); ESP_LOGW(TAG, "forgot \"%s\", %d left", ssid.c_str(), m); }
    return hit;
}

void clear()
{
    Net none[COUNT];
    store(none, 0);
    ESP_LOGW(TAG, "all remembered networks cleared");
}

}  // namespace WifiSlots
