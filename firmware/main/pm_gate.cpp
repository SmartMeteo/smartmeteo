#include "pm_gate.h"
#include "epaper.h"
#include "i2c_manager.h"

#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_log.h"

static const char *TAG = "PM";

namespace PmGate {
namespace {

esp_pm_lock_handle_t s_block = nullptr;   // held = light sleep forbidden
bool       s_ready  = false;
int        s_depth  = 0;                  // open naps
gpio_num_t s_button = GPIO_NUM_NC;
void     (*s_on_press)() = nullptr;

int64_t  s_slept_us = 0;
uint32_t s_sleeps   = 0;

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
// Runs in the IDLE task, straight out of light sleep. Two adds and nothing else: the framework
// warns that whatever a callback spends here is taken off the very sleep it is reporting, and a
// blocking call would deadlock the idle task. `slept_us` is the actual duration, not the estimate
// the entry callback would have seen.
esp_err_t on_light_sleep_exit(int64_t slept_us, void *)
{
    s_slept_us += slept_us;
    s_sleeps++;
    return ESP_OK;
}
#endif

}  // namespace

int64_t  slept_us() { return s_slept_us; }
uint32_t sleeps()   { return s_sleeps; }

bool begin(gpio_num_t button, gpio_num_t led, void (*on_press)())
{
    if (s_ready) return true;
    s_button   = button;
    s_on_press = on_press;

    // The pads that must survive an isolated sleep. CONFIG_PM_SLP_DISABLE_GPIO is on, so every pin
    // is released for the duration unless it is excluded here. GPIO10 is the panel's power gate
    // with a 100k pull-up to 3.3 V, so a released pin reads as "panel off" — that combination is
    // why automatic light sleep was dropped from the product in 0.9.43, and it is handled the same
    // way Matter mode handles it rather than by a new mechanism.
    EPaper::keepPadsThroughLightSleep();
    I2CManager::keepPadsThroughLightSleep();
    if (led != GPIO_NUM_NC) gpio_sleep_sel_dis(led);

    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    esp_err_t e = esp_pm_configure(&pm);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "light sleep not configured (%s) — waits stay awake", esp_err_to_name(e));
        return false;
    }
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "cycle", &s_block) != ESP_OK || !s_block) {
        // Without the lock the cycle would sleep wherever it idles, which is exactly what was
        // removed in 0.9.43. Put the configuration back rather than leave that running.
        ESP_LOGE(TAG, "no light-sleep lock — reverting to no light sleep");
        pm.light_sleep_enable = false;
        esp_pm_configure(&pm);
        s_block = nullptr;
        return false;
    }
    esp_pm_lock_acquire(s_block);          // blocked by default; only a Nap lifts it
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    esp_pm_sleep_cbs_register_config_t cbs = {};
    cbs.exit_cb = on_light_sleep_exit;
    const esp_err_t ce = esp_pm_light_sleep_register_cbs(&cbs);
    if (ce != ESP_OK) ESP_LOGW(TAG, "sleep counter not registered (%s)", esp_err_to_name(ce));
#endif
    s_ready = true;
    ESP_LOGI(TAG, "light sleep armed, permitted only inside a wait");
    return true;
}

bool active() { return s_ready; }

Nap::Nap(uint32_t expected_ms)
{
    if (!s_ready || expected_ms < kMinNapMs) return;
    held_    = true;
    want_ms_ = expected_ms;
    slept0_  = s_slept_us;
    naps0_   = s_sleeps;
    if (s_depth++ == 0) {
        if (s_button != GPIO_NUM_NC) {
            // A light-sleep wake source can only be a LEVEL — the chip cannot see an edge while it
            // is asleep, and gpio_wakeup_enable() rejects an edge outright. But that same call sets
            // the pin's interrupt type (gpio.c), and this pin also carries the edge handler that
            // records presses. Left as it is, a held button would re-enter that handler for as long
            // as the finger is down, because a level interrupt is re-asserted the moment the driver
            // clears it. So the interrupt is switched off for the length of the wait, exactly as
            // matter_btn_isr does it for the same reason, and the press is picked up below instead.
            gpio_intr_disable(s_button);
            gpio_wakeup_enable(s_button, GPIO_INTR_LOW_LEVEL);
            esp_sleep_enable_gpio_wakeup();
        }
        esp_pm_lock_release(s_block);
    }
}

Nap::~Nap()
{
    if (!held_) return;
    if (--s_depth == 0) {
        esp_pm_lock_acquire(s_block);
        if (s_button != GPIO_NUM_NC) {
            gpio_wakeup_disable(s_button);
            // gpio_wakeup_disable() does not put the interrupt type back, so this has to.
            gpio_set_intr_type(s_button, GPIO_INTR_NEGEDGE);
            // A press made during the wait left no edge behind for the handler to catch: either it
            // happened while the chip was asleep, or while the interrupt was off. The line itself
            // is the evidence. Checked once before re-arming and once after, so a press landing in
            // the microseconds between the two is not lost either; the callback is idempotent.
            if (s_on_press && gpio_get_level(s_button) == 0) s_on_press();
            gpio_intr_enable(s_button);
            if (s_on_press && gpio_get_level(s_button) == 0) s_on_press();
        }
        // The GPIO wake source itself is left for arm_wake_pins() to clear: it begins every deep
        // sleep with esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL), which is precisely the
        // guard that the 0.9.43 failure lacked.
    }
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    // The only place the saving is visible at all. A wait that reports 0 ms slept means the gate
    // opened and nothing went to sleep behind it — which is a defect, not a detail.
    ESP_LOGI(TAG, "wait %u ms: slept %d ms over %u light sleeps",
             (unsigned)want_ms_, (int)((s_slept_us - slept0_) / 1000),
             (unsigned)(s_sleeps - naps0_));
#endif
}

}  // namespace PmGate
