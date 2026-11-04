#include "hist_store.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_task_wdt.h"
#include <string.h>

static const char* TAG = "HIST";
static constexpr uint32_t REC = sizeof(HistRec);      // 32; the static_assert in the header is
                                                      // what actually holds it there
static constexpr uint32_t SECTOR = 4096;              // flash erase granularity
static constexpr uint32_t SECTOR_RECS = SECTOR / REC; // 128
static constexpr uint32_t T_MIN = 1600000000;         // 2020 — sanity floor
// Fallback ceiling, used only when the clock has never been set — the real "not from the future"
// test is recover()'s now + 1 day. This was 2100000000, which is 18 July 2036, and it is read as a
// hard validity limit by query() and newestTimestamp(). On that date every record written from then
// on would have started failing the test: both graphs blank, newestTimestamp() returning 0, and
// init() declaring the head unusable so that all 2 MB were rescanned on every one of the ~1440 wakes
// a day. The log itself would have been intact throughout, which is what would have made it hard to
// diagnose. 4000000000 is 2096, still far below the 0xFFFFFFFF that erased flash reads as.
static constexpr uint32_t T_MAX = 4000000000;

// Write head + count must survive BOTH deep sleep AND esp_restart (the USB path reboots the
// chip every cycle). RTC_DATA_ATTR re-initializes on esp_restart (§3 bug) -> head reset to 0
// every USB cycle, forcing a full 2 MB rescan each measure tick and making the ring fragile.
// RTC_NOINIT_ATTR survives reflash/reset/esp_restart and dies only on real power loss; a magic
// word distinguishes "valid" from "garbage after power loss" (then we recover() by scanning).
RTC_NOINIT_ATTR static uint32_t rtc_hist_head;
RTC_NOINIT_ATTR static uint32_t rtc_hist_count;
RTC_NOINIT_ATTR static uint32_t rtc_hist_magic;
static constexpr uint32_t HIST_MAGIC = 0x48495355;   // 'HISU' — bumped once on purpose:
// changing it forces every device to rescan the ring at the next boot. Needed because the fix
// below lives in recover(), and recover() was only ever reached when the magic did NOT match —
// so a head already parked next to a bad record would have stayed parked forever.

HistStore& HistStore::getInstance() {
    static HistStore instance;
    return instance;
}

bool HistStore::init() {
    if (part_) return true;
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!p) { ESP_LOGE(TAG, "storage partition not found"); return false; }
    part_ = p;
    cap_ = p->size / REC;
    // Only rescan when the RTC head/count are not ours (power loss). A reflash or esp_restart
    // keeps them, so on USB we no longer rescan 2 MB every measure tick and, crucially, the ring
    // keeps growing instead of resetting head to 0 each cycle.
    bool recovered = false;
    // The cached head is trusted only if the record behind it actually looks like the newest one.
    // A head can end up parked next to a record dated in the future — which is how the entire log
    // went invisible while every byte of it was still there — and nothing would re-examine it,
    // because rescanning was gated purely on the magic. One extra read per boot buys self-healing.
    // A head or count outside the ring was not written by this code (append() keeps head below cap_);
    // as in EvtLog::init, it is lost state to rescan, not a position to write past the partition at.
    bool head_sane = (rtc_hist_head < cap_ && rtc_hist_count <= cap_);
    if (rtc_hist_magic == HIST_MAGIC && head_sane && rtc_hist_count > 0) {
        HistRec last;
        const uint32_t prev = (rtc_hist_head + cap_ - 1) % cap_;
        const time_t nowt = time(NULL);
        if (esp_partition_read(p, (size_t)prev * REC, &last, REC) == ESP_OK) {
            const bool blank  = !(last.t > T_MIN && last.t < T_MAX);
            const bool future = (nowt > (time_t)T_MIN) && (last.t > (uint32_t)nowt + 86400u);
            if (blank || future) {
                head_sane = false;
                ESP_LOGW(TAG, "cached head points at a %s record — rescanning",
                         future ? "future-dated" : "blank");
            }
        }
    }
    if (rtc_hist_magic != HIST_MAGIC || !head_sane) {
        recover(); rtc_hist_magic = HIST_MAGIC; recovered = true;
    }
    ESP_LOGI(TAG, "History log: %u slots (%u KB), head=%u count=%u%s",
             (unsigned)cap_, (unsigned)(p->size / 1024),
             (unsigned)rtc_hist_head, (unsigned)rtc_hist_count, recovered ? " (recovered)" : "");
    return true;
}

// Head = slot after the newest timestamp; count = valid (non-empty) records.
void HistStore::recover() {
    const esp_partition_t* p = (const esp_partition_t*)part_;
    uint32_t bestIdx = 0, cnt = 0; uint32_t bestT = 0;
    HistRec r;
    // A record cannot be from the future. The newest timestamp decides where the head goes, so a
    // single record dated ahead of now captures it permanently: everything written afterwards
    // looks older, the head stays parked next to the bad record, and reading "the last N records"
    // returns the empty slots in front of it. The log is intact the whole time and appears lost.
    //
    // Seen for real: a wrap test wrote records stamped in 2030, and the graph went blank for good.
    // The same happens from a supply dip mid-write or a clock that jumps, which over years of
    // running is not a hypothetical.
    const time_t nowt = time(NULL);
    const uint32_t ceiling = (nowt > (time_t)T_MIN) ? (uint32_t)nowt + 86400u : T_MAX;
    for (uint32_t i = 0; i < cap_; i++) {
        if (esp_partition_read(p, i * REC, &r, REC) != ESP_OK) continue;
        if (r.t > T_MIN && r.t < ceiling) {
            cnt++;
            if (r.t >= bestT) { bestT = r.t; bestIdx = i; }
        }
    }
    if (cnt == 0) { rtc_hist_head = 0; rtc_hist_count = 0; }
    else { rtc_hist_head = (bestIdx + 1) % cap_; rtc_hist_count = cnt > cap_ ? cap_ : cnt; }
}

bool HistStore::append(const HistRec &rec) {
    if (!part_) return false;
    const esp_partition_t* p = (const esp_partition_t*)part_;
    uint32_t slot = rtc_hist_head;
    // Erase the sector before writing its first record (overwrites oldest data).
    if (slot % SECTOR_RECS == 0) {
        if (esp_partition_erase_range(p, slot * REC, SECTOR) != ESP_OK) {
            ESP_LOGW(TAG, "erase failed at slot %u", (unsigned)slot);
            return false;
        }
        if (rtc_hist_count > cap_ - SECTOR_RECS) rtc_hist_count = cap_ - SECTOR_RECS;
    }
    if (esp_partition_write(p, slot * REC, &rec, REC) != ESP_OK) {
        ESP_LOGW(TAG, "write failed at slot %u", (unsigned)slot);
        return false;
    }
    rtc_hist_head = (slot + 1) % cap_;
    if (rtc_hist_count < cap_) rtc_hist_count++;
    return true;
}

uint32_t HistStore::count() const { return rtc_hist_count; }

void HistStore::debugSetHead(uint32_t slot, uint32_t count) {
    rtc_hist_head = slot % (cap_ ? cap_ : 1);
    rtc_hist_count = count;
    rtc_hist_magic = HIST_MAGIC;
}
uint32_t HistStore::debugHead() const { return rtc_hist_head; }

void HistStore::eraseAll() {
    const esp_partition_t *p = (const esp_partition_t*)part_;
    if (!p) return;
    const uint32_t CHUNK = 64 * 1024;             // 16 sectors at a time, then let others run
    for (uint32_t off = 0; off < p->size; off += CHUNK) {
        const uint32_t len = (p->size - off < CHUNK) ? (p->size - off) : CHUNK;
        if (esp_partition_erase_range(p, off, len) != ESP_OK) {
            ESP_LOGE(TAG, "history erase failed at offset %u", (unsigned)off);
            break;
        }
        // Two megabytes of erasing can outlast the task watchdog on a slow flash chip, and the
        // factory reset now runs this together with the flight recorder and the sensor's own reset.
        // Fed only from a task the watchdog knows: feeding from any other one logs an error.
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
        vTaskDelay(1);
    }
    rtc_hist_head = 0; rtc_hist_count = 0; rtc_hist_magic = HIST_MAGIC;
    ESP_LOGW(TAG, "history erased (%u KB)", (unsigned)(p->size / 1024));
}

void HistStore::query(uint32_t from, uint32_t to, uint32_t max_out, RecCb cb, void *ctx,
                      RecKeep keep) {
    if (!part_) return;
    const esp_partition_t* p = (const esp_partition_t*)part_;
    // Scan only the slots that actually hold records, not the whole partition. Walking all
    // cap_ (65536) slots meant ~131k 32-byte flash reads per query in a tight loop: it pushed
    // the measure tick from ~5.7 s to 11.4 s of awake time (the tick that dominates the power
    // budget, 4.13) and starved the IDLE task long enough to trip the task watchdog every
    // measure tick. With a young log this is a ~300x saving; when the ring is full it is a
    // no-op. The oldest record sits `count` slots behind the write head.
    HistRec r;
    const uint32_t n = (rtc_hist_count > cap_) ? cap_ : rtc_hist_count;
    if (n == 0 || max_out == 0) return;

    // BACKWARDS FIRST, and this is the whole point. Records sit in the ring in time order, so the
    // answer to "the last day" or "the last 48 readings" is always at the END — yet both passes
    // used to start at the OLDEST record and walk the entire log forward, discarding almost all of
    // it one 32-byte flash read at a time. Nothing stopped early.
    //
    // The cost of that grows with the log, which is why it went unnoticed: the callers run on every
    // wake (the screen's graph, the clock-sanity check) and on every measurement (the barometer),
    // so the device gets more expensive every day it runs. On a five-day-old log the scans cost
    // some 50-140 uA, which hides inside the measurement noise; at a month they are 300-850, and by
    // the time the ring is full they exceed the entire rest of the budget. The device was measured
    // at 994 uA on day five — the cheapest it will ever be.
    //
    // So: walk back from the newest record and stop as soon as the window is behind us. The graph
    // then reads about 48 slots instead of twice the whole log.
    uint32_t back = 0;          // how many slots back from the head the window reaches
    uint32_t match = 0;
    for (uint32_t k = 1; k <= n; k++) {
        const uint32_t i = (rtc_hist_head + cap_ - k) % cap_;      // newest -> oldest
        if (esp_partition_read(p, i * REC, &r, REC) != ESP_OK) { back = k; continue; }
        if (r.t >= T_MIN && r.t <= T_MAX) {
            // Past the start of the window: everything older is older still, so stop.
            if (from && r.t < from) break;
            if (!(to && r.t > to) && (!keep || keep(r))) {
                match++;
                // Unbounded request: the callers that make one (the graph's positional fallback,
                // the power estimate) want the most RECENT max_out records, so stop once we have
                // them rather than reading back to the beginning of time.
                if (!from && match >= max_out) { back = k; break; }
            }
        }
        back = k;
    }
    if (match == 0) return;

    const uint32_t stride = (match + max_out - 1) / max_out;       // decimate if still too many
    // Second pass over the SAME bounded range, forwards, so the callback still sees oldest->newest.
    uint32_t idx = 0;
    for (uint32_t k = back; k >= 1; k--) {
        const uint32_t i = (rtc_hist_head + cap_ - k) % cap_;
        if (esp_partition_read(p, i * REC, &r, REC) != ESP_OK) continue;
        if (r.t < T_MIN || r.t > T_MAX) continue;
        if (from && r.t < from) continue;
        if (to && r.t > to) continue;
        if (keep && !keep(r)) continue;
        if ((idx % stride) == 0) cb(r, ctx);
        idx++;
    }
}

// The newest timestamp in the log, without reading the log. The clock-sanity check wanted exactly
// this and asked for it with an unbounded query() — two full walks of every record ever written,
// on every single wake, to find one number that is always in the slot just behind the head.
uint32_t HistStore::newestTimestamp() {
    if (!part_ || rtc_hist_count == 0) return 0;
    const esp_partition_t* p = (const esp_partition_t*)part_;
    const uint32_t n = (rtc_hist_count > cap_) ? cap_ : rtc_hist_count;
    HistRec r;
    // A handful of slots back at most: a torn write or a bad read should not send this to the
    // beginning of the ring, but neither should one make it give up.
    for (uint32_t k = 1; k <= n && k <= 8; k++) {
        const uint32_t i = (rtc_hist_head + cap_ - k) % cap_;
        if (esp_partition_read(p, i * REC, &r, REC) != ESP_OK) continue;
        if (r.t >= T_MIN && r.t <= T_MAX) return r.t;
    }
    return 0;
}
