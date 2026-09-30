#ifndef IPC_CAMERA_TIME_OSD_H
#define IPC_CAMERA_TIME_OSD_H

/* 时间水印模块对外使用不透明上下文，隐藏 RGN、位图和线程细节。 */
typedef struct TimeOsdContext TimeOsdContext;

typedef struct TimeOsdConfig {
    int venc_channel;       /* 水印要附着的 VENC 通道。 */
    unsigned int handle;    /* MPP RGN 句柄，同一进程内必须唯一。 */
    int x;                  /* 水印左上角横坐标，RGN 要求4像素对齐。 */
    int y;                  /* 水印左上角纵坐标，RGN 要求4像素对齐。 */
    unsigned int update_seconds; /* 时间位图刷新周期。 */
} TimeOsdConfig;

/* 生命周期：create -> start -> stop -> destroy。 */
TimeOsdContext *time_osd_create(const TimeOsdConfig *config);
int time_osd_start(TimeOsdContext *context);
int time_osd_stop(TimeOsdContext *context);
void time_osd_destroy(TimeOsdContext *context);

#endif
