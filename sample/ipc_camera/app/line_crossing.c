#define _POSIX_C_SOURCE 200809L

#include "line_crossing.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <utils/plat_log.h>

#define LINE_CROSSING_TRACK_CAPACITY 32U

/*
 * YOLOv8只有检测框，没有目标ID，因此这里维护一个轻量轨迹表。
 * anchor是目标最后一次明确处于防抖带外的位置，用于判断是否真正穿线。
 */
typedef struct LineTrack {
    int active;
    unsigned int id;
    int point_x;
    int point_y;
    int anchor_x;
    int anchor_y;
    int stable_side;
    unsigned int missing_snapshots;
    unsigned long long last_trigger_ms;
} LineTrack;

struct LineCrossingContext {
    LineCrossingConfig config;
    pthread_t thread_id;
    volatile int stop_requested;
    int thread_started;
    unsigned long long last_sequence;
    unsigned long long processed_snapshots;
    unsigned long long event_count;
    unsigned int next_track_id;
    LineTrack tracks[LINE_CROSSING_TRACK_CAPACITY];
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
    nanosleep(&delay, NULL);
}

/* 叉积的符号表示点位于有向警戒线的哪一侧。 */
static long long line_side_value(const LineCrossingContext *context,
                                 int point_x,
                                 int point_y)
{
    long long line_dx = context->config.line_bx - context->config.line_ax;
    long long line_dy = context->config.line_by - context->config.line_ay;
    long long point_dx = point_x - context->config.line_ax;
    long long point_dy = point_y - context->config.line_ay;

    return line_dx * point_dy - line_dy * point_dx;
}

/*
 * 用“叉积平方”和“线长平方×滞回宽度平方”比较，不需要sqrt和libm。
 * 返回0表示点仍在线附近的防抖带内，暂不改变稳定侧别。
 */
static int classify_stable_side(const LineCrossingContext *context,
                                int point_x,
                                int point_y)
{
    long long side = line_side_value(context, point_x, point_y);
    long long line_dx = context->config.line_bx - context->config.line_ax;
    long long line_dy = context->config.line_by - context->config.line_ay;
    unsigned long long side_square;
    unsigned long long line_length_square;
    unsigned long long margin_square;

    side_square = (unsigned long long)(side < 0 ? -side : side);
    side_square *= side_square;
    line_length_square = (unsigned long long)(line_dx * line_dx +
                                               line_dy * line_dy);
    margin_square = (unsigned long long)context->config.hysteresis_pixels *
                    context->config.hysteresis_pixels;
    if (side_square <= line_length_square * margin_square) {
        return 0;
    }
    return side > 0 ? 1 : -1;
}

static long long orientation(int ax,
                             int ay,
                             int bx,
                             int by,
                             int cx,
                             int cy)
{
    return (long long)(bx - ax) * (cy - ay) -
           (long long)(by - ay) * (cx - ax);
}

static int point_on_segment(int ax,
                            int ay,
                            int bx,
                            int by,
                            int px,
                            int py)
{
    int min_x = ax < bx ? ax : bx;
    int max_x = ax > bx ? ax : bx;
    int min_y = ay < by ? ay : by;
    int max_y = ay > by ? ay : by;

    return px >= min_x && px <= max_x && py >= min_y && py <= max_y;
}

/* 判断目标轨迹段与有限警戒线段是否相交，避免在线段延长线上误报。 */
static int segments_intersect(int first_ax,
                              int first_ay,
                              int first_bx,
                              int first_by,
                              int second_ax,
                              int second_ay,
                              int second_bx,
                              int second_by)
{
    long long o1 = orientation(first_ax, first_ay, first_bx, first_by,
                               second_ax, second_ay);
    long long o2 = orientation(first_ax, first_ay, first_bx, first_by,
                               second_bx, second_by);
    long long o3 = orientation(second_ax, second_ay, second_bx, second_by,
                               first_ax, first_ay);
    long long o4 = orientation(second_ax, second_ay, second_bx, second_by,
                               first_bx, first_by);

    if (((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) &&
        ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0))) {
        return 1;
    }
    if (o1 == 0 && point_on_segment(first_ax, first_ay, first_bx, first_by,
                                    second_ax, second_ay)) {
        return 1;
    }
    if (o2 == 0 && point_on_segment(first_ax, first_ay, first_bx, first_by,
                                    second_bx, second_by)) {
        return 1;
    }
    if (o3 == 0 && point_on_segment(second_ax, second_ay,
                                    second_bx, second_by,
                                    first_ax, first_ay)) {
        return 1;
    }
    if (o4 == 0 && point_on_segment(second_ax, second_ay,
                                    second_bx, second_by,
                                    first_bx, first_by)) {
        return 1;
    }
    return 0;
}

static const char *direction_name(LineCrossingDirection direction)
{
    return direction == LINE_CROSSING_POSITIVE_TO_NEGATIVE
               ? "positive-to-negative"
               : "negative-to-positive";
}

static void publish_event(LineCrossingContext *context,
                          LineTrack *track,
                          int new_side,
                          const NpuDetectionSnapshot *snapshot,
                          int point_x,
                          int point_y,
                          unsigned long long now_ms)
{
    LineCrossingEvent event;

    memset(&event, 0, sizeof(event));
    event.track_id = track->id;
    event.direction = track->stable_side > 0 && new_side < 0
                          ? LINE_CROSSING_POSITIVE_TO_NEGATIVE
                          : LINE_CROSSING_NEGATIVE_TO_POSITIVE;
    event.sequence = snapshot->sequence;
    event.frame_pts_us = snapshot->frame_pts_us;
    event.point_x = point_x;
    event.point_y = point_y;

    ++context->event_count;
    track->last_trigger_ms = now_ms;
    alogd("[LINE] Crossing event: id=%u, direction=%s, point=(%d,%d), "
          "sequence=%llu, pts=%llu us, total=%llu",
          event.track_id,
          direction_name(event.direction),
          event.point_x,
          event.point_y,
          event.sequence,
          event.frame_pts_us,
          context->event_count);

    if (context->config.event_callback != NULL) {
        context->config.event_callback(context->config.event_callback_opaque,
                                       &event);
    }
}

static void update_track(LineCrossingContext *context,
                         LineTrack *track,
                         const NpuDetectionSnapshot *snapshot,
                         int point_x,
                         int point_y,
                         unsigned long long now_ms)
{
    int new_side = classify_stable_side(context, point_x, point_y);

    track->point_x = point_x;
    track->point_y = point_y;
    track->missing_snapshots = 0U;

    if (new_side == 0) {
        return;
    }
    if (track->stable_side == 0) {
        track->stable_side = new_side;
        track->anchor_x = point_x;
        track->anchor_y = point_y;
        return;
    }
    if (new_side != track->stable_side) {
        int crosses_finite_line = segments_intersect(
            track->anchor_x,
            track->anchor_y,
            point_x,
            point_y,
            context->config.line_ax,
            context->config.line_ay,
            context->config.line_bx,
            context->config.line_by);
        int cooldown_elapsed = track->last_trigger_ms == 0U ||
            now_ms < track->last_trigger_ms ||
            now_ms - track->last_trigger_ms >= context->config.cooldown_ms;

        if (crosses_finite_line && cooldown_elapsed) {
            publish_event(context,
                          track,
                          new_side,
                          snapshot,
                          point_x,
                          point_y,
                          now_ms);
        }
        /* 即使仍在冷却期，也更新侧别，防止冷却结束后补报旧动作。 */
        track->stable_side = new_side;
    }
    track->anchor_x = point_x;
    track->anchor_y = point_y;
}

static LineTrack *allocate_track(LineCrossingContext *context)
{
    unsigned int index;

    for (index = 0U; index < context->config.max_tracks; ++index) {
        if (!context->tracks[index].active) {
            LineTrack *track = &context->tracks[index];

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

static void process_snapshot(LineCrossingContext *context,
                             const NpuDetectionSnapshot *snapshot)
{
    unsigned char matched[LINE_CROSSING_TRACK_CAPACITY];
    unsigned int detection_index;
    unsigned int track_index;
    unsigned long long now_ms = monotonic_time_ms();
    unsigned long long match_limit_square =
        (unsigned long long)context->config.match_distance_pixels *
        context->config.match_distance_pixels;

    memset(matched, 0, sizeof(matched));
    for (detection_index = 0U;
         detection_index < (unsigned int)snapshot->detection_count;
         ++detection_index) {
        const YoloDetection *detection = &snapshot->detections[detection_index];
        LineTrack *best_track = NULL;
        unsigned int best_index = 0U;
        unsigned long long best_distance_square = ULLONG_MAX;
        int point_x;
        int point_y;

        if (detection->label != 0) {
            continue;
        }
        /* 监控规则使用脚底近似点，而不是人框中心。 */
        point_x = (detection->xmin + detection->xmax) / 2;
        point_y = detection->ymax;

        for (track_index = 0U;
             track_index < context->config.max_tracks;
             ++track_index) {
            LineTrack *candidate = &context->tracks[track_index];
            long long dx;
            long long dy;
            unsigned long long distance_square;

            if (!candidate->active || matched[track_index]) {
                continue;
            }
            dx = point_x - candidate->point_x;
            dy = point_y - candidate->point_y;
            distance_square = (unsigned long long)(dx * dx + dy * dy);
            if (distance_square <= match_limit_square &&
                distance_square < best_distance_square) {
                best_track = candidate;
                best_index = track_index;
                best_distance_square = distance_square;
            }
        }

        if (best_track == NULL) {
            best_track = allocate_track(context);
            if (best_track == NULL) {
                alogw("[LINE] Track table full; detection skipped");
                continue;
            }
            best_index = (unsigned int)(best_track - context->tracks);
        }
        matched[best_index] = 1U;
        update_track(context,
                     best_track,
                     snapshot,
                     point_x,
                     point_y,
                     now_ms);
    }

    for (track_index = 0U;
         track_index < context->config.max_tracks;
         ++track_index) {
        LineTrack *track = &context->tracks[track_index];

        if (!track->active || matched[track_index]) {
            continue;
        }
        ++track->missing_snapshots;
        if (track->missing_snapshots >
            context->config.max_missing_snapshots) {
            memset(track, 0, sizeof(*track));
        }
    }
    ++context->processed_snapshots;
}

static void *line_crossing_thread(void *argument)
{
    LineCrossingContext *context = argument;

    alogd("[LINE] Detection thread started");
    while (!context->stop_requested) {
        NpuDetectionSnapshot snapshot;

        memset(&snapshot, 0, sizeof(snapshot));
        if (npu_detector_get_latest(context->config.detector, &snapshot) != 0) {
            aloge("[LINE] Read NPU result snapshot failed");
            break;
        }
        if (snapshot.sequence != 0U &&
            snapshot.sequence != context->last_sequence) {
            /* 防御异常数量，避免损坏的快照造成数组越界。 */
            if (snapshot.detection_count < 0) {
                snapshot.detection_count = 0;
            } else if (snapshot.detection_count > YOLOV8_MAX_DETECTIONS) {
                snapshot.detection_count = YOLOV8_MAX_DETECTIONS;
            }
            process_snapshot(context, &snapshot);
            context->last_sequence = snapshot.sequence;
        }
        sleep_ms(context->config.poll_interval_ms);
    }

    alogd("[LINE] Detection thread stopped: snapshots=%llu, events=%llu",
          context->processed_snapshots,
          context->event_count);
    return NULL;
}

LineCrossingContext *line_crossing_create(const LineCrossingConfig *config)
{
    LineCrossingContext *context;

    if (config == NULL || config->detector == NULL ||
        config->model_width <= 0 || config->model_height <= 0 ||
        (config->line_ax == config->line_bx &&
         config->line_ay == config->line_by) ||
        config->line_ax < 0 || config->line_ax > config->model_width ||
        config->line_bx < 0 || config->line_bx > config->model_width ||
        config->line_ay < 0 || config->line_ay > config->model_height ||
        config->line_by < 0 || config->line_by > config->model_height ||
        config->hysteresis_pixels == 0U ||
        config->match_distance_pixels == 0U ||
        config->max_missing_snapshots == 0U ||
        config->cooldown_ms == 0U || config->poll_interval_ms == 0U ||
        config->max_tracks == 0U ||
        config->max_tracks > LINE_CROSSING_TRACK_CAPACITY) {
        return NULL;
    }

    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    context->config = *config;
    context->next_track_id = 1U;
    return context;
}

int line_crossing_start(LineCrossingContext *context)
{
    int thread_ret;

    if (context == NULL || context->thread_started) {
        return -1;
    }
    context->stop_requested = 0;
    thread_ret = pthread_create(&context->thread_id,
                                NULL,
                                line_crossing_thread,
                                context);
    if (thread_ret != 0) {
        aloge("[LINE] Create detection thread failed: ret=%d", thread_ret);
        return -1;
    }
    context->thread_started = 1;
    alogd("[LINE] Crossing detector started: line=(%d,%d)-(%d,%d), "
          "hysteresis=%u, match=%u, missing=%u, cooldown=%u ms, tracks=%u",
          context->config.line_ax,
          context->config.line_ay,
          context->config.line_bx,
          context->config.line_by,
          context->config.hysteresis_pixels,
          context->config.match_distance_pixels,
          context->config.max_missing_snapshots,
          context->config.cooldown_ms,
          context->config.max_tracks);
    return 0;
}

int line_crossing_stop(LineCrossingContext *context)
{
    if (context == NULL) {
        return -1;
    }
    context->stop_requested = 1;
    if (context->thread_started) {
        if (pthread_join(context->thread_id, NULL) != 0) {
            aloge("[LINE] Join detection thread failed");
            return -1;
        }
        context->thread_started = 0;
    }
    return 0;
}

void line_crossing_destroy(LineCrossingContext *context)
{
    if (context == NULL) {
        return;
    }
    if (context->thread_started) {
        line_crossing_stop(context);
    }
    free(context);
}
