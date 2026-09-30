#ifndef IPC_CAMERA_VIDEO_ENCODER_H
#define IPC_CAMERA_VIDEO_ENCODER_H

#include <stddef.h>

#include <media/mm_comm_video.h>

typedef struct VideoEncoderContext VideoEncoderContext;

/*
 * VENC 取到一帧 H.264 后调用此函数。
 * data0/1/2 是同一帧可能分成的三段码流；回调必须在返回前深拷贝，
 * 因为随后的 AW_MPI_VENC_ReleaseStream() 会把这些缓冲区归还 VENC。
 */
typedef int (*VideoEncoderFrameCallback)(
    void *opaque,
    const unsigned char *header,
    size_t header_size,
    const unsigned char *data0,
    size_t size0,
    const unsigned char *data1,
    size_t size1,
    const unsigned char *data2,
    size_t size2,
    unsigned long long pts,
    int key_frame);

typedef struct VideoEncoderConfig {
    int vi_device;                  /* 编码专用 VIPP，当前为 0。 */
    int isp_device;
    int vi_channel;
    int channel;                    /* VENC 通道号。 */
    int width;
    int height;
    int frame_rate;
    int bit_rate;                   /* 目标码率，单位 bit/s。 */
    int gop_size;                   /* 两个关键帧之间的帧数。 */
    PIXEL_FORMAT_E pixel_format;
    const char *output_path;        /* 原始 Annex-B H.264 对照文件。 */
    VideoEncoderFrameCallback frame_callback; /* 可选的 RTSP 码流消费者。 */
    void *frame_callback_opaque;     /* 原样传回回调的用户指针。 */
} VideoEncoderConfig;

VideoEncoderContext *video_encoder_create(const VideoEncoderConfig *config);
int video_encoder_start(VideoEncoderContext *encoder);
int video_encoder_stop(VideoEncoderContext *encoder);
void video_encoder_destroy(VideoEncoderContext *encoder);

#endif
