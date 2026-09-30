#ifndef IPC_CAMERA_RTSP_STREAM_H
#define IPC_CAMERA_RTSP_STREAM_H

#include <stddef.h>
#include <stdint.h>

#include "rtsp_server.h"

typedef struct RtspStreamContext RtspStreamContext;

typedef struct RtspStreamConfig {
    int session_id;                 /* RTSP 通道号，0 对应 URL /ch0。 */
    RtspNetType net_type;           /* 绑定的网卡，本项目使用 wlan0。 */
    int frame_rate;                 /* 告诉 RTSP 客户端的视频帧率。 */
    unsigned int queue_capacity;    /* 有界队列容量，防止网络慢时无限吃内存。 */
} RtspStreamConfig;

/* 生命周期：create -> start -> push... -> stop -> destroy。 */
RtspStreamContext *rtsp_stream_create(const RtspStreamConfig *config);
int rtsp_stream_start(RtspStreamContext *context);
/* 深拷贝 VENC 的一帧码流并放入环形队列。 */
int rtsp_stream_push_h264(RtspStreamContext *context,
                          const unsigned char *header,
                          size_t header_size,
                          const unsigned char *data0,
                          size_t size0,
                          const unsigned char *data1,
                          size_t size1,
                          const unsigned char *data2,
                          size_t size2,
                          uint64_t pts,
                          int key_frame);
int rtsp_stream_stop(RtspStreamContext *context);
void rtsp_stream_destroy(RtspStreamContext *context);

#endif
