#ifndef SAMPLE_DEMO_RTSP_SERVER_H
#define SAMPLE_DEMO_RTSP_SERVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RtspNetType {
    RTSP_NET_TYPE_LO = 0,
    RTSP_NET_TYPE_ETH0,
    RTSP_NET_TYPE_BR0,
    RTSP_NET_TYPE_WLAN0
} RtspNetType;

typedef enum RtspFrameType {
    RTSP_FRAME_TYPE_I = 0,
    RTSP_FRAME_TYPE_P
} RtspFrameType;

typedef struct RtspServerConfig {
    RtspNetType net_type;
    int frame_rate;
} RtspServerConfig;

int rtsp_server_open(int id, const RtspServerConfig *config);
int rtsp_server_start(int id);
int rtsp_server_send_video(int id,
                           unsigned char *data,
                           unsigned int size,
                           uint64_t pts,
                           RtspFrameType frame_type);
void rtsp_server_stop(int id);
void rtsp_server_close(int id);

#ifdef __cplusplus
}
#endif

#endif
