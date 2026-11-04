#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t; typedef void* spi_device_handle_t; typedef int spi_host_device_t;
#define SPI2_HOST 1
#define ESP_OK 0
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz, mode, spics_io_num, queue_size, flags; } spi_device_interface_config_t;
typedef struct { size_t length; const void* tx_buffer; void* rx_buffer; size_t rxlength; uint32_t flags; } spi_transaction_t;
static inline esp_err_t spi_bus_initialize(spi_host_device_t,const spi_bus_config_t*,int){return 0;}
static inline esp_err_t spi_bus_add_device(spi_host_device_t,const spi_device_interface_config_t*,spi_device_handle_t*){return 0;}
static inline esp_err_t spi_device_polling_transmit(spi_device_handle_t,spi_transaction_t*){return 0;}
#define SPI_DMA_CH_AUTO 3
