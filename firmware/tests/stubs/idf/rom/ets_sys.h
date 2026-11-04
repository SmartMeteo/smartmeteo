#pragma once
#include "../esp_rom_sys.h"

#ifdef __cplusplus
extern "C" {
#endif
#define ets_delay_us esp_rom_delay_us
static inline int ets_printf(const char *f, ...){ (void)f; return 0; }

#ifdef __cplusplus
}
#endif
