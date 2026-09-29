#include "config.h"

#include <stdlib.h>

#include <utils/plat_log.h>

IpCameraContext *constructIpCameraContext(void)
{
    IpCameraContext *context = calloc(1, sizeof(*context));

    if (context == NULL) {
        aloge("fatal error! allocate IP camera context failed");
        return NULL;
    }

    /* Keep the first reconstructed VI path consistent with the original
     * real-time preview path. These values can be moved to a configuration
     * file after the capture path has been verified on the board. */
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
    free(context);
}
