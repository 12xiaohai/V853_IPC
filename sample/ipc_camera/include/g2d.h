#ifndef IPC_CAMERA_G2D_H
#define IPC_CAMERA_G2D_H

#include <media/mm_comm_video.h>

typedef struct G2dContext {
    int fd;
    int rotation;
    int source_width;
    int source_height;
    int destination_width;
    int destination_height;
} G2dContext;

int g2d_open(G2dContext *context,
             int source_width,
             int source_height,
             int rotation);
int g2d_convert_frame(G2dContext *context,
                      const VIDEO_FRAME_INFO_S *source,
                      VIDEO_FRAME_INFO_S *destination);
void g2d_close(G2dContext *context);

#endif
