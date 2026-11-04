#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef enum { ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_ALL, ESP_SLEEP_WAKEUP_EXT0,
               ESP_SLEEP_WAKEUP_EXT1, ESP_SLEEP_WAKEUP_TIMER, ESP_SLEEP_WAKEUP_TOUCHPAD,
               ESP_SLEEP_WAKEUP_ULP, ESP_SLEEP_WAKEUP_GPIO, ESP_SLEEP_WAKEUP_UART,
               ESP_SLEEP_WAKEUP_WIFI, ESP_SLEEP_WAKEUP_COCPU, ESP_SLEEP_WAKEUP_BT } esp_sleep_source_t;
typedef esp_sleep_source_t esp_sleep_wakeup_cause_t;
#define ESP_GPIO_WAKEUP_GPIO_LOW  0
#define ESP_GPIO_WAKEUP_GPIO_HIGH 1
// Всё, что касается сна, ЗАПИСЫВАЕТСЯ, а не исполняется: тест спрашивает, какие источники
// вооружены и на какой срок поставлен таймер. Именно эти два вопроса стоили отказа 0.9.43, когда
// менеджер питания оставлял свой таймер и прибор просыпался сам.
esp_sleep_source_t esp_sleep_get_wakeup_cause(void);
uint64_t  esp_sleep_get_gpio_wakeup_status(void);
esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_source_t s);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us);
esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t mask, int level);
esp_err_t esp_sleep_enable_gpio_wakeup(void);
esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t mask, int mode);
[[noreturn]] void esp_deep_sleep_start(void);          // не возвращается: бросает HostSleep
esp_err_t esp_light_sleep_start(void);

#ifdef __cplusplus
}
#endif
