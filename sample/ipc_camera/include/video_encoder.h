#ifndef IPC_CAMERA_VIDEO_ENCODER_H
#define IPC_CAMERA_VIDEO_ENCODER_H

#include <media/mm_comm_video.h>

typedef struct VideoEncoderContext VideoEncoderContext;

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
} VideoEncoderConfig;

VideoEncoderContext *video_encoder_create(const VideoEncoderConfig *config);
int video_encoder_start(VideoEncoderContext *encoder);
int video_encoder_stop(VideoEncoderContext *encoder);
void video_encoder_destroy(VideoEncoderContext *encoder);

#endif
