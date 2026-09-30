#ifndef IPC_CAMERA_VIDEO_ENCODER_H
#define IPC_CAMERA_VIDEO_ENCODER_H

#include <stddef.h>

#include <media/mm_comm_video.h>

typedef struct VideoEncoderContext VideoEncoderContext;

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
    int vi_device;
    int isp_device;
    int vi_channel;
    int channel;
    int width;
    int height;
    int frame_rate;
    int bit_rate;
    int gop_size;
    PIXEL_FORMAT_E pixel_format;
    const char *output_path;
    VideoEncoderFrameCallback frame_callback;
    void *frame_callback_opaque;
} VideoEncoderConfig;

VideoEncoderContext *video_encoder_create(const VideoEncoderConfig *config);
int video_encoder_start(VideoEncoderContext *encoder);
int video_encoder_stop(VideoEncoderContext *encoder);
void video_encoder_destroy(VideoEncoderContext *encoder);

#endif
