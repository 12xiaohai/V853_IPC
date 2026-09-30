#ifndef IPC_CAMERA_CONTEXT_H
#define IPC_CAMERA_CONTEXT_H

#include <pthread.h>
#include <stdint.h>

#include <media/mm_comm_vi.h>

struct VideoDisplayContext;

typedef struct VideoCaptureContext {
    VI_DEV device;
    ISP_DEV isp_device;
    VI_CHN channel;
    int width;
    int height;
    int frame_rate;
    int timeout_ms;
    PIXEL_FORMAT_E pixel_format;
    struct VideoDisplayContext *display;

    pthread_t thread_id;
    volatile int stop_requested;
    uint64_t frame_count;

    int vipp_created;
    int isp_running;
    int vipp_enabled;
    int channel_created;
    int channel_enabled;
    int thread_started;
} VideoCaptureContext;

typedef struct IpCameraContext {
    int initialized;
    VideoCaptureContext video_capture;
} IpCameraContext;

#endif
