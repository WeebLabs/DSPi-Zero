#pragma once

#include "hardware/timer.h"

static inline absolute_time_t get_absolute_time(void) {
    return time_us_64();
}
static inline uint64_t to_us_since_boot(absolute_time_t t) {
    return t;
}
static inline uint32_t to_ms_since_boot(absolute_time_t t) {
    return (uint32_t)(t / 1000u);
}
static inline absolute_time_t delayed_by_us(absolute_time_t t, uint64_t us) {
    return t + us;
}
static inline absolute_time_t delayed_by_ms(absolute_time_t t, uint32_t ms) {
    return t + (uint64_t)ms * 1000u;
}
static inline absolute_time_t make_timeout_time_us(uint64_t us) {
    return time_us_64() + us;
}
static inline absolute_time_t make_timeout_time_ms(uint32_t ms) {
    return time_us_64() + (uint64_t)ms * 1000u;
}
static inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) {
    return (int64_t)(to - from);
}
static inline bool time_reached(absolute_time_t t) {
    return time_us_64() >= t;
}

static inline void sleep_us(uint64_t us) {
    busy_wait_us(us);
}
static inline void sleep_ms(uint32_t ms) {
    busy_wait_ms(ms);
}

// Alarm pools exist only to be named in configurations (pico_spdif_rx's); nothing schedules on them.
typedef struct alarm_pool alarm_pool_t;
static inline alarm_pool_t *alarm_pool_get_default(void) {
    return 0;
}
