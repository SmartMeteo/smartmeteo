#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// На компьютере ждать нечего: подтяжка устанавливается мгновенно, потому что её моделирует
// gpio_get_level, а не резистор.
static inline void esp_rom_delay_us(uint32_t us) { (void)us; }
#ifdef __cplusplus
}
#endif
