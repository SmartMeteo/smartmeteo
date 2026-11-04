#pragma once
#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef void *TaskHandle_t;
// Задержка — единственная функция FreeRTOS с поведением: она ДВИГАЕТ часы оснастки. Иначе
// пятисекундное ожидание датчика прошло бы мгновенно, и такт, который на приборе занимает шесть
// секунд, посчитался бы нулевым — а его длительность и есть то, из чего считается опережение.
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void){ return (TaskHandle_t)1; }
static inline BaseType_t xTaskCreate(void (*f)(void*), const char *n, uint32_t s, void *p, UBaseType_t pr, TaskHandle_t *h){
    (void)f;(void)n;(void)s;(void)p;(void)pr; if(h)*h=(TaskHandle_t)1; return pdPASS; }
static inline BaseType_t xTaskCreatePinnedToCore(void (*f)(void*), const char *n, uint32_t s, void *p, UBaseType_t pr, TaskHandle_t *h, int c){
    (void)c; return xTaskCreate(f,n,s,p,pr,h); }
static inline void vTaskDelete(TaskHandle_t h){ (void)h; }
static inline void vTaskSuspend(TaskHandle_t h){ (void)h; }
static inline void vTaskResume(TaskHandle_t h){ (void)h; }
static inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t h){ (void)h; return 4096; }
static inline void taskYIELD(void){}

#ifdef __cplusplus
}
#endif
