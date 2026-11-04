#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef void *spi_device_handle_t;
typedef int spi_host_device_t;
#define SPI1_HOST 0
#define SPI2_HOST 1
#define SPI3_HOST 2
#define SPI_DMA_DISABLED 0
#define SPI_DMA_CH_AUTO  3
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz, flags, intr_flags; } spi_bus_config_t;
typedef struct { uint8_t command_bits, address_bits, dummy_bits, mode; uint16_t duty_cycle_pos, cs_ena_pretrans;
                 uint8_t cs_ena_posttrans; int clock_speed_hz, input_delay_ns, spics_io_num; uint32_t flags;
                 int queue_size; void (*pre_cb)(void*); void (*post_cb)(void*); } spi_device_interface_config_t;
typedef struct { uint32_t flags; uint16_t cmd; uint64_t addr; size_t length, rxlength; void *user;
                 const void *tx_buffer; void *rx_buffer; } spi_transaction_t;
static inline esp_err_t spi_bus_initialize(spi_host_device_t h, const spi_bus_config_t *c, int d){ (void)h;(void)c;(void)d; return ESP_OK; }
static inline esp_err_t spi_bus_free(spi_host_device_t h){ (void)h; return ESP_OK; }
static inline esp_err_t spi_bus_add_device(spi_host_device_t h, const spi_device_interface_config_t *c, spi_device_handle_t *d){ (void)h;(void)c; if(d)*d=(spi_device_handle_t)1; return ESP_OK; }
static inline esp_err_t spi_bus_remove_device(spi_device_handle_t d){ (void)d; return ESP_OK; }
static inline esp_err_t spi_device_transmit(spi_device_handle_t d, spi_transaction_t *t){ (void)d;(void)t; return ESP_OK; }
static inline esp_err_t spi_device_polling_transmit(spi_device_handle_t d, spi_transaction_t *t){ (void)d;(void)t; return ESP_OK; }

#ifdef __cplusplus
}
#endif
