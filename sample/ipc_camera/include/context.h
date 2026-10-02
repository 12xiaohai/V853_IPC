#ifndef IPC_CAMERA_CONTEXT_H
#define IPC_CAMERA_CONTEXT_H

#include <pthread.h>
#include <stdint.h>

#include <media/mm_comm_vi.h>

struct VideoDisplayContext;

/* VI（Video Input）采集通路的配置、线程和资源状态。 */
typedef struct VideoCaptureContext {
    VI_DEV device;                    /* VIPP 号：实时预览使用 VIPP 4。 */
    ISP_DEV isp_device;               /* 与摄像头相连的 ISP 设备号。 */
    VI_CHN channel;                   /* VIPP 下的虚拟通道号。 */
    int width;                        /* 采集图像宽度，单位：像素。 */
    int height;                       /* 采集图像高度，单位：像素。 */
    int frame_rate;                   /* 期望采集帧率。 */
    int timeout_ms;                   /* GetFrame 单次等待超时时间。 */
    PIXEL_FORMAT_E pixel_format;      /* 本项目使用 NV21。 */
    struct VideoDisplayContext *display; /* 可选的 G2D/VO 显示模块。 */
    int diagnostic_timing;            /* 开启单调时钟耗时统计，默认关闭。 */
    int diagnostic_capture_only;      /* 只取/还帧，跳过逐帧G2D/VO；不关闭VIPP4。 */

    pthread_t thread_id;              /* 执行 GetFrame/ReleaseFrame 的工作线程。 */
    volatile int stop_requested;      /* 主线程通知采集线程退出的标志。 */
    uint64_t frame_count;             /* 成功取得的帧数。 */

    /* 下列标志记录哪些 MPP 资源已成功创建，便于失败回滚。 */
    int vipp_created;
    int isp_running;
    int vipp_enabled;
    int channel_created;
    int channel_enabled;
    int thread_started;
} VideoCaptureContext;

typedef struct IpCameraContext {
    int initialized;                  /* 应用级上下文是否已完成基础初始化。 */
    VideoCaptureContext video_capture;/* 实时预览采集通路。 */
} IpCameraContext;

#endif
