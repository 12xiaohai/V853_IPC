#ifndef IPC_CAMERA_NPU_DETECTOR_H
#define IPC_CAMERA_NPU_DETECTOR_H

#include "yolov8_postprocess.h"

/*
 * 实时检测模块的启动参数。
 *
 * 预览、编码和 NPU 各自使用独立 VIPP：
 *   - VIPP 4：LCD 预览；
 *   - VIPP 0：H.264 编码；
 *   - VIPP 8：320x320 NPU 输入。
 * 三条通路共享同一个摄像头和 ISP，但不会互相争抢同一个 VI 虚拟通道。
 */
typedef struct NpuDetectorConfig {
    int vi_device;
    int isp_device;
    int vi_channel;
    int width;
    int height;
    int frame_rate;
    int buffer_count;
    int timeout_ms;
    const char *model_path;
    /*
     * 可选的一次性调试抓帧路径。非NULL时保存第一张真正送入NPU的NV12帧；
     * 只用于板端联调，确认后可在main.c中设为NULL关闭。
     */
    const char *debug_dump_path;
    /* 第几个有效推理帧执行抓图；小于1时按第1帧处理。 */
    unsigned int debug_dump_after_frames;
    float confidence_threshold;
    float nms_threshold;
    unsigned int log_interval_frames;
} NpuDetectorConfig;

/*
 * 一次检测结果的只读快照。
 * sequence 每完成一次推理加 1；frame_pts_us 来自原始 VI 帧，可供后续跟踪、
 * 越线检测和录像事件对齐使用。
 */
typedef struct NpuDetectionSnapshot {
    unsigned long long sequence;
    unsigned long long frame_pts_us;
    unsigned long long inference_time_us;
    int detection_count;
    YoloDetection detections[YOLOV8_MAX_DETECTIONS];
} NpuDetectionSnapshot;

typedef struct NpuDetectorContext NpuDetectorContext;

/* 只分配并保存配置，不接触硬件。 */
NpuDetectorContext *npu_detector_create(const NpuDetectorConfig *config);

/* 创建低分辨率 VI 通路、加载模型，并启动实时推理线程。 */
int npu_detector_start(NpuDetectorContext *detector);

/*
 * 将最新结果复制给调用者。此函数不会返回内部指针，因此调用者不需要长期持锁。
 * 尚未完成第一帧推理时也返回 0，此时 snapshot->sequence 为 0。
 */
int npu_detector_get_latest(NpuDetectorContext *detector,
                            NpuDetectionSnapshot *snapshot);

/* 先结束推理线程，再按启动的逆序释放 NPU、VI 和 ISP 资源。 */
int npu_detector_stop(NpuDetectorContext *detector);

/* 释放上下文；即使调用者忘记 stop，也会先执行安全停止。 */
void npu_detector_destroy(NpuDetectorContext *detector);

#endif
