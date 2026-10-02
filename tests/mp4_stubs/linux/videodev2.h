#ifndef MP4_TEST_VIDEODEV2_H
#define MP4_TEST_VIDEODEV2_H
#ifdef _WIN32
/* MUX测试只需要mm_comm_venc.h中颜色空间字段的类型，不测试V4L2。 */
enum v4l2_colorspace { V4L2_COLORSPACE_DEFAULT = 0 };
#else
#include_next <linux/videodev2.h>
#endif
#endif
