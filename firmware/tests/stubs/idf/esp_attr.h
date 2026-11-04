#pragma once

#ifdef __cplusplus
extern "C" {
#endif
// На приборе это размещение в RTC-памяти. Здесь — обычные переменные: тест их и читает, и пишет,
// а «переживает сон» проверяется на железе, не на компьютере.
#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_ATTR

#ifdef __cplusplus
}
#endif
