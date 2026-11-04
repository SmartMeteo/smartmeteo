#pragma once
#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void){ return (SemaphoreHandle_t)1; }
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){ return (SemaphoreHandle_t)1; }
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void){ return (SemaphoreHandle_t)1; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t){ (void)s;(void)t; return pdTRUE; }
static inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t){ (void)s;(void)t; return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s){ (void)s; return pdTRUE; }
static inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s){ (void)s; return pdTRUE; }
static inline BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *w){ (void)s;(void)w; return pdTRUE; }
static inline void vSemaphoreDelete(SemaphoreHandle_t s){ (void)s; }

#ifdef __cplusplus
}
#endif
