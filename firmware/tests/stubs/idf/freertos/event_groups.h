#pragma once
#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef void *EventGroupHandle_t;
typedef uint32_t EventBits_t;
static inline EventGroupHandle_t xEventGroupCreate(void){ return (EventGroupHandle_t)1; }
static inline EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b){ (void)g; return b; }
static inline EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b){ (void)g; return b; }
static inline EventBits_t xEventGroupGetBits(EventGroupHandle_t g){ (void)g; return 0; }
static inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t b, BaseType_t c, BaseType_t w, TickType_t t){
    (void)g;(void)b;(void)c;(void)w;(void)t; return 0; }
static inline void vEventGroupDelete(EventGroupHandle_t g){ (void)g; }

#ifdef __cplusplus
}
#endif
