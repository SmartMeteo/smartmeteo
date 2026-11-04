#include "clock_sync.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_netif_sntp.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char* TAG = "CLOCK";

// Wall clock sanity floor: anything below this means "never NTP-synced"
static constexpr time_t TIME_VALID_FLOOR = 1750000000; // 2025-06-15
// Routine resync period. The board runs on an external 32.768 kHz crystal, verified as actually
// oscillating at boot, so the clock drifts by seconds a week, not minutes: at a pessimistic 30 ppm
// (datasheet tolerance plus the tuning fork's temperature curve indoors) a week accumulates ~18 s,
// which is invisible on a display that shows minutes. A month would start to show, so a week it is.
// The saving is small while weather keeps the radio busy anyway — it matters when weather is off
// and NTP is the only reason to raise the radio at all.
static constexpr time_t NTP_RESYNC_SEC   = 7 * 24 * 3600;
static constexpr time_t NTP_RETRY_SEC    = 6 * 3600;      // after a failed attempt
// The same while the clock has never been set: the device needs the time, so it asks sooner — but not
// in every radio window, where a network that does not answer NTP would cost each one 30 s.
static constexpr time_t NTP_RETRY_UNSYNCED_SEC = 30 * 60;

// NOINIT, not DATA: RTC_DATA is wiped by esp_restart (which the setup wizard ends with), and a
// wiped timestamp means an immediate pointless resync. Garbage after a real power loss is caught
// by the sanity check in maintainClock rather than by a magic word.
RTC_NOINIT_ATTR static time_t rtc_last_ntp_sync;
// When the last attempt failed, so a network that does not answer NTP is asked again after NTP_RETRY_SEC
// rather than in every radio window. NOINIT for the same reason; sanity-checked where it is read.
RTC_NOINIT_ATTR static time_t rtc_ntp_failed_at;

// How many attempts in a row have failed, and the only reason it exists: a FLAT retry is the same
// mistake the Wi-Fi join path made until a router switched off for a weekend flattened a cell.
// An attempt that cannot succeed costs up to 30 s of radio (the lwIP budget below), and on a network
// that drops UDP/123 — some corporate and captive ones do — four of those a day is about 3 mA.h
// against a measured total of 13.66, which DOUBLES the radio line and keeps doubling it for ever.
// There is no symptom either: the clock was set once and goes on running.
//
// Standing further back costs nothing. This crystal drifts a few ppm (and the sync below measures
// it, rather than trusting a datasheet), so an extra day between syncs is under a second.
RTC_NOINIT_ATTR static uint8_t rtc_ntp_fails;

// The unsynced ladder is shorter and lower because the device actually needs the time: it should
// keep asking, just not in every window. Declared in the header — see there for why.
time_t ClockSync::retryAfter(bool clock_valid, unsigned fails)
{
    const time_t   base = clock_valid ? NTP_RETRY_SEC : NTP_RETRY_UNSYNCED_SEC;
    const unsigned cap  = clock_valid ? 2u : 3u;
    const unsigned n    = fails > 1 ? fails - 1 : 0;
    return base << (n < cap ? n : cap);
}

ClockSync& ClockSync::getInstance() {
    static ClockSync instance;
    return instance;
}

void ClockSync::forceClockSync() {
    rtc_last_ntp_sync = 0;
    rtc_ntp_failed_at = 0;
    rtc_ntp_fails     = 0;   // "ask now" means now, not after the ladder the last network earned
}

void ClockSync::maintainClock() {
    if (!WiFiManager::getInstance().isConnected()) return;

    time_t now = time(NULL);
    bool valid = now > TIME_VALID_FLOOR;
    // A stamp in the future, or one from before the clock was ever set, is uninitialised RTC
    // memory. Left alone it would read as "synced very recently" and suppress every sync forever.
    if (rtc_last_ntp_sync < TIME_VALID_FLOOR || rtc_last_ntp_sync > now) rtc_last_ntp_sync = 0;
    if (valid && (now - rtc_last_ntp_sync) < NTP_RESYNC_SEC) return;
    if (rtc_ntp_failed_at > now) rtc_ntp_failed_at = 0;
    // Uninitialised RTC memory again: the counter only ever walks up to the cap in normal use, so
    // anything past it is somebody else's bytes, and a large one would hold the retry off for ever.
    if (rtc_ntp_fails > 8) rtc_ntp_fails = 0;
    if (rtc_ntp_failed_at > rtc_last_ntp_sync &&
        (now - rtc_ntp_failed_at) < retryAfter(valid, rtc_ntp_fails)) return;

    const time_t  prev_sync = rtc_last_ntp_sync;
    const time_t  before    = now;
    const int64_t t_enter   = esp_timer_get_time();

    ESP_LOGI(TAG, "Syncing clock via SNTP...");
    // Espressif's SNTP example (examples/protocols/sntp): esp_netif_sntp_init() with the default config,
    // esp_netif_sntp_sync_wait() for the sync event, esp_netif_sntp_deinit(). The wait's result decides.
    // A clock that was already valid before the request says nothing about this sync, and judging by it
    // stamped every failed attempt as a sync and put the next one a week away.
    // The first request leaves after a random delay of up to CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY (5 s,
    // RFC 4330), so the wait covers that and the exchange.
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    bool synced = false;
    if (esp_netif_sntp_init(&config) == ESP_OK) {
        // The example's wait: 2 s at a time, up to 15 times. lwIP retries a lost request only after
        // SNTP_RECV_TIMEOUT (15 s), so a shorter budget turned one dropped packet into a failed sync.
        esp_err_t r = ESP_ERR_TIMEOUT;
        for (int retry = 0; retry < 15 && r == ESP_ERR_TIMEOUT; retry++) {
            r = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
            esp_task_wdt_reset();          // up to 30 s inside a radio window, on the cycle task
        }
        synced = (r == ESP_OK);
        esp_netif_sntp_deinit();
    }

    now = time(NULL);
    if (synced && now > TIME_VALID_FLOOR) {
        // How far had this particular crystal drifted? The correction is what NTP just moved the
        // clock by, minus the time the sync itself took. Measured, not assumed from a datasheet —
        // it is the only way to know whether a week between syncs is generous or optimistic.
        const double waited = (esp_timer_get_time() - t_enter) / 1000000.0;
        const double corr   = (double)(now - before) - waited;
        if (prev_sync > TIME_VALID_FLOOR && now > prev_sync) {
            const double span = (double)(now - prev_sync);
            ESP_LOGW(TAG, "clock drift: %+.1f s over %.1f h = %+.1f ppm",
                     corr, span / 3600.0, corr / span * 1e6);
        }
        rtc_last_ntp_sync = now;
        rtc_ntp_fails     = 0;
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        ESP_LOGI(TAG, "Clock synced: %04d-%02d-%02d %02d:%02d UTC",
                 tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                 tm_utc.tm_hour, tm_utc.tm_min);
    } else {
        rtc_ntp_failed_at = now;
        if (rtc_ntp_fails < 8) rtc_ntp_fails++;
        const time_t next = retryAfter(valid, rtc_ntp_fails);
        ESP_LOGW(TAG, "SNTP sync failed (no time source reachable), %u in a row — next attempt in %u min",
                 (unsigned)rtc_ntp_fails, (unsigned)(next / 60));
    }
}
