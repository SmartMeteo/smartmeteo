#pragma once
#include <stdint.h>
typedef int gpio_num_t;
enum { GPIO_NUM_0 = 0, GPIO_NUM_1 = 1, GPIO_NUM_2 = 2, GPIO_NUM_3 = 3, GPIO_NUM_4 = 4, GPIO_NUM_5 = 5, GPIO_NUM_6 = 6, GPIO_NUM_7 = 7, GPIO_NUM_8 = 8, GPIO_NUM_9 = 9, GPIO_NUM_10 = 10, GPIO_NUM_11 = 11, GPIO_NUM_12 = 12, GPIO_NUM_13 = 13, GPIO_NUM_14 = 14, GPIO_NUM_15 = 15, GPIO_NUM_16 = 16, GPIO_NUM_17 = 17, GPIO_NUM_18 = 18, GPIO_NUM_19 = 19, GPIO_NUM_20 = 20, GPIO_NUM_21 = 21, GPIO_NUM_22 = 22, GPIO_NUM_23 = 23, GPIO_NUM_24 = 24, GPIO_NUM_25 = 25, GPIO_NUM_26 = 26, GPIO_NUM_27 = 27, GPIO_NUM_28 = 28, GPIO_NUM_29 = 29, GPIO_NUM_30 = 30 }; typedef int esp_err_t; typedef int gpio_mode_t;
#define ESP_OK 0
#define GPIO_MODE_OUTPUT 2
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_DISABLE 0
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
static inline esp_err_t gpio_config(const gpio_config_t*){return 0;}
static inline esp_err_t gpio_set_level(gpio_num_t,uint32_t){return 0;}
static inline int gpio_get_level(gpio_num_t){return 1;}
static inline esp_err_t gpio_set_direction(gpio_num_t,gpio_mode_t){return 0;}
static inline esp_err_t gpio_hold_en(gpio_num_t){return 0;}
static inline esp_err_t gpio_hold_dis(gpio_num_t){return 0;}
static inline esp_err_t gpio_deep_sleep_hold_en(void){return 0;}
static inline esp_err_t gpio_deep_sleep_hold_dis(void){return 0;}
static inline esp_err_t gpio_sleep_sel_dis(gpio_num_t){return 0;}
static inline esp_err_t gpio_set_pull_mode(gpio_num_t,int){return 0;}
#define GPIO_PULLUP_ONLY 0
#define GPIO_INTR_DISABLE 0
#define GPIO_FLOATING 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLUP_ENABLE 1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_FAIL -1
