#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
// Детерминированный: пароль точки доступа и код Matter должны быть воспроизводимы в прогоне, иначе
// тест на «ровно 8 знаков из разрешённого алфавита» падал бы через раз и его бы отключили.
uint32_t esp_random(void);
void     esp_fill_random(void *buf, size_t len);

#ifdef __cplusplus
}
#endif
