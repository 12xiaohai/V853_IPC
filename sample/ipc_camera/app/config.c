#include "config.h"

#include <stdlib.h>

#include <utils/plat_log.h>

/*
 * 为整个应用分配一份零初始化的上下文。
 * calloc 与 malloc 的区别是：calloc 会把内存全部清 0，因此各种
 * "xxx_created/started" 状态标志初始都是未创建，便于失败时安全回滚。
 */
IpCameraContext *constructIpCameraContext(void)
{
    IpCameraContext *context = calloc(1, sizeof(*context));

    if (context == NULL) {
        aloge("fatal error! allocate IP camera context failed");
        return NULL;
    }

    /*
     * 实时预览沿用原项目的 VIPP 4。VIPP 可理解为 VI 的一条图像管线，
     * 它从 ISP 0 取得 GC2053 摄像头的数据。NV21 是 Y 平面 + VU 交错平面。
     */
    context->video_capture.device = 4;
    context->video_capture.isp_device = 0;
    context->video_capture.channel = 0;
    context->video_capture.width = 1920;
    context->video_capture.height = 1080;
    context->video_capture.frame_rate = 20;
    context->video_capture.timeout_ms = 200;
    context->video_capture.pixel_format = MM_PIXEL_FORMAT_YVU_SEMIPLANAR_420;

    return context;
}

void destructIpCameraContext(IpCameraContext *context)
{
    /* free(NULL) 在 C 语言中是安全的，因此无需额外判空。 */
    free(context);
}
