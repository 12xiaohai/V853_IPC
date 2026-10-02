#ifndef IPC_CAMERA_VIPP4_DIAGNOSTICS_H
#define IPC_CAMERA_VIPP4_DIAGNOSTICS_H

#include <stdint.h>
#include <time.h>

/* 诊断使用单调时钟：NTP调整系统日期时，处理耗时不会发生跳变。 */
static inline uint64_t vipp4_monotonic_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

/* 时钟读取失败时不把无效时间计入统计，避免产生巨大的伪耗时。 */
static inline uint64_t vipp4_elapsed_us(uint64_t begin, uint64_t end)
{
    return begin != 0 && end >= begin ? end - begin : 0;
}

typedef struct Vipp4DurationStat {
    uint64_t count;
    uint64_t sum_us;
    uint64_t max_us;
} Vipp4DurationStat;

static inline void vipp4_duration_add(Vipp4DurationStat *stat,
                                     uint64_t begin, uint64_t end)
{
    if (begin == 0 || end == 0 || end < begin) {
        return;
    }
    ++stat->count;
    stat->sum_us += end - begin;
    if (end - begin > stat->max_us) {
        stat->max_us = end - begin;
    }
}

static inline uint64_t vipp4_duration_avg(const Vipp4DurationStat *stat)
{
    return stat->count != 0 ? stat->sum_us / stat->count : 0;
}

/* 一次显示调用的数据，由采集线程独占；VO回调不会访问这个结构。 */
typedef struct VideoDisplayTiming {
    uint64_t g2d_begin_us;
    uint64_t g2d_end_us;
    uint64_t vo_begin_us;
    uint64_t vo_end_us;
} VideoDisplayTiming;

#endif
