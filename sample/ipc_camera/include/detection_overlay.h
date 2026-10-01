#ifndef IPC_CAMERA_DETECTION_OVERLAY_H
#define IPC_CAMERA_DETECTION_OVERLAY_H

#include "npu_detector.h"

/*
 * 阶段9.3检测框叠加模块的配置。
 *
 * NPU使用320x320独立VIPP进行推理，编码通路则使用1920x1080的VIPP 0。
 * 本模块从detector读取结果快照，完成坐标变换后，用MPP ORL_RGN
 * 把矩形框附着到目标VIPP。
 */
typedef struct DetectionOverlayConfig {
    NpuDetectorContext *detector;

    int target_vi_device;
    int target_vi_channel;
    int target_width;
    int target_height;

    int model_width;
    int model_height;

    /*
     * 仅当NPU图像和目标编码画面方向不同时才启用。V853当前的
     * mirror/flip实测作用于共享sensor，VIPP 0和VIPP 8方向一致，
     * 因此main.c中保持为0。
     */
    int map_mirror;
    int map_flip;

    unsigned int region_handle_base;
    unsigned int max_regions;
    unsigned int color;
    unsigned int thickness;
    unsigned int poll_interval_ms;
    unsigned int stale_timeout_ms;
} DetectionOverlayConfig;

typedef struct DetectionOverlayContext DetectionOverlayContext;

/* 仅分配上下文，不创建MPP region。 */
DetectionOverlayContext *detection_overlay_create(
    const DetectionOverlayConfig *config);

/* 启动结果读取线程，在新快照到来时刷新ORL框。 */
int detection_overlay_start(DetectionOverlayContext *context);

/* 停止线程，并从VIPP拆除所有检测框。 */
int detection_overlay_stop(DetectionOverlayContext *context);

/* 释放上下文；如果忘记stop，内部会先停止。 */
void detection_overlay_destroy(DetectionOverlayContext *context);

#endif
