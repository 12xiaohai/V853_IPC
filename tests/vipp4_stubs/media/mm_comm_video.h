#ifndef VIPP4_TEST_VIDEO_H
#define VIPP4_TEST_VIDEO_H
#include <stdint.h>
/* 主机替身只描述采集测试使用的字段；真实SDK类型另做ARM语法检查。 */
typedef int PIXEL_FORMAT_E;
typedef struct VIDEO_FRAME_INFO_S {
    unsigned int mId;
    struct { unsigned int mWidth, mHeight; uint64_t mpts; } VFrame;
} VIDEO_FRAME_INFO_S;
#endif
