#define _POSIX_C_SOURCE 200809L

#include "region_intrusion.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utils/plat_log.h>

#define REGION_TRACK_CAPACITY 32U
#define REGION_MAX_MODEL_SIZE 8192

typedef enum RegionTrackState {
    REGION_OUTSIDE = 0,
    REGION_ENTER_PENDING,
    REGION_INSIDE,
    REGION_LEAVE_PENDING
} RegionTrackState;

/* 每个人独立维护状态，不能让两个人共用一个inside变量。 */
typedef struct RegionTrack {
    int active;
    unsigned int id;
    int point_x;
    int point_y;
    RegionTrackState state;
    unsigned int confirm_count;
    unsigned int missing_snapshots;
} RegionTrack;

struct RegionIntrusionContext {
    RegionIntrusionConfig config;
    pthread_t thread_id;
    atomic_int stop_requested; /* C原子变量避免主线程和工作线程的读写竞争。 */
    int thread_started;
    unsigned int next_track_id;
    unsigned long long last_sequence;
    unsigned long long processed_snapshots;
    unsigned long long enter_events;
    unsigned long long leave_events;
    unsigned long long expired_tracks;
    RegionTrack tracks[REGION_TRACK_CAPACITY];
};

static unsigned long long monotonic_time_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return (unsigned long long)now.tv_sec * 1000ULL +
           (unsigned long long)now.tv_nsec / 1000000ULL;
}

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000U);
    delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        /* 信号中断等待后继续睡剩余时间，不改变区域判定状态。 */
    }
}

static long long orientation(RegionPoint a, RegionPoint b, RegionPoint p)
{
    return (long long)(b.x - a.x) * (p.y - a.y) -
           (long long)(b.y - a.y) * (p.x - a.x);
}

static int point_on_segment(RegionPoint a, RegionPoint b, RegionPoint p)
{
    return orientation(a, b, p) == 0 &&
           p.x >= (a.x < b.x ? a.x : b.x) &&
           p.x <= (a.x > b.x ? a.x : b.x) &&
           p.y >= (a.y < b.y ? a.y : b.y) &&
           p.y <= (a.y > b.y ? a.y : b.y);
}

static int segments_intersect(RegionPoint a, RegionPoint b,
                              RegionPoint c, RegionPoint d)
{
    long long o1 = orientation(a, b, c), o2 = orientation(a, b, d);
    long long o3 = orientation(c, d, a), o4 = orientation(c, d, b);
    if (((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) &&
        ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0))) {
        return 1;
    }
    return point_on_segment(a, b, c) || point_on_segment(a, b, d) ||
           point_on_segment(c, d, a) || point_on_segment(c, d, b);
}

/*
 * 沿用原项目pnpoly射线法：从目标点向右画射线，穿边次数为奇数即在内。
 * 半开区间避免顶点重复计数；叉积替代整数除法，避免截断误差。
 * 边界归为区域内，底部被裁剪到y=320的人员仍能参加确认。
 */
static int point_in_polygon(const RegionIntrusionConfig *config, RegionPoint p)
{
    unsigned int i;
    int inside = 0;
    for (i = 0U; i < config->point_count; ++i) {
        RegionPoint a = config->points[i];
        RegionPoint b = config->points[(i + 1U) % config->point_count];
        long long side;
        if (point_on_segment(a, b, p)) {
            return 1;
        }
        if ((a.y > p.y) == (b.y > p.y)) {
            continue;
        }
        side = orientation(a, b, p);
        if ((b.y > a.y && side > 0) || (b.y < a.y && side < 0)) {
            inside = !inside;
        }
    }
    return inside;
}

/* 重复顶点、共线拐点、自交及零面积都视为配置错误。 */
static int valid_polygon(const RegionIntrusionConfig *config)
{
    unsigned int i, j;
    long long twice_area = 0;
    /* 先检查所有顶点再做差值/叉积，非法大整数也不能触发有符号溢出。 */
    for (i = 0U; i < config->point_count; ++i) {
        RegionPoint point = config->points[i];
        if (point.x < 0 || point.x > config->model_width ||
            point.y < 0 || point.y > config->model_height) {
            return 0;
        }
    }
    for (i = 0U; i < config->point_count; ++i) {
        RegionPoint a = config->points[i];
        RegionPoint b = config->points[(i + 1U) % config->point_count];
        RegionPoint c = config->points[(i + 2U) % config->point_count];
        if (orientation(a, b, c) == 0) {
            return 0;
        }
        twice_area += (long long)a.x * b.y - (long long)b.x * a.y;
        for (j = i + 1U; j < config->point_count; ++j) {
            RegionPoint d = config->points[j];
            RegionPoint e = config->points[(j + 1U) % config->point_count];
            if (a.x == d.x && a.y == d.y) {
                return 0;
            }
            /* 相邻边允许共享顶点，非相邻边不允许相交或接触。 */
            if (j != i + 1U && !(i == 0U && j == config->point_count - 1U) &&
                segments_intersect(a, b, d, e)) {
                return 0;
            }
        }
    }
    return twice_area != 0;
}

static void publish_event(RegionIntrusionContext *context, RegionTrack *track,
                          RegionIntrusionEventType type,
                          const NpuDetectionSnapshot *snapshot)
{
    RegionIntrusionEvent event;
    memset(&event, 0, sizeof(event));
    event.region_id = context->config.region_id;
    event.track_id = track->id;
    event.type = type;
    event.sequence = snapshot->sequence;
    event.frame_pts_us = snapshot->frame_pts_us;
    event.point_x = track->point_x;
    event.point_y = track->point_y;
    if (type == REGION_INTRUSION_ENTER) {
        ++context->enter_events;
    } else {
        ++context->leave_events;
    }
    alogd("[REGION] Intrusion event: region=%u, id=%u, type=%s, "
          "point=(%d,%d), sequence=%llu, pts=%llu us, enter=%llu, leave=%llu",
          event.region_id, event.track_id,
          type == REGION_INTRUSION_ENTER ? "enter" : "leave",
          event.point_x, event.point_y, event.sequence, event.frame_pts_us,
          context->enter_events, context->leave_events);
    if (context->config.event_callback != NULL) {
        context->config.event_callback(context->config.event_callback_opaque, &event);
    }
}

/* 漏检或跳过快照只撤销待确认计数，不能直接解释成“人员已经离开”。 */
static void cancel_pending(RegionTrack *track)
{
    if (track->state == REGION_ENTER_PENDING) {
        track->state = REGION_OUTSIDE;
    } else if (track->state == REGION_LEAVE_PENDING) {
        track->state = REGION_INSIDE;
    }
    track->confirm_count = 0U;
}

static void update_track(RegionIntrusionContext *context, RegionTrack *track,
                         const NpuDetectionSnapshot *snapshot, RegionPoint point)
{
    int inside = point_in_polygon(&context->config, point);
    track->point_x = point.x;
    track->point_y = point.y;
    track->missing_snapshots = 0U;
    if (inside) {
        if (track->state == REGION_INSIDE || track->state == REGION_LEAVE_PENDING) {
            track->state = REGION_INSIDE;
            track->confirm_count = 0U;
            return;
        }
        track->state = REGION_ENTER_PENDING;
        if (++track->confirm_count >= context->config.enter_confirm_snapshots) {
            track->state = REGION_INSIDE;
            track->confirm_count = 0U;
            publish_event(context, track, REGION_INTRUSION_ENTER, snapshot);
        }
    } else {
        if (track->state == REGION_OUTSIDE || track->state == REGION_ENTER_PENDING) {
            track->state = REGION_OUTSIDE;
            track->confirm_count = 0U;
            return;
        }
        track->state = REGION_LEAVE_PENDING;
        if (++track->confirm_count >= context->config.leave_confirm_snapshots) {
            track->state = REGION_OUTSIDE;
            track->confirm_count = 0U;
            publish_event(context, track, REGION_INTRUSION_LEAVE, snapshot);
        }
    }
}

static void expire_track(RegionIntrusionContext *context, RegionTrack *track)
{
    ++context->expired_tracks;
    /* 过期只表示无法继续关联，不产生leave，更不代表实际离开。 */
    memset(track, 0, sizeof(*track));
}

static RegionTrack *allocate_track(RegionIntrusionContext *context)
{
    unsigned int i;
    for (i = 0U; i < context->config.max_tracks; ++i) {
        RegionTrack *track = &context->tracks[i];
        if (!track->active) {
            memset(track, 0, sizeof(*track));
            track->active = 1;
            track->id = context->next_track_id++;
            if (context->next_track_id == 0U) {
                context->next_track_id = 1U;
            }
            return track;
        }
    }
    return NULL;
}

static void process_snapshot(RegionIntrusionContext *context,
                             const NpuDetectionSnapshot *snapshot)
{
    RegionPoint points[YOLOV8_MAX_DETECTIONS];
    unsigned char track_matched[REGION_TRACK_CAPACITY] = {0};
    unsigned char point_matched[YOLOV8_MAX_DETECTIONS] = {0};
    unsigned int point_count = 0U, i, j;
    int detection_count = snapshot->detection_count;
    unsigned long long match_limit =
        (unsigned long long)context->config.match_distance_pixels *
        context->config.match_distance_pixels;

    if (snapshot->sequence == 0U || snapshot->sequence == context->last_sequence) {
        return; /* 同一结果不能被50ms轮询重复计作两帧。 */
    }
    if (context->last_sequence != 0U &&
        (snapshot->sequence < context->last_sequence ||
         snapshot->sequence - context->last_sequence != 1U)) {
        for (i = 0U; i < context->config.max_tracks; ++i) {
            cancel_pending(&context->tracks[i]);
        }
    }
    if (detection_count < 0) {
        detection_count = 0;
    } else if (detection_count > YOLOV8_MAX_DETECTIONS) {
        detection_count = YOLOV8_MAX_DETECTIONS;
    }
    for (i = 0U; i < (unsigned int)detection_count; ++i) {
        const YoloDetection *detection = &snapshot->detections[i];
        if (detection->label != 0 || detection->xmin < 0 ||
            detection->ymin < 0 || detection->xmax > context->config.model_width ||
            detection->ymax > context->config.model_height ||
            detection->xmin >= detection->xmax || detection->ymin >= detection->ymax) {
            continue;
        }
        points[point_count].x = (detection->xmin + detection->xmax) / 2;
        points[point_count++].y = detection->ymax;
    }

    /*
     * 有界的最近距离关联：每轮选择全表最近的一对，双方各匹配一次。
     * 先关联所有旧轨迹，再创建新轨迹，避免两个框共用一个新ID。
     * 这是轻量跟踪，不是身份识别，遮挡或两人交错仍可能产生ID切换。
     */
    for (;;) {
        unsigned long long best_distance = ULLONG_MAX;
        unsigned int best_track = 0U, best_point = 0U;
        int found = 0;
        for (i = 0U; i < context->config.max_tracks; ++i) {
            RegionTrack *track = &context->tracks[i];
            if (!track->active || track_matched[i]) {
                continue;
            }
            for (j = 0U; j < point_count; ++j) {
                long long dx = points[j].x - track->point_x;
                long long dy = points[j].y - track->point_y;
                unsigned long long distance = (unsigned long long)(dx * dx + dy * dy);
                if (!point_matched[j] && distance <= match_limit && distance < best_distance) {
                    found = 1;
                    best_distance = distance;
                    best_track = i;
                    best_point = j;
                }
            }
        }
        if (!found) {
            break;
        }
        track_matched[best_track] = 1U;
        point_matched[best_point] = 1U;
        update_track(context, &context->tracks[best_track], snapshot, points[best_point]);
    }
    for (i = 0U; i < context->config.max_tracks; ++i) {
        RegionTrack *track = &context->tracks[i];
        if (track->active && !track_matched[i]) {
            cancel_pending(track);
            if (++track->missing_snapshots > context->config.max_missing_snapshots) {
                expire_track(context, track);
            }
        }
    }
    for (j = 0U; j < point_count; ++j) {
        if (!point_matched[j]) {
            RegionTrack *track = allocate_track(context);
            if (track == NULL) {
                alogw("[REGION] Track table full; detection skipped");
                continue;
            }
            update_track(context, track, snapshot, points[j]);
        }
    }
    context->last_sequence = snapshot->sequence;
    ++context->processed_snapshots;
}

static void *region_intrusion_thread(void *argument)
{
    RegionIntrusionContext *context = argument;
    unsigned long long last_update_ms = monotonic_time_ms();
    int stale_reported = 0;
    alogd("[REGION] Detection thread started");
    while (!atomic_load_explicit(&context->stop_requested, memory_order_relaxed)) {
        NpuDetectionSnapshot snapshot;
        unsigned long long now_ms = monotonic_time_ms();
        memset(&snapshot, 0, sizeof(snapshot));
        if (npu_detector_get_latest(context->config.detector, &snapshot) != 0) {
            aloge("[REGION] Read NPU result snapshot failed");
            break;
        }
        if (snapshot.sequence != 0U && snapshot.sequence != context->last_sequence) {
            process_snapshot(context, &snapshot);
            last_update_ms = now_ms;
            stale_reported = 0;
        } else if (!stale_reported && now_ms >= last_update_ms &&
                   now_ms - last_update_ms >= context->config.stale_timeout_ms) {
            unsigned int i;
            for (i = 0U; i < context->config.max_tracks; ++i) {
                if (context->tracks[i].active) {
                    expire_track(context, &context->tracks[i]);
                }
            }
            alogw("[REGION] NPU snapshot stale; tracks cleared without leave events");
            stale_reported = 1;
        }
        sleep_ms(context->config.poll_interval_ms);
    }
    alogd("[REGION] Detection thread stopped: snapshots=%llu, enter=%llu, "
          "leave=%llu, expired=%llu", context->processed_snapshots,
          context->enter_events, context->leave_events, context->expired_tracks);
    return NULL;
}

RegionIntrusionContext *region_intrusion_create(const RegionIntrusionConfig *config)
{
    RegionIntrusionContext *context;
    if (config == NULL || config->detector == NULL ||
        config->model_width <= 0 || config->model_width > REGION_MAX_MODEL_SIZE ||
        config->model_height <= 0 || config->model_height > REGION_MAX_MODEL_SIZE ||
        config->point_count < 3U || config->point_count > REGION_INTRUSION_MAX_POINTS ||
        config->enter_confirm_snapshots == 0U || config->leave_confirm_snapshots == 0U ||
        config->match_distance_pixels == 0U ||
        config->max_missing_snapshots == 0U || config->max_missing_snapshots > 1000U ||
        config->max_tracks == 0U || config->max_tracks > REGION_TRACK_CAPACITY ||
        config->poll_interval_ms == 0U || config->poll_interval_ms > 1000U ||
        config->stale_timeout_ms < config->poll_interval_ms || !valid_polygon(config)) {
        return NULL;
    }
    context = calloc(1, sizeof(*context));
    if (context != NULL) {
        context->config = *config; /* 顶点数组也被复制，调用者无需保留局部配置变量。 */
        context->next_track_id = 1U;
        atomic_init(&context->stop_requested, 0);
    }
    return context;
}

int region_intrusion_start(RegionIntrusionContext *context)
{
    int result;
    unsigned int i;
    if (context == NULL || context->thread_started) {
        return -1;
    }
    /* 支持stop后再次start，不带入旧轨迹和统计。 */
    memset(context->tracks, 0, sizeof(context->tracks));
    context->next_track_id = 1U;
    context->last_sequence = 0U;
    context->processed_snapshots = 0U;
    context->enter_events = context->leave_events = context->expired_tracks = 0U;
    atomic_store_explicit(&context->stop_requested, 0, memory_order_relaxed);
    result = pthread_create(&context->thread_id, NULL, region_intrusion_thread, context);
    if (result != 0) {
        aloge("[REGION] Create detection thread failed: ret=%d", result);
        return -1;
    }
    context->thread_started = 1;
    alogd("[REGION] Intrusion detector started: region=%u, points=%u, "
          "enter_confirm=%u, leave_confirm=%u, match=%u, missing=%u, tracks=%u",
          context->config.region_id, context->config.point_count,
          context->config.enter_confirm_snapshots, context->config.leave_confirm_snapshots,
          context->config.match_distance_pixels, context->config.max_missing_snapshots,
          context->config.max_tracks);
    for (i = 0U; i < context->config.point_count; ++i) {
        alogd("[REGION] Vertex[%u]=(%d,%d)", i,
              context->config.points[i].x, context->config.points[i].y);
    }
    return 0;
}

int region_intrusion_stop(RegionIntrusionContext *context)
{
    if (context == NULL) {
        return -1;
    }
    atomic_store_explicit(&context->stop_requested, 1, memory_order_relaxed);
    if (context->thread_started) {
        int result = pthread_join(context->thread_id, NULL);
        if (result != 0) {
            aloge("[REGION] Join detection thread failed: ret=%d", result);
            return -1;
        }
        context->thread_started = 0;
    }
    return 0;
}

void region_intrusion_destroy(RegionIntrusionContext *context)
{
    if (context == NULL) {
        return;
    }
    if (context->thread_started && region_intrusion_stop(context) != 0) {
        return; /* join失败不能释放仍可能被线程使用的内存。 */
    }
    free(context);
}
