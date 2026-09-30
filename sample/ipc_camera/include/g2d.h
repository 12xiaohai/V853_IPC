#ifndef IPC_CAMERA_G2D_H
#define IPC_CAMERA_G2D_H

#include <media/mm_comm_video.h>

typedef struct G2dContext {
    int fd;                 /* /dev/g2d 文件描述符，-1 表示未打开。 */
    int rotation;           /* 顺时针旋转角度：0/90/180/270。 */
    int source_width;
    int source_height;
    int destination_width; /* 90/270 度旋转后宽高交换。 */
    int destination_height;
} G2dContext;

/* 打开 G2D 硬件并根据旋转角度计算输出尺寸。 */
int g2d_open(G2dContext *context,
             int source_width,
             int source_height,
             int rotation);
/* 通过物理地址完成一帧硬件旋转，不由 CPU 逐像素拷贝。 */
int g2d_convert_frame(G2dContext *context,
                      const VIDEO_FRAME_INFO_S *source,
                      VIDEO_FRAME_INFO_S *destination);
void g2d_close(G2dContext *context); /* 关闭 /dev/g2d。 */

#endif
