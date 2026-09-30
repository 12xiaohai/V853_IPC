#ifndef IPC_CAMERA_VIDEO_DISPLAY_H
#define IPC_CAMERA_VIDEO_DISPLAY_H

#include <media/mm_comm_video.h>

typedef struct VideoDisplayContext VideoDisplayContext;

typedef struct VideoDisplayConfig {
    int source_width;
    int source_height;
    PIXEL_FORMAT_E pixel_format;
    int rotation;
    int display_x;
    int display_y;
    int display_width;
    int display_height;
} VideoDisplayConfig;

VideoDisplayContext *video_display_create(const VideoDisplayConfig *config);
int video_display_start(VideoDisplayContext *display);
int video_display_submit(VideoDisplayContext *display,
                         const VIDEO_FRAME_INFO_S *source);
int video_display_stop(VideoDisplayContext *display);
void video_display_destroy(VideoDisplayContext *display);

#endif
