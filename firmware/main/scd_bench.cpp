// ============================================================================
// ISOLATED CO2 BENCH — does the SCD41 measure at all?
//
// Nothing else runs: no gas sensor, no fuel gauge, no radio, no screen, no schedule. Just the one
// sensor and the one question, so there is only one thing that can be at fault.
//
// It prints every step, waits, and repeats — and it stays AWAKE, so the USB port exists the whole
// time and the log can actually be read. That alone is why this exists: in the real cycle the
// interesting moments happen in the two seconds after boot, which is exactly when the port does
// not yet exist.
// ============================================================================
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "scd_bench.h"
#include "i2c_manager.h"
#include "scd41.h"

static const char *TAG = "SCD_BENCH";

void scd_bench_run(void)
{
    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, "CO2 sensor bench — nothing else is running");

    I2CManager::getInstance().init();
    i2c_master_bus_handle_t bus = I2CManager::getInstance().getBusHandle();
    if (!bus) { ESP_LOGE(TAG, "no I2C bus at all"); vTaskDelay(portMAX_DELAY); }

    SCD41 scd;
    ESP_LOGW(TAG, "init: %d", scd.init(bus));

    // Whatever state the sensor was left in, this is the way out of it: wake it, stop anything
    // it might be doing, and only then ask for a measurement.
    ESP_LOGW(TAG, "wakeUp: %d", scd.wakeUp());
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGW(TAG, "stopPeriodic: %d", scd.stopPeriodicMeasurement());

    // Who is actually answering at 0x62, and does it think it is healthy? The self test exercises
    // the measurement path itself and reports a malfunction if there is one — the manufacturer's
    // verdict, which beats anything inferred from the outside.
    uint64_t sn = 0;
    if (scd.getSerial(sn)) ESP_LOGW(TAG, "serial number: %012llX", (unsigned long long)sn);
    else                   ESP_LOGE(TAG, "serial number: could not be read");
    ESP_LOGW(TAG, "running the built-in self test (takes 10 s) ...");
    uint16_t st = 0xFFFF;
    if (scd.selfTest(st)) ESP_LOGW(TAG, "SELF TEST: 0x%04X (%s)", st,
                                   st == 0 ? "sensor reports itself HEALTHY"
                                           : "sensor reports a MALFUNCTION");
    else                  ESP_LOGE(TAG, "SELF TEST: no answer");

    // The sensor returns good temperature and humidity but zero CO2, which means it is converting
    // normally with broken calibration. Only two things ever write to that calibration: a forced
    // recalibration and the temperature offset. Put it back to how it left the factory.
    ESP_LOGW(TAG, "factoryReset: %d", scd.factoryReset());
    ESP_LOGW(TAG, "reinit: %d", scd.reinit());
    scd.stopPeriodicMeasurement();

    uint16_t serial_hi = 0;
    (void)serial_hi;

    for (int round = 1;; round++) {
        ESP_LOGW(TAG, "---- round %d ----", round);

        const bool shot = scd.measureSingleShot();
        const int64_t t0 = esp_timer_get_time();
        ESP_LOGW(TAG, "measureSingleShot: %d", shot);

        // Poll for ten seconds, reporting what the sensor says as it goes. A single shot is
        // specified at about five seconds; anything much earlier is a stale flag from before.
        bool got = false;
        for (int ms = 0; ms < 10000 && !got; ms += 250) {
            vTaskDelay(pdMS_TO_TICKS(250));
            bool rdy = false;
            const bool ok = scd.isDataReady(rdy);
            if (ms % 1000 == 0) ESP_LOGW(TAG, "  t=%4d ms  isDataReady ok=%d ready=%d", ms, ok, rdy);
            if (ok && rdy) {
                uint16_t c = 0; float t = 0, h = 0;
                const bool rd = scd.readMeasurement(c, t, h);
                ESP_LOGW(TAG, "  READ at %lld ms: ok=%d  CO2=%u ppm  T=%.2f C  H=%.2f %%",
                         (esp_timer_get_time() - t0) / 1000, rd, c, t, h);
                got = true;
            }
        }
        if (!got) ESP_LOGE(TAG, "  nothing within 10 s — the sensor never finished a conversion");

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
