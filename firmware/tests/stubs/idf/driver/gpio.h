#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
#define GPIO_NUM_0 0
#define GPIO_NUM_1 1
#define GPIO_NUM_2 2
#define GPIO_NUM_3 3
#define GPIO_NUM_4 4
#define GPIO_NUM_5 5
#define GPIO_NUM_6 6
#define GPIO_NUM_7 7
#define GPIO_NUM_8 8
#define GPIO_NUM_9 9
#define GPIO_NUM_10 10
#define GPIO_NUM_11 11
#define GPIO_NUM_12 12
#define GPIO_NUM_13 13
#define GPIO_NUM_14 14
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16
#define GPIO_NUM_17 17
#define GPIO_NUM_18 18
#define GPIO_NUM_19 19
#define GPIO_NUM_20 20
#define GPIO_NUM_21 21
#define GPIO_NUM_22 22
#define GPIO_NUM_23 23
#define GPIO_NUM_24 24
#define GPIO_NUM_25 25
#define GPIO_NUM_26 26
#define GPIO_NUM_27 27
#define GPIO_NUM_28 28
#define GPIO_NUM_29 29
#define GPIO_NUM_30 30
typedef enum { GPIO_MODE_DISABLE, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT, GPIO_MODE_OUTPUT_OD,
               GPIO_MODE_INPUT_OUTPUT_OD, GPIO_MODE_INPUT_OUTPUT } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE, GPIO_PULLDOWN_ENABLE } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE, GPIO_INTR_ANYEDGE,
               GPIO_INTR_LOW_LEVEL, GPIO_INTR_HIGH_LEVEL } gpio_int_type_t;
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; gpio_pullup_t pull_up_en;
                 gpio_pulldown_t pull_down_en; gpio_int_type_t intr_type; } gpio_config_t;
typedef void (*gpio_isr_t)(void *);

// Уровень задаёт тест: это вход кабеля (USB_DETECT) и кнопки, а обе линии решают, что прибор
// сделает на этом пробуждении.
int      gpio_get_level(gpio_num_t pin);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
esp_err_t gpio_config(const gpio_config_t *cfg);

// Остальное записывается для проверок и ничего не делает: тип прерывания кнопки сверяется тестом,
// потому что расхождение между тем, что ставит загрузка, и тем, что возвращает ожидание, уже
// стоило одного дефекта (журнал 2026-09-19).
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
esp_err_t gpio_set_pull_mode(gpio_num_t pin, int mode);
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t t);
esp_err_t gpio_intr_enable(gpio_num_t pin);
esp_err_t gpio_intr_disable(gpio_num_t pin);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t fn, void *arg);
esp_err_t gpio_isr_handler_remove(gpio_num_t pin);
esp_err_t gpio_hold_en(gpio_num_t pin);
esp_err_t gpio_hold_dis(gpio_num_t pin);
esp_err_t gpio_deep_sleep_hold_en(void);
esp_err_t gpio_deep_sleep_hold_dis(void);
esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t t);
esp_err_t gpio_wakeup_disable(gpio_num_t pin);
esp_err_t gpio_sleep_sel_dis(gpio_num_t pin);
esp_err_t gpio_sleep_sel_en(gpio_num_t pin);
esp_err_t gpio_reset_pin(gpio_num_t pin);
#define GPIO_PULLUP_ONLY   2
#define GPIO_FLOATING      3

#ifdef __cplusplus
}
#endif
