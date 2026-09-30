#ifndef IPC_CAMERA_RTSP_STREAM_H
#define IPC_CAMERA_RTSP_STREAM_H

#include <stddef.h>
#include <stdint.h>

#include "rtsp_server.h"

typedef struct RtspStreamContext RtspStreamContext;

typedef struct RtspStreamConfig {
    int session_id;
    RtspNetType net_type;
    int frame_rate;
    unsigned int queue_capacity;
} RtspStreamConfig;

RtspStreamContext *rtsp_stream_create(const RtspStreamConfig *config);
int rtsp_stream_start(RtspStreamContext *context);
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
