#ifndef VIPP4_TEST_VI_H
#define VIPP4_TEST_VI_H
#include "mm_comm_video.h"
typedef int VI_DEV;
typedef int VI_CHN;
typedef int ISP_DEV;
typedef int ERRORTYPE;
#define SUCCESS 0
#define V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE 9
#define V4L2_MEMORY_MMAP 1
#define V4L2_FIELD_NONE 1
#define V4L2_COLORSPACE_JPEG 7
#define V4L2_MODE_VIDEO 2
typedef struct VI_ATTR_S {
    int type, memtype;
    struct { int pixelformat, field, colorspace, width, height; } format;
    int nbufs, nplanes, fps, capturemode, use_current_win, wdr_mode;
    int drop_frame_num;
} VI_ATTR_S;
#endif
