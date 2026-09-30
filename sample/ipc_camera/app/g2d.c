#include "g2d.h"

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/g2d_driver.h>
#include <utils/PIXEL_FORMAT_E_g2d_format_convert.h>
#include <utils/plat_log.h>

/* 把容易阅读的角度转成 G2D 驱动要求的命令标志。 */
static int rotation_to_g2d_flag(int rotation)
{
    switch (rotation) {
    case 0:
        return G2D_BLT_NONE_H;
    case 90:
        return G2D_ROT_90;
    case 180:
        return G2D_ROT_180;
    case 270:
        return G2D_ROT_270;
    default:
        return -1;
    }
}

int g2d_open(G2dContext *context,
             int source_width,
             int source_height,
             int rotation)
{
    if (context == NULL || source_width <= 0 || source_height <= 0 ||
        rotation_to_g2d_flag(rotation) < 0) {
        aloge("[G2D] Invalid conversion configuration");
        return -1;
    }

    memset(context, 0, sizeof(*context));
    context->fd = -1;
    context->rotation = rotation;
    context->source_width = source_width;
    context->source_height = source_height;

    /* 图像旋转 90/270 度后，输出图的宽和高交换。 */
    if (rotation == 90 || rotation == 270) {
        context->destination_width = source_height;
        context->destination_height = source_width;
    } else {
        context->destination_width = source_width;
        context->destination_height = source_height;
    }

    /* G2D 由内核字符设备 /dev/g2d 提供 ioctl 接口。 */
    context->fd = open("/dev/g2d", O_RDWR);
    if (context->fd < 0) {
        aloge("[G2D] Open /dev/g2d failed");
        return -1;
    }

    alogd("[G2D] Opened: %dx%d -> %dx%d, rotation=%d",
          context->source_width,
          context->source_height,
          context->destination_width,
          context->destination_height,
          context->rotation);
    return 0;
}

int g2d_convert_frame(G2dContext *context,
                      const VIDEO_FRAME_INFO_S *source,
                      VIDEO_FRAME_INFO_S *destination)
{
    g2d_blt_h blit;
    g2d_fmt_enh source_format;
    g2d_fmt_enh destination_format;
    int flag;
    int ret;

    if (context == NULL || context->fd < 0 || source == NULL ||
        destination == NULL) {
        return -1;
    }

    /* 尺寸不匹配可能导致 G2D 越界访问物理内存，所以必须先拒绝。 */
    if ((int)source->VFrame.mWidth != context->source_width ||
        (int)source->VFrame.mHeight != context->source_height ||
        (int)destination->VFrame.mWidth != context->destination_width ||
        (int)destination->VFrame.mHeight != context->destination_height) {
        aloge("[G2D] Frame size does not match conversion configuration");
        return -1;
    }

    ret = convert_PIXEL_FORMAT_E_to_g2d_fmt_enh(
        source->VFrame.mPixelFormat, &source_format);
    if (ret != SUCCESS) {
        aloge("[G2D] Unsupported source format: 0x%x",
              source->VFrame.mPixelFormat);
        return -1;
    }

    ret = convert_PIXEL_FORMAT_E_to_g2d_fmt_enh(
        destination->VFrame.mPixelFormat, &destination_format);
    if (ret != SUCCESS) {
        aloge("[G2D] Unsupported destination format: 0x%x",
              destination->VFrame.mPixelFormat);
        return -1;
    }

    flag = rotation_to_g2d_flag(context->rotation);
    if (flag < 0) {
        return -1;
    }

    /* g2d_blt_h 描述一次硬件 BitBLT：旋转标志、源图和目标图。 */
    memset(&blit, 0, sizeof(blit));
    blit.flag_h = flag;

    blit.src_image_h.format = source_format;
    /* NV21 主要使用 0(Y) 和 1(VU) 两个平面的物理地址。 */
    blit.src_image_h.laddr[0] = source->VFrame.mPhyAddr[0];
    blit.src_image_h.laddr[1] = source->VFrame.mPhyAddr[1];
    blit.src_image_h.laddr[2] = source->VFrame.mPhyAddr[2];
    blit.src_image_h.width = source->VFrame.mWidth;
    blit.src_image_h.height = source->VFrame.mHeight;
    blit.src_image_h.clip_rect.x = 0;
    blit.src_image_h.clip_rect.y = 0;
    blit.src_image_h.clip_rect.w = source->VFrame.mWidth;
    blit.src_image_h.clip_rect.h = source->VFrame.mHeight;
    blit.src_image_h.gamut = G2D_BT601;
    blit.src_image_h.mode = G2D_PIXEL_ALPHA;
    blit.src_image_h.fd = -1;
    blit.src_image_h.use_phy_addr = 1;

    blit.dst_image_h.format = destination_format;
    blit.dst_image_h.laddr[0] = destination->VFrame.mPhyAddr[0];
    blit.dst_image_h.laddr[1] = destination->VFrame.mPhyAddr[1];
    blit.dst_image_h.laddr[2] = destination->VFrame.mPhyAddr[2];
    blit.dst_image_h.width = destination->VFrame.mWidth;
    blit.dst_image_h.height = destination->VFrame.mHeight;
    blit.dst_image_h.clip_rect.x = 0;
    blit.dst_image_h.clip_rect.y = 0;
    blit.dst_image_h.clip_rect.w = destination->VFrame.mWidth;
    blit.dst_image_h.clip_rect.h = destination->VFrame.mHeight;
    blit.dst_image_h.gamut = G2D_BT601;
    blit.dst_image_h.mode = G2D_PIXEL_ALPHA;
    blit.dst_image_h.fd = -1;
    blit.dst_image_h.use_phy_addr = 1;

    /* ioctl 返回时硬件旋转已完成，源 VI 帧随后即可归还。 */
    ret = ioctl(context->fd, G2D_CMD_BITBLT_H, (unsigned long)&blit);
    if (ret < 0) {
        aloge("[G2D] BITBLT failed: ret=%d", ret);
        return -1;
    }

    destination->VFrame.mOffsetTop = 0;
    destination->VFrame.mOffsetBottom = context->destination_height;
    destination->VFrame.mOffsetLeft = 0;
    destination->VFrame.mOffsetRight = context->destination_width;
    /* 继承源帧 PTS，使 LCD 显示保留原采集时间轴。 */
    destination->VFrame.mpts = source->VFrame.mpts;
    return 0;
}

void g2d_close(G2dContext *context)
{
    if (context != NULL && context->fd >= 0) {
        close(context->fd);
        context->fd = -1;
        alogd("[G2D] Closed");
    }
}
