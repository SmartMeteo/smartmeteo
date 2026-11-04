#pragma once
#include <stdint.h>
#include <sys/time.h>
static inline int64_t esp_timer_get_time(void){ struct timeval tv; gettimeofday(&tv,0); return (int64_t)tv.tv_sec*1000000+tv.tv_usec; }
