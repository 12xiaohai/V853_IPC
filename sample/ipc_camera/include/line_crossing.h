#ifndef IPC_CAMERA_LINE_CROSSING_H
#define IPC_CAMERA_LINE_CROSSING_H

#include "npu_detector.h"

/*
 * 越线方向由警戒线A->B的有向关系表示。
 * POSITIVE_TO_NEGATIVE表示目标从叉积为正的一侧移动到负的一侧，
 * NEGATIVE_TO_POSITIVE则相反。这样任意方向的警戒线都能使用同一套算法。
 */
typedef enum LineCrossingDirection {
    LINE_CROSSING_POSITIVE_TO_NEGATIVE = 1,
    LINE_CROSSING_NEGATIVE_TO_POSITIVE = 2
} LineCrossingDirection;

typedef struct LineCrossingEvent {
    unsigned int track_id;
    LineCrossingDirection direction;
    unsigned long long sequence;
    unsigned long long frame_pts_us;
    int point_x;
    int point_y;
} LineCrossingEvent;

typedef void (*LineCrossingEventCallback)(
    void *opaque,
    const LineCrossingEvent *event);

typedef struct LineCrossingConfig {
    NpuDetectorContext *detector;
    int model_width;
    int model_height;

    /* 警戒线两个端点，坐标属于NPU模型画面。 */
    int line_ax;
    int line_ay;
    int line_bx;
    int line_by;

    /*
     * hysteresis_pixels：线两侧的防抖带宽度；目标离开防抖带后才确认侧别。
     * match_distance_pixels：相邻检测之间允许的最大底边中点距离。
     */
    unsigned int hysteresis_pixels;
    unsigned int match_distance_pixels;
    unsigned int max_missing_snapshots;
    unsigned int cooldown_ms;
    unsigned int poll_interval_ms;
    unsigned int max_tracks;

    /* 可选回调，后续音频报警模块可直接订阅越线事件。 */
    LineCrossingEventCallback event_callback;
    void *event_callback_opaque;
} LineCrossingConfig;

typedef struct LineCrossingContext LineCrossingContext;

/* 只分配并检查配置，不启动线程。 */
LineCrossingContext *line_crossing_create(const LineCrossingConfig *config);

/* 启动检测结果消费线程。 */
int line_crossing_start(LineCrossingContext *context);

/* 停止线程；必须在销毁NPU detector之前调用。 */
int line_crossing_stop(LineCrossingContext *context);

/* 释放上下文，未显式stop时会自动安全停止。 */
void line_crossing_destroy(LineCrossingContext *context);

#endif
