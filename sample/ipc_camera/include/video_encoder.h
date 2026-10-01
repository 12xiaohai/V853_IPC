#ifndef IPC_CAMERA_VIDEO_ENCODER_H
#define IPC_CAMERA_VIDEO_ENCODER_H

#include <stddef.h>

#include <media/mm_comm_venc.h>
#include <media/mm_comm_video.h>

typedef struct VideoEncoderContext VideoEncoderContext;

/*
 * VENC 取到一帧 H.264 后调用此函数。
 * stream描述完整的一帧及其pack；回调必须在返回前深拷贝或同步消费，
 * 因为随后的 AW_MPI_VENC_ReleaseStream() 会把这些缓冲区归还VENC。
 */
typedef int (*VideoEncoderFrameCallback)(void *opaque,
                                         const unsigned char *header,
                                         size_t header_size,
                                         const VENC_STREAM_S *stream,
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

/*
 * 返回编码器内部保存的 SPS/PPS。该指针在 video_encoder_stop() 前有效，
 * 调用者如果需要长期保存，应立即复制一份。
 */
int video_encoder_get_h264_header(const VideoEncoderContext *encoder,
                                  const unsigned char **data,
                                  size_t *size);

/* 请求编码器尽快产生IDR帧，供新录像文件建立随机访问起点。 */
int video_encoder_request_key_frame(VideoEncoderContext *encoder);

#endif
