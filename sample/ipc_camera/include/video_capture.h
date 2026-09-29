#ifndef IPC_CAMERA_VIDEO_CAPTURE_H
#define IPC_CAMERA_VIDEO_CAPTURE_H

#include "context.h"

int video_capture_start(VideoCaptureContext *capture);
int video_capture_stop(VideoCaptureContext *capture);

#endif
