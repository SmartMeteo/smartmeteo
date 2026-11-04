#include "ota_check.h"
#include "fw_version.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include "https_client.h"
#include "esp_mac.h"
#include "ArduinoJson.h"
#include <string.h>
#include <vector>

static const char *TAG = "OTA_CHK";

// The host is compiled in and the manifest may only ever supply a PATH. This is the difference
// between a server that can offer us a new version and a server that can point us anywhere: even
// with the machine fully taken over, the bytes still have to come from here and still have to carry
// our signature. A URL in a manifest would hand both of those away.
#define OTA_HOST      "ota.smartmeteo.org"
#define OTA_MANIFEST  "https://" OTA_HOST "/v1/esp32c6/stable.json"

#define CHECK_EVERY_S   86400        // once a day
#define CHECK_LATEST_S 172800        // ...and at the latest after two, see below

namespace OtaCheck {

char     available_ver[16] = {0};
uint32_t available_seq     = 0;
time_t   last_checked      = 0;
char     last_error[24]    = {0};
char     available_path[128] = {0};   // where the image is; validated, and now actually kept

static void clearOffer(void)
{ available_ver[0] = '\0'; available_seq = 0; available_path[0] = '\0'; }

// The hour of the day this particular device asks. Derived from its own MAC so that a hundred
// devices do not all wake the server at midnight — and so that the spread needs no coordination,
// no server-side scheduling and no identifier sent anywhere.
static int my_hour(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    return mac[5] % 24;
}

bool due(time_t now)
{
    if (now < 1600000000) return false;               // no clock, no schedule

    uint8_t on = 1;
    NVSManager::getInstance().getU8("ota_auto", on, 1);   // on by default: a stranger who never
    if (!on) return false;                                // opens the console still gets fixes

    uint32_t seen = 0;
    NVSManager::getInstance().getU32("ota_seen", seen, 0);
    const time_t since = now - (time_t)seen;

    struct tm lt; gmtime_r(&now, &lt);
    // Two clauses, and the second one is not decoration. A device whose radio windows never happen
    // to land in its own hour would otherwise never check at all — quietly, for ever, with nothing
    // in any log to say so. After two days it stops caring which hour it is.
    if (since >= CHECK_LATEST_S) return true;
    return (lt.tm_hour == my_hour()) && (since >= CHECK_EVERY_S);
}

// A path we are willing to fetch. Not paranoia about our own server: a path is the one field an
// attacker who owns the server still controls, and "//evil.example/x" is a URL wearing a path's
// clothes to any client that resolves it. Everything outside the shape we publish is refused.
static bool path_ok(const char *p)
{
    if (!p || strncmp(p, "/v1/", 4) != 0) return false;
    if (strstr(p, "..") || strstr(p, "//") || strstr(p, "://")) return false;
    for (const char *c = p; *c; c++)
        if (!(isalnum((unsigned char)*c) || *c == '/' || *c == '.' || *c == '-' || *c == '_'))
            return false;
    return strlen(p) < 120;
}

void run(void)
{
    const time_t now = time(NULL);
    // The ATTEMPT is recorded, never the success. Stamping only successes is how a device with no
    // route to the server decides it is permanently overdue and retries in every single window —
    // the same trap that once turned 48 daily radio sessions into 288 (CLAUDE.md 4.22).
    const uint32_t stamp = (uint32_t)(now > 1600000000 ? now : 0);
    NVSManager::getInstance().setU32("ota_seen",  stamp);   // drives the SCHEDULE
    // A second copy, for the console to display. The two look redundant and are not: a manual
    // check deliberately puts "ota_seen" back so that pressing the button cannot postpone the
    // automatic check — and that also rewound what the card showed, so the card said "checked 23 h
    // ago" one second after you watched it check. One field cannot answer both questions.
    NVSManager::getInstance().setU32("ota_shown", stamp);
    last_checked = now;
    // Start from what was learned last time, not from nothing. A check that never reaches the
    // server has not discovered that there is no update — it has discovered nothing, and wiping
    // the pending version on a night when the router was off would tell the owner "up to date"
    // while an update was in fact waiting. Only an answer FROM the manifest replaces the answer.
    restore();
    last_error[0] = '\0';

    // Verified TLS like every outbound call here, with ten seconds for the whole body (https_client.h).
    Https::Options opt;
    opt.transfer_us = 10LL * 1000 * 1000;
    std::vector<char> buf(1024);
    const Https::Body got = Https::get(OTA_MANIFEST, buf.data(), (int)buf.size(), opt);
    if (got.err == ESP_ERR_NO_MEM) { snprintf(last_error, sizeof(last_error), "init"); persist(); return; }

    if (got.status != 200) {
        // "http 0" when the connection was never made, as this field has always said.
        snprintf(last_error, sizeof(last_error), "http %d", got.err == ESP_OK ? got.status : 0);
        ESP_LOGW(TAG, "manifest fetch failed: %s", last_error);
        persist();               // record WHY, keep whatever was already on offer
        return;
    }

    if (!got.complete) {
        snprintf(last_error, sizeof(last_error), "incomplete");
        ESP_LOGW(TAG, "manifest transfer incomplete after %d bytes", got.len);
        persist();
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, buf.data()) != DeserializationError::Ok) {
        snprintf(last_error, sizeof(last_error), "bad json");
        ESP_LOGW(TAG, "manifest is not readable");
        persist();
        return;
    }

    const uint32_t seq     = doc["seq"]     | 0u;
    const uint32_t min_seq = doc["min_seq"] | 0u;
    const char    *ver     = doc["ver"]     | (const char *)nullptr;
    const char    *path    = doc["path"]    | (const char *)nullptr;
    // Taken only if it is really a string. The publisher may leave it out; the installer treats an
    // absent digest as "no early check" and still refuses anything the signature rejects.
    const char    *sha     = doc["sha256"]  | (const char *)nullptr;

    uint32_t bad = 0;
    NVSManager::getInstance().getU32("ota_bad", bad, 0);
    // A blacklist entry at or below the running build is dead: the manifest is refused as "up to
    // date" before the entry is ever consulted, so it can never fire again. What it does do is
    // survive for ever and keep telling the owner that some build failed here — a note about a
    // version that is now ancient history, on a device that has updated past it many times.
    // Clear it, and say so once. Entries ABOVE the running build are the live ones and stay:
    // that is the image this device reverted out of, and it must not be fetched again.
    if (bad && bad <= (uint32_t)FW_SEQ) {
        ESP_LOGW(TAG, "clearing stale blacklist for seq %u — this build is seq %u and newer",
                 (unsigned)bad, (unsigned)FW_SEQ);
        NVSManager::getInstance().setU32("ota_bad", 0);
        bad = 0;
    }

    // Every reason to say no, in the order that makes the log useful.
    if (!seq || !ver || !path_ok(path)) {
        snprintf(last_error, sizeof(last_error), "malformed");
        ESP_LOGW(TAG, "manifest rejected: seq=%u path=%s", (unsigned)seq, path ? path : "(none)");
        clearOffer(); persist();
        return;
    }
    if (seq == bad) {
        snprintf(last_error, sizeof(last_error), "known bad");
        ESP_LOGW(TAG, "seq %u already failed on this device — not offering it again", (unsigned)seq);
        clearOffer(); persist();
        return;
    }
    if (seq <= (uint32_t)FW_SEQ) {
        ESP_LOGW(TAG, "firmware is up to date (seq %u)", (unsigned)FW_SEQ);
        clearOffer(); persist();
        return;
    }
    // min_seq is the publisher's way of saying "do not jump straight here from something older",
    // for the release where the step itself matters — a partition change, a state format, a key.
    if (min_seq > (uint32_t)FW_SEQ) {
        snprintf(last_error, sizeof(last_error), "needs seq %u", (unsigned)min_seq);
        ESP_LOGW(TAG, "seq %u requires at least seq %u first", (unsigned)seq, (unsigned)min_seq);
        clearOffer(); persist();
        return;
    }

    available_seq = seq;
    snprintf(available_ver, sizeof(available_ver), "%s", ver);
    snprintf(available_path, sizeof(available_path), "%s", path);
    NVSManager::getInstance().setString("ota_sha", std::string(sha ? sha : ""));
    ESP_LOGW(TAG, "update available: %s (seq %u), path %s", available_ver, (unsigned)seq, path);
    persist();
}

// Everything above lives in .bss, and this device cold-boots out of deep sleep roughly every
// minute. So the result of a check survived exactly as long as the wake that produced it — and a
// check happens once a DAY. The console's firmware card, built precisely so that a channel which
// is silent by design can still be read, therefore showed "never checked / up to date" on almost
// every wake, whatever had actually been found. Persisting costs one NVS write a day.
void persist(void)
{
    NVSManager &n = NVSManager::getInstance();
    n.setU32("ota_seq", available_seq);
    n.setString("ota_ver", std::string(available_ver));
    n.setString("ota_err", std::string(last_error));
    n.setString("ota_path", std::string(available_path));
}

void restore(void)
{
    NVSManager &n = NVSManager::getInstance();
    std::string s;
    n.getU32("ota_seq", available_seq, 0);
    if (n.getString("ota_ver",  s)) snprintf(available_ver,  sizeof(available_ver),  "%s", s.c_str());
    if (n.getString("ota_err",  s)) snprintf(last_error,     sizeof(last_error),     "%s", s.c_str());
    if (n.getString("ota_path", s)) snprintf(available_path, sizeof(available_path), "%s", s.c_str());
    // An offer that is not NEWER than what is running is not an offer. Without this the version
    // just installed stays in flash as "available" and the console announces the firmware to
    // itself, with a live Install button that can only be refused.
    if (available_seq && available_seq <= (uint32_t)FW_SEQ) {
        available_seq = 0;
        available_ver[0]  = '\0';
        available_path[0] = '\0';
    }
    uint32_t shown = 0;
    n.getU32("ota_shown", shown, 0);
    if (!shown) n.getU32("ota_seen", shown, 0);   // devices that predate the display stamp
    last_checked = (time_t)shown;
}

}  // namespace OtaCheck
