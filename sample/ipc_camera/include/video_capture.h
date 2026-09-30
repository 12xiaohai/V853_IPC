#ifndef IPC_CAMERA_VIDEO_CAPTURE_H
#define IPC_CAMERA_VIDEO_CAPTURE_H

#include "context.h"

/* 启动顺序：VIPP -> ISP -> VI 虚拟通道 -> 采集线程。 */
int video_capture_start(VideoCaptureContext *capture);
/* 先结束线程，再按与启动相反的顺序销毁资源。 */
int video_capture_stop(VideoCaptureContext *capture);

#endif
