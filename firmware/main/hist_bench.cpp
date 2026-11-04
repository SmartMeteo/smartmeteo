// ============================================================================
// ISOLATED HISTORY-RING BENCH — what happens when the ring wraps?
//
// At 288 records a day the ring turns over after about 228 days, so the code that runs at the
// boundary — erase the sector, clamp the count, wrap the head to zero — has never executed on any
// device. A fault there would corrupt the whole log, once, silently, most of a year in.
//
// Waiting is not a test. This moves the head to the last slot instead and writes across the seam,
// then reads back to see whether the records survived, the head came round to zero and the count
// behaved. Everything is printed; the device stays awake so the log can be read.
// ============================================================================
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hist_bench.h"
#include "hist_store.h"

static const char *TAG = "HIST_BENCH";

struct Seen { uint32_t n; uint32_t first_t; uint32_t last_t; };

static void countCb(const HistRec &r, void *ctx) {
    Seen *s = (Seen *)ctx;
    if (!s->n) s->first_t = r.t;
    s->last_t = r.t;
    s->n++;
}

void hist_bench_run(void)
{
    HistStore &H = HistStore::getInstance();
    ESP_LOGW(TAG, "==================================================");
    if (!H.init()) { ESP_LOGE(TAG, "no history partition"); vTaskDelay(portMAX_DELAY); }

    const uint32_t cap = H.capacity();
    ESP_LOGW(TAG, "capacity %u slots (%u days at 288/day) | count now %u | head %u",
             (unsigned)cap, (unsigned)(cap / 288), (unsigned)H.count(), (unsigned)H.debugHead());

    // Park the head two slots short of the end, with the ring declared almost full — the state it
    // would reach after months of ordinary use.
    const uint32_t start = cap - 2;
    H.debugSetHead(start, cap - 2);
    ESP_LOGW(TAG, "head moved to %u, count set to %u — writing across the seam",
             (unsigned)start, (unsigned)(cap - 2));

    // Six records with recognisable timestamps: two before the seam, four after.
    const uint32_t T0 = 1900000000;
    for (int i = 0; i < 6; i++) {
        HistRec r = {};
        r.t = T0 + i;
        r.co2 = (uint16_t)(1000 + i);
        r.temp_d = 200; r.hum = 50; r.soc_d = 5000; r.vcell_mv = 3900;
        const bool ok = H.append(r);
        ESP_LOGW(TAG, "  append #%d (co2=%u): %s | head now %u | count %u",
                 i, (unsigned)r.co2, ok ? "ok" : "FAILED",
                 (unsigned)H.debugHead(), (unsigned)H.count());
        if (!ok) ESP_LOGE(TAG, "  ^ this is the failure a real wrap would have hit");
    }

    if (H.debugHead() > 8)
        ESP_LOGE(TAG, "head did NOT wrap round to the start — it is %u", (unsigned)H.debugHead());
    else
        ESP_LOGW(TAG, "head wrapped to %u, as it should", (unsigned)H.debugHead());

    // Read the tail back. The four records written after the seam must be there, in order.
    Seen s = {0, 0, 0};
    H.query(T0, T0 + 100, 32, countCb, &s);
    ESP_LOGW(TAG, "read back %u of the 6 written | first t=%u last t=%u",
             (unsigned)s.n, (unsigned)s.first_t, (unsigned)s.last_t);
    if (s.n == 6 && s.first_t == T0 && s.last_t == T0 + 5)
        ESP_LOGW(TAG, "RESULT: the ring survives its wrap intact");
    else
        ESP_LOGE(TAG, "RESULT: records were LOST across the wrap — %u of 6 came back", (unsigned)s.n);

    ESP_LOGW(TAG, "the log has been disturbed by this test; erase it from Settings if you care");
    ESP_LOGW(TAG, "==================================================");
    while (true) vTaskDelay(pdMS_TO_TICKS(10000));
}
