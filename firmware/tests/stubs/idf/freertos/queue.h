#pragma once
#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef void *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t sz){ (void)n;(void)sz; return (QueueHandle_t)1; }
static inline BaseType_t xQueueSend(QueueHandle_t q, const void *i, TickType_t t){ (void)q;(void)i;(void)t; return pdTRUE; }
static inline BaseType_t xQueueReceive(QueueHandle_t q, void *i, TickType_t t){ (void)q;(void)i;(void)t; return pdFALSE; }

#ifdef __cplusplus
}
#endif
