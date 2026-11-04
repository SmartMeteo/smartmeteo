#pragma once

#ifdef __cplusplus
extern "C" {
#endif
// На приборе это включение истинного источника случайности, пока радио не поднято. Здесь ничего:
// оснастка даёт детерминированные числа, и это осознанно — см. esp_random.h.
static inline void bootloader_random_enable(void){}
static inline void bootloader_random_disable(void){}

#ifdef __cplusplus
}
#endif
