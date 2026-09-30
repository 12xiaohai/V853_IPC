#ifndef IPC_CAMERA_VIDEO_DISPLAY_H
#define IPC_CAMERA_VIDEO_DISPLAY_H

#include <media/mm_comm_video.h>

typedef struct VideoDisplayContext VideoDisplayContext;

typedef struct VideoDisplayConfig {
    int source_width;           /* VI 输入宽度。 */
    int source_height;          /* VI 输入高度。 */
    PIXEL_FORMAT_E pixel_format;/* G2D 输入/输出格式，当前为 NV21。 */
    int rotation;               /* 送给 LCD 前由 G2D 旋转的角度。 */
    int display_x;              /* LCD 显示矩形左上角 X。 */
    int display_y;              /* LCD 显示矩形左上角 Y。 */
    int display_width;          /* LCD 上的显示宽度。 */
    int display_height;         /* LCD 上的显示高度。 */
} VideoDisplayConfig;

/* create/destroy 只管理应用上下文，start/stop 管理 G2D、MMZ 和 VO 资源。 */
VideoDisplayContext *video_display_create(const VideoDisplayConfig *config);
int video_display_start(VideoDisplayContext *display);
/* 将一帧 VI 图像旋转到 MMZ 帧池，再异步送入 VO。 */
int video_display_submit(VideoDisplayContext *display,
                         const VIDEO_FRAME_INFO_S *source);
int video_display_stop(VideoDisplayContext *display);
void video_display_destroy(VideoDisplayContext *display);

#endif
