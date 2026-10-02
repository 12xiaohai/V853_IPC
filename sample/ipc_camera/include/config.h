#ifndef IPC_CAMERA_CONFIG_H
#define IPC_CAMERA_CONFIG_H

#include <media/mm_common.h>
#include <media/mm_comm_video.h>

#include "audio_alarm.h"
#include "audio_encoder.h"
#include "detection_overlay.h"
#include "line_crossing.h"
#include "mp4_recorder.h"
#include "npu_detector.h"
#include "region_intrusion.h"
#include "rtsp_stream.h"
#include "time_osd.h"
#include "video_display.h"
#include "video_encoder.h"

#define IPC_CAMERA_DEFAULT_MODEL "/lib/yolov8n.nb"
#define IPC_CAMERA_DEFAULT_ALARM "/lib/alarm.wav"

/* 运行模式与媒体资源无关：自检不应意外启动完整监控链路。 */
typedef enum IpCameraRunMode {
    IPC_CAMERA_MONITOR = 0,
    IPC_CAMERA_NPU_SELF_TEST,
    IPC_CAMERA_AUDIO_ALARM_TEST
} IpCameraRunMode;

typedef struct IpCameraOptions {
    IpCameraRunMode mode;
    const char *model_path; /* 借用argv或静态常量，生命周期覆盖应用运行。 */
    const char *input_path;
    const char *alarm_path;
} IpCameraOptions;

/* 这里只保存启动参数，不混入线程ID、缓冲计数或资源创建状态。 */
typedef struct VideoCaptureConfig {
    VI_DEV device;
    ISP_DEV isp_device;
    VI_CHN channel;
    int width;
    int height;
    int frame_rate;
    int timeout_ms;
    PIXEL_FORMAT_E pixel_format;
} VideoCaptureConfig;

/*
 * 应用默认配置的唯一入口。模块配置中的回调/服务指针默认为空；
 * application在创建真实服务后再接线，config不拥有或启动任何资源。
 */
typedef struct IpCameraConfig {
    VideoCaptureConfig preview;
    VideoDisplayConfig display;
    RtspStreamConfig rtsp;
    VideoEncoderConfig video_encoder;
    TimeOsdConfig time_osd;
    Mp4RecorderConfig recorder;
    AudioEncoderConfig audio_encoder;
    NpuDetectorConfig npu;
    DetectionOverlayConfig overlay;
    DetectionOverlayConfig lcd_overlay;
    AudioAlarmConfig alarm;
    LineCrossingConfig line;
    RegionIntrusionConfig region;
} IpCameraConfig;

/* 0表示合法；未知选项、缺少参数、空路径或多余参数均返回-1。 */
int ip_camera_options_parse(int argc, char *argv[], IpCameraOptions *options);
void ip_camera_config_defaults(IpCameraConfig *config);

#endif
