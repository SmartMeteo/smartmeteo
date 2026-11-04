#pragma once
#include <stdlib.h>
#include <stddef.h>
#define MALLOC_CAP_8BIT     (1<<2)
#define MALLOC_CAP_DEFAULT  (1<<3)
#define MALLOC_CAP_INTERNAL (1<<11)
#define MALLOC_CAP_DMA      (1<<3)
#define MALLOC_CAP_SPIRAM   (1<<10)

#ifdef __cplusplus
extern "C" {
#endif
// Задаётся из теста: свободная куча — это порог самотеста нового образа, и ложное срабатывание
// здесь отправляет здоровый образ в откат навсегда.
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
size_t heap_caps_get_total_size(unsigned caps);
size_t heap_caps_get_minimum_free_size(unsigned caps);
static inline void *heap_caps_malloc(size_t n, unsigned caps){ (void)caps; return malloc(n); }
static inline void  heap_caps_free(void *p){ free(p); }

#ifdef __cplusplus
}
#endif
