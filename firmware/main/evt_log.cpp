#include "evt_log.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"

static const char *TAG = "EVT";
static constexpr uint32_t REC = sizeof(EvtRec);          // 32
static constexpr uint32_t SECTOR = 4096;
static constexpr uint32_t SECTOR_RECS = SECTOR / REC;    // 128
static constexpr uint32_t SEQ_EMPTY = 0xFFFFFFFFu;       // what erased flash reads as

// Head, count and the sequence counter must survive deep sleep AND esp_restart, and must be
// recognisable as garbage after a real power loss — the same requirement, and the same answer, as
// the history ring (§3: RTC_DATA_ATTR is reset by esp_restart and would silently restart the log
// on every USB cycle).
RTC_NOINIT_ATTR static uint32_t rtc_evt_head;
RTC_NOINIT_ATTR static uint32_t rtc_evt_count;
RTC_NOINIT_ATTR static uint32_t rtc_evt_seq;
RTC_NOINIT_ATTR static uint32_t rtc_evt_magic;
RTC_NOINIT_ATTR static uint32_t rtc_evt_sent;    // highest seq handed to the hub
static constexpr uint32_t EVT_MAGIC = 0x45565431;        // 'EVT1'

// What this wake has established about itself so far. Plain statics: deep sleep destroys them,
// which is exactly the lifetime wanted — every wake starts with an empty sheet.
static EvtRec  cur_;
static int64_t base_us_ = 0;
static bool    have_base_ = false;

EvtLog& EvtLog::getInstance() { static EvtLog i; return i; }

// One ring, two tasks. Until 2026-09-18 only the cycle task ever wrote here and no lock was needed;
// the security log added seven writes from the httpd task (a sign-in, a wrong password, a password
// change, a factory request, a revert), and those arrive while the cycle task is writing its own
// records in the very same window. Without this both tasks read the same head, write the same slot
// and advance the counters twice: one record silently gone, the sequence numbers skipping, and —
// every 128 records — one task erasing a sector while the other writes into it. A lossy log is
// worse than no log, because it is the log a dispute would be settled with.
//
// A mutex rather than a spinlock: these calls erase and program flash, which blocks, and a
// spinlock would hold interrupts off across it. Recursive, because event() takes it and then calls
// append(). Bounded rather than portMAX_DELAY, and a record is DROPPED rather than waited for:
// nothing about logging may hold up a web handler or the cycle.
static SemaphoreHandle_t evt_lock()
{
    static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
    return m;
}
namespace {
struct EvtGuard {
    SemaphoreHandle_t m;
    bool held;
    explicit EvtGuard(uint32_t ms = 1000)
        : m(evt_lock()),
          held(m && xSemaphoreTakeRecursive(m, pdMS_TO_TICKS(ms)) == pdTRUE) {}
    ~EvtGuard() { if (held) xSemaphoreGiveRecursive(m); }
};
}   // namespace

static uint16_t awake_ms_now()
{
    // esp_timer restarts at zero out of deep sleep, so its raw value is the time since this wake
    // — but that is only true on the sleeping path, and this file should not care which path it is
    // on. Measuring from our own first init() is correct in both cases.
    const int64_t now = esp_timer_get_time();
    const int64_t ms = have_base_ ? (now - base_us_) / 1000 : 0;
    return (ms < 0) ? 0 : (ms > 65535 ? 65535 : (uint16_t)ms);
}

bool EvtLog::init()
{
    // Both tasks reach this: whichever writes first does the discovery, and the other must not run
    // it at the same time or start from a half-set head.
    EvtGuard lock;
    if (!lock.held) return part_ != nullptr;
    if (!have_base_) { base_us_ = esp_timer_get_time(); have_base_ = true; memset(&cur_, 0, sizeof(cur_)); }
    if (part_) return true;
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "events");
    if (!p) { ESP_LOGE(TAG, "events partition not found"); return false; }
    part_ = p;
    cap_  = p->size / REC;

    if (rtc_evt_magic != EVT_MAGIC || rtc_evt_head >= cap_) {
        restoreSeq();
        rtc_evt_magic = EVT_MAGIC;
        ESP_LOGI(TAG, "flight recorder recovered: %u slots, head=%u count=%u next seq=%u",
                 (unsigned)cap_, (unsigned)rtc_evt_head, (unsigned)rtc_evt_count,
                 (unsigned)rtc_evt_seq);
    }
    // The high-water mark gets the same suspicion as the head. It is RTC_NOINIT and it is only
    // ever cleared by restoreSeq(), so a stray byte that leaves the magic intact would park it
    // above every sequence number the ring will ever hold — and then buildChunk() finds nothing
    // new, for good, and says nothing about it. That is the exact shape this project has been
    // caught by five times (a stale RTC field read as a decision), and it costs one comparison.
    if (rtc_evt_sent >= rtc_evt_seq) {
        if (rtc_evt_sent) ESP_LOGW(TAG, "sent mark %u is ahead of seq %u — resending from the top",
                                   (unsigned)rtc_evt_sent, (unsigned)rtc_evt_seq);
        rtc_evt_sent = 0;
    }
    return true;
}

// Only after a real power loss. Ordering is by sequence number, so this looks for the highest one
// rather than the newest timestamp — which is the point: a record written before the clock was set
// carries t = 0, and a clock that jumps cannot move the head.
void EvtLog::restoreSeq()
{
    const esp_partition_t *p = (const esp_partition_t *)part_;
    static EvtRec blk[32];
    uint32_t best_seq = 0, best_slot = 0, cnt = 0;
    bool any = false;
    for (uint32_t s = 0; s < cap_; s += 32) {
        const uint32_t n = (cap_ - s < 32) ? (cap_ - s) : 32;
        if (esp_partition_read(p, (size_t)s * REC, blk, n * REC) != ESP_OK) break;
        for (uint32_t i = 0; i < n; i++) {
            if (blk[i].seq == SEQ_EMPTY) continue;
            cnt++;
            if (!any || blk[i].seq > best_seq) { any = true; best_seq = blk[i].seq; best_slot = s + i; }
        }
        if ((s & 0x3FF) == 0) vTaskDelay(0);      // 768 KB in one go would hog the CPU
    }
    rtc_evt_count = cnt;
    rtc_evt_head  = any ? ((best_slot + 1) % cap_) : 0;
    rtc_evt_seq   = any ? (best_seq + 1) : 1;
    // Deliberately zero, not best_seq: after a power loss we do not know what the hub already has,
    // and re-sending is free where a gap is not. The reader drops duplicates by sequence number.
    rtc_evt_sent  = 0;
}

bool EvtLog::append(EvtRec &r)
{
    if (!part_ || !cap_) return false;
    EvtGuard lock;
    if (!lock.held) { ESP_LOGW(TAG, "ring busy — dropping one record"); return false; }
    const esp_partition_t *p = (const esp_partition_t *)part_;
    const uint32_t slot = rtc_evt_head;
    if (slot % SECTOR_RECS == 0) {
        if (esp_partition_erase_range(p, (size_t)slot * REC, SECTOR) != ESP_OK) {
            ESP_LOGW(TAG, "erase failed at slot %u", (unsigned)slot);
            return false;
        }
        if (rtc_evt_count > cap_ - SECTOR_RECS) rtc_evt_count = cap_ - SECTOR_RECS;
    }
    r.seq = rtc_evt_seq;
    if (esp_partition_write(p, (size_t)slot * REC, &r, REC) != ESP_OK) {
        ESP_LOGW(TAG, "write failed at slot %u", (unsigned)slot);
        return false;
    }
    rtc_evt_seq++;
    rtc_evt_head = (slot + 1) % cap_;
    if (rtc_evt_count < cap_) rtc_evt_count++;
    return true;
}

// An event record carries the whole context the wake has gathered, not just its own news. That
// costs nothing (the fields are already sitting in cur_) and means a single record is enough to
// reconstruct the moment without hunting for the wake summary it belongs to.
void EvtLog::event(EvtKind kind, uint32_t detail)
{
    if (!init()) return;
    // Held from here so the copy of cur_ below cannot be taken while the cycle task is updating it.
    EvtGuard lock;
    if (!lock.held) { ESP_LOGW(TAG, "ring busy — dropping one event"); return; }
    EvtRec r = cur_;
    r.kind     = (uint8_t)kind;
    r.detail   = detail;
    r.awake_ms = awake_ms_now();
    const time_t now = time(NULL);
    r.t        = (now > 1600000000) ? (uint32_t)now : 0;
    r.heap_kb  = (uint16_t)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024);
    append(r);
}

int64_t EvtLog::baseUs() const { return have_base_ ? base_us_ : 0; }

void EvtLog::wake(uint16_t awake_ms)
{
    if (!init()) return;
    EvtRec r = cur_;
    r.kind     = (uint8_t)EVT_WAKE;
    r.awake_ms = awake_ms ? awake_ms : awake_ms_now();
    const time_t now = time(NULL);
    r.t        = (now > 1600000000) ? (uint32_t)now : 0;
    r.heap_kb  = (uint16_t)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024);
    append(r);
}

void EvtLog::setWake(EvtWake w, bool first)
{
    cur_.wake = (uint8_t)w;
    if (first) cur_.flags |= EF_FIRST;
}
void EvtLog::setFlags(uint8_t set, uint8_t clear)
{
    cur_.flags = (uint8_t)((cur_.flags & ~clear) | set);
}
void EvtLog::setSensor(uint8_t bit, bool ok)
{
    if (ok) cur_.sens |= bit; else cur_.sens = (uint8_t)(cur_.sens & ~bit);
}
void EvtLog::setGas(uint8_t accuracy, uint8_t pollution_class, bool outputs)
{
    cur_.gas = (uint8_t)((accuracy & 3) | ((pollution_class & 7) << 2) | (outputs ? 0x20 : 0));
}
void EvtLog::setDraw(uint16_t ms, bool full)
{
    cur_.draw_ms = ms;
    cur_.flags |= EF_DREW;
    if (full) cur_.flags |= EF_FULL; else cur_.flags = (uint8_t)(cur_.flags & ~EF_FULL);
}
void EvtLog::setRadio(uint16_t ms, int8_t rssi) { cur_.radio_ms = ms; cur_.rssi = rssi; cur_.flags |= EF_RADIO; }
void EvtLog::setBattery(uint16_t mv, uint16_t soc_d) { cur_.vcell_mv = mv; cur_.soc_d = soc_d; }
void EvtLog::setTemp(float c)
{
    if (c > -60.0f && c < 90.0f) cur_.temp_c = (int8_t)(c < 0 ? c - 0.5f : c + 0.5f);
}
void EvtLog::setBootIncomplete(uint8_t n) { cur_.boot_inc = n; }
void EvtLog::setDetail(uint32_t d) { cur_.detail = d; }

uint32_t EvtLog::lastSent() const { return rtc_evt_sent; }
void EvtLog::markSent(uint32_t seq) { if (seq > rtc_evt_sent) rtc_evt_sent = seq; }

uint32_t EvtLog::buildChunk(uint32_t since, uint32_t max_records, std::string &out)
{
    out.clear();
    if (!init() || !cap_) return since;
    struct __attribute__((packed)) ChunkHdr {
        char     magic[4];      // "SMEC" — one chunk of Smart Meteo events
        uint16_t ver;
        uint16_t rec;
        uint32_t count;
    } h;
    memcpy(h.magic, "SMEC", 4);
    h.ver = 1; h.rec = (uint16_t)REC; h.count = 0;
    out.append((const char *)&h, sizeof(h));

    // Where the new records start is arithmetic, not a search: seq and slot advance together, one
    // each, so the record carrying sequence S sits exactly (nextSeq - S) slots behind the head.
    // Walking the whole ring instead would read 768 KB of flash to find the twenty records a radio
    // window actually produced — inside the one window this file calls the most expensive thing the
    // device does. It also answers "nothing new" without touching flash at all.
    //
    // Counting backwards from the head is what makes this safe across a wrap: append() erases the
    // sector AHEAD of the head and clamps the count to match, so the erased gap is never in the
    // span this walks, and head - count lands on the oldest surviving record either way.
    const uint32_t have  = (rtc_evt_count > cap_) ? cap_ : rtc_evt_count;
    uint32_t newer = (rtc_evt_seq > since + 1) ? (rtc_evt_seq - 1 - since) : 0;
    if (!newer || !have) { out.clear(); return since; }
    if (newer > have) newer = have;          // the rest was overwritten; send what survived
    const uint32_t start = (rtc_evt_head + cap_ - newer) % cap_;

    uint32_t highest = since, n = 0;
    // Oldest first, so a chunk is already in order and the receiver never sorts. Oldest first also
    // means a capped chunk leaves the NEWEST records behind rather than punching a hole: the next
    // window continues where this one stopped.
    for (uint32_t i = 0; i < newer && n < max_records; i++) {
        EvtRec r;
        if (!read((start + i) % cap_, r)) break;
        // Kept as a guard, not as the search: if the RTC bookkeeping is off, this drops the bad
        // records instead of shipping them.
        if (r.seq == SEQ_EMPTY || r.seq <= since) continue;
        out.append((const char *)&r, REC);
        if (r.seq > highest) highest = r.seq;
        n++;
    }
    if (!n) { out.clear(); return since; }
    h.count = n;
    memcpy(&out[0], &h, sizeof(h));      // rewrite the header now that the count is known
    return highest;
}

// Summarised from the ring rather than from counters kept alongside it, and the choice is
// deliberate. Counters would be O(1), but they would be ten more RTC_NOINIT fields — ten more
// things that must be named in the build-stamp reset block, and a field forgotten there is the
// single shape that has bitten this project four separate times (4.36, 4.53, 4.55). The ring
// already holds the timestamps and the flags; it needs no help remembering them.
//
// NB for anyone tempted to "fix" this the way buildChunk() was fixed: that walk read the WHOLE
// ring to find twenty records and could be replaced by arithmetic, because sequence numbers map
// to slots. This walk reads only as far back as it needs and genuinely wants every record it
// touches — about 1500 for a normal day, once per radio window. They are not the same walk.
EvtLog::Health EvtLog::health() const
{
    Health h = {};
    if (!part_ || !cap_ || !rtc_evt_count) return h;
    const time_t now = time(NULL);
    if (now < 1600000000) return h;              // no clock, so no window to speak of
    const uint32_t cutoff = (uint32_t)now - 86400;

    uint32_t newest_t = 0, oldest_t = 0, prev_edges = 0;
    bool have_prev = false;
    const uint32_t have = (rtc_evt_count > cap_) ? cap_ : rtc_evt_count;
    for (uint32_t i = 1; i <= have; i++) {
        EvtRec r;
        if (!read((rtc_evt_head + cap_ - i) % cap_, r)) break;
        if (r.seq == SEQ_EMPTY) break;
        // A record written before the clock was set carries t = 0. Stop rather than skip: older
        // records cannot be newer, so everything beyond this point is outside the window too.
        if (!r.t || r.t < cutoff) break;
        // EVERYTHING below is counted from wake records only, and that is not tidiness. `detail`
        // is kind-specific: on a wake it is the rejected-edge counter, on a boot record it is the
        // reset reason packed with the image fingerprint in the top half. Reading it off every
        // record put a fingerprint-sized number into the subtraction and reported 3323200873
        // rejected edges per day on the first real reading. Flags have the same shape of problem
        // from the other side: event() inherits them from the wake in progress, so an event on a
        // drawing wake counted as a second refresh of the same frame.
        //
        // One wake, one row. That is also what the numbers are supposed to mean.
        if (r.kind != EVT_WAKE) continue;
        if (!newest_t) newest_t = r.t;
        oldest_t = r.t;
        // Summed as increments rather than taken as the difference of the two ends, because the
        // counter is cleared by a firmware change and a window that spans one then has a smaller
        // number at the newer end. Checked against a real 20.5 h dump: the end-to-end difference
        // said 1 where the increments said 1236, and the window did span a flash. We walk from
        // newest to oldest, so a legitimate increment is prev >= current.
        if (have_prev && prev_edges >= r.detail) h.edges += prev_edges - r.detail;
        prev_edges = r.detail;
        have_prev = true;
        h.wakes++;
        h.awake_ms += r.awake_ms;                // saturates at 65535 per record; see below
        if (r.flags & EF_DREW) h.draws++;
        if (r.flags & EF_FULL) h.fulls++;
    }
    if (newest_t <= oldest_t) return h;
    h.span_s = newest_t - oldest_t;
    // An hour is the shortest span worth extrapolating from: below it a single console session or
    // one cable event dominates, and a health figure that swings by 10x on its own is worse than
    // no figure at all. The consumer decides what to show while this is false.
    h.valid  = (h.span_s >= 3600 && h.wakes >= 10);
    return h;
}

uint32_t EvtLog::count()   const { return rtc_evt_count; }
uint32_t EvtLog::head()    const { return rtc_evt_head; }
uint32_t EvtLog::nextSeq() const { return rtc_evt_seq; }

bool EvtLog::read(uint32_t slot, EvtRec &out) const
{
    if (!part_ || slot >= cap_) return false;
    return esp_partition_read((const esp_partition_t *)part_, (size_t)slot * REC, &out, REC) == ESP_OK;
}

void EvtLog::eraseAll()
{
    const esp_partition_t *p = (const esp_partition_t *)part_;
    if (!p) return;
    // Longer than a record's wait: this is the factory reset, and a log record arriving in the
    // middle of it must not land in a sector that is about to be erased anyway.
    EvtGuard lock(5000);
    const uint32_t CHUNK = 64 * 1024;             // in pieces, yielding between them: one blocking
    for (uint32_t off = 0; off < p->size; off += CHUNK) {   // erase of the whole partition starves
        const uint32_t len = (p->size - off < CHUNK) ? (p->size - off) : CHUNK;   // the idle task
        if (esp_partition_erase_range(p, off, len) != ESP_OK) {                   // and trips the
            ESP_LOGE(TAG, "event log erase failed at offset %u", (unsigned)off);  // watchdog
            break;
        }
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();   // see HistStore::eraseAll
        vTaskDelay(1);
    }
    rtc_evt_head = 0; rtc_evt_count = 0; rtc_evt_seq = 1; rtc_evt_magic = EVT_MAGIC;
    // Nothing in an empty log has been handed to the hub. Left at the old mark, the next boot's
    // init() happens to reset it (sent >= seq), but only because the old numbers were larger.
    rtc_evt_sent = 0;
    ESP_LOGW(TAG, "flight recorder erased");
}
