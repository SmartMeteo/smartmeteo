#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdMS_TO_TICKS(ms)  ((TickType_t)((ms) / portTICK_PERIOD_MS))
#define portTICK_PERIOD_MS 1U
#define portMAX_DELAY      ((TickType_t)0xFFFFFFFFU)
#define configTICK_RATE_HZ 1000
#define tskNO_AFFINITY     0x7FFFFFFF

// Спинлок: на одном ядре без прерываний это ничто, а тест не многопоточный. Проверять здесь
// нечего — гонка между задачами httpd и цикла ловится только на приборе.
typedef struct { unsigned owner; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(m)          do{ (void)(m); }while(0)
#define portEXIT_CRITICAL(m)           do{ (void)(m); }while(0)
#define portENTER_CRITICAL_ISR(m)      do{ (void)(m); }while(0)
#define portEXIT_CRITICAL_ISR(m)       do{ (void)(m); }while(0)
#define portENTER_CRITICAL_SAFE(m)     do{ (void)(m); }while(0)
#define portEXIT_CRITICAL_SAFE(m)      do{ (void)(m); }while(0)
#define portYIELD_FROM_ISR(...)        do{}while(0)
#define spinlock_initialize(m)         do{ (void)(m); }while(0)

#define BIT0 (1U<<0)
#define BIT1 (1U<<1)
#define BIT2 (1U<<2)
#define BIT3 (1U<<3)
#define BIT4 (1U<<4)
#define BIT5 (1U<<5)
#define BIT6 (1U<<6)
#define BIT7 (1U<<7)
#define BIT8 (1U<<8)
#define BIT9 (1U<<9)
#define BIT10 (1U<<10)
#define BIT11 (1U<<11)
#define BIT12 (1U<<12)
#define BIT13 (1U<<13)
#define BIT14 (1U<<14)
#define BIT15 (1U<<15)
#define BIT16 (1U<<16)
#define BIT17 (1U<<17)
#define BIT18 (1U<<18)
#define BIT19 (1U<<19)
#define BIT20 (1U<<20)
#define BIT21 (1U<<21)
#define BIT22 (1U<<22)
#define BIT23 (1U<<23)
#define BIT24 (1U<<24)
#define BIT25 (1U<<25)
#define BIT26 (1U<<26)
#define BIT27 (1U<<27)
#define BIT28 (1U<<28)
#define BIT29 (1U<<29)
#define BIT30 (1U<<30)
#define BIT31 (1U<<31)

#ifdef __cplusplus
}
#endif
