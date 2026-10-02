#ifndef IPC_CAMERA_REGION_INTRUSION_H
#define IPC_CAMERA_REGION_INTRUSION_H

#include "npu_detector.h"

#define REGION_INTRUSION_MAX_POINTS 8U

/* 顶点按顺/逆时针排列，不重复首顶点；坐标属于NPU模型画面。 */
typedef struct RegionPoint { int x; int y; } RegionPoint;

typedef enum RegionIntrusionEventType {
    REGION_INTRUSION_ENTER = 1,
    REGION_INTRUSION_LEAVE = 2
} RegionIntrusionEventType;

typedef struct RegionIntrusionEvent {
    unsigned int region_id;
    unsigned int track_id; /* 本模块临时ID，不是人员身份或越线模块ID。 */
    RegionIntrusionEventType type;
    unsigned long long sequence;
    unsigned long long frame_pts_us;
    int point_x;
    int point_y;
} RegionIntrusionEvent;

/* 回调在规则线程执行，只应快速入队，不能直接阻塞播放音频。 */
typedef void (*RegionIntrusionEventCallback)(
    void *opaque, const RegionIntrusionEvent *event);

typedef struct RegionIntrusionConfig {
    NpuDetectorContext *detector;
    int model_width;
    int model_height;
    unsigned int region_id;
    unsigned int point_count;
    RegionPoint points[REGION_INTRUSION_MAX_POINTS];
    /* 漏检或序号跳跃会中断连续快照确认计数，但保留已确认的在内/在外状态。 */
    unsigned int enter_confirm_snapshots;
    unsigned int leave_confirm_snapshots;
    unsigned int match_distance_pixels;
    unsigned int max_missing_snapshots;
    unsigned int max_tracks;
    unsigned int poll_interval_ms;
    unsigned int stale_timeout_ms;
    RegionIntrusionEventCallback event_callback;
    void *event_callback_opaque;
} RegionIntrusionConfig;

typedef struct RegionIntrusionContext RegionIntrusionContext;

/* 单实例监控一个简单多边形，允许凹区域，拒绝自交/退化配置。 */
RegionIntrusionContext *region_intrusion_create(const RegionIntrusionConfig *config);
int region_intrusion_start(RegionIntrusionContext *context);
/* 必须先停止区域线程，再销毁它依赖的NPU detector。 */
int region_intrusion_stop(RegionIntrusionContext *context);
void region_intrusion_destroy(RegionIntrusionContext *context);

#endif
