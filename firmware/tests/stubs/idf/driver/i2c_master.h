#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;
typedef int i2c_port_num_t;
#define I2C_NUM_0 0
#define I2C_NUM_1 1
typedef enum { I2C_CLK_SRC_DEFAULT } i2c_clock_source_t;
typedef enum { I2C_ADDR_BIT_LEN_7, I2C_ADDR_BIT_LEN_10 } i2c_addr_bit_len_t;
typedef struct { i2c_port_num_t i2c_port; int sda_io_num, scl_io_num; i2c_clock_source_t clk_source;
                 uint8_t glitch_ignore_cnt; int intr_priority; int trans_queue_depth;
                 struct { uint32_t enable_internal_pullup:1; uint32_t allow_pd:1; } flags; } i2c_master_bus_config_t;
typedef struct { i2c_addr_bit_len_t dev_addr_length; uint16_t device_address; uint32_t scl_speed_hz;
                 uint32_t scl_wait_us; struct { uint32_t disable_ack_check:1; } flags; } i2c_device_config_t;

// Шина отвечает НЕ пустотой. Описатель устройства помнит свой адрес, и обмен разбирается по нему:
// газомер MAX17048 (0x36) отдаёт настоящие регистры, посчитанные из Host::vcell, поэтому над
// заглушкой собирается настоящий max17048.cpp и арифметика LSB 78,125 мкВ исполняется как на
// приборе. Остальные адреса не отвечают — и это не лень: «датчик молчит» — законное состояние, в
// котором прошивка обязана не создать запись в истории и не выдать прошлое значение за свежее.
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *c, i2c_master_bus_handle_t *h);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t h);
esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t h);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b, const i2c_device_config_t *c, i2c_master_dev_handle_t *d);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t b, uint16_t addr, int t);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *w, size_t n, int t);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t d, uint8_t *r, size_t n, int t);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn, int t);

#ifdef __cplusplus
}
#endif
