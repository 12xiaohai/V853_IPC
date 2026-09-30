#ifndef SAMPLE_DEMO_RTSP_SERVER_H
#define SAMPLE_DEMO_RTSP_SERVER_H

#include <stdint.h>

#ifdef __cplusplus
/* 让 C 模块可以调用由 C++ 实现的 TinyServer 封装。 */
extern "C" {
#endif

typedef enum RtspNetType {
    RTSP_NET_TYPE_LO = 0, /* 本机回环，只能在板端自测。 */
    RTSP_NET_TYPE_ETH0,   /* 有线网卡。 */
    RTSP_NET_TYPE_BR0,    /* Linux 网桥。 */
    RTSP_NET_TYPE_WLAN0   /* Wi-Fi 网卡。 */
} RtspNetType;

typedef enum RtspFrameType {
    RTSP_FRAME_TYPE_I = 0,
    RTSP_FRAME_TYPE_P
} RtspFrameType;

typedef struct RtspServerConfig {
    RtspNetType net_type;
    int frame_rate;
} RtspServerConfig;

/* open 创建服务器和 chN 媒体流，start 启动 RTSP 事件线程。 */
int rtsp_server_open(int id, const RtspServerConfig *config);
int rtsp_server_start(int id);
int rtsp_server_send_video(int id,
                           unsigned char *data,
                           unsigned int size,
                           uint64_t pts,
                           RtspFrameType frame_type);
/* 向同一个媒体会话追加一帧带 ADTS 头的 AAC 数据。 */
int rtsp_server_send_audio(int id,
                           unsigned char *data,
                           unsigned int size,
                           uint64_t pts);
void rtsp_server_stop(int id);
void rtsp_server_close(int id);

#ifdef __cplusplus
}
#endif

#endif
