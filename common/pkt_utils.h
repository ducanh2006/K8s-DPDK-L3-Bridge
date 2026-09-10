#ifndef PKT_UTILS_H
#define PKT_UTILS_H

#include <stdint.h>
#include <time.h>

/**
 * Lấy thời gian hiện tại tính bằng nano giây (đồng hồ Monotonic độ chính xác cao)
 */
static inline uint64_t get_current_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#endif /* PKT_UTILS_H */
