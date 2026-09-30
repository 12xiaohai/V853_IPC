#include "video_capture.h"

#include <string.h>

#include <media/mpi_isp.h>
#include <media/mpi_vi.h>
#include <media/mpi_videoformat_conversion.h>
#include <utils/plat_log.h>

#include "video_display.h"

/*
 * VI 采集线程：循环从 VIPP 虚拟通道取帧，交给显示模块处理，
 * 然后立即归还 VI 帧。GetFrame 成功后的每一条路径都必须 ReleaseFrame。
 */
static void *video_capture_thread(void *argument)
{
    VideoCaptureContext *capture = argument;
    unsigned int consecutive_failures = 0;

    alogd("[VI] Capture thread started");

    while (!capture->stop_requested) {
        VIDEO_FRAME_INFO_S frame;
        ERRORTYPE ret;

        memset(&frame, 0, sizeof(frame));
        /* 带超时取帧，使 stop_requested 可以在有限时间内被检查。 */
        ret = AW_MPI_VI_GetFrame(capture->device,
                                 capture->channel,
                                 &frame,
                                 capture->timeout_ms);
        if (ret != SUCCESS) {
            if (capture->stop_requested) {
                break;
            }

            /* 只记录首次和每 25 次连续失败，避免硬件异常时刷屏。 */
            ++consecutive_failures;
            if (consecutive_failures == 1 ||
                (consecutive_failures % 25U) == 0U) {
                alogw("[VI] GetFrame failed: ret=%d, consecutive=%u",
                      ret, consecutive_failures);
            }
            continue;
        }

        consecutive_failures = 0;
        ++capture->frame_count;

        if (capture->frame_count == 1U ||
            (capture->frame_count % 100U) == 0U) {
            alogd("[VI] Frame=%llu, id=%u, size=%ux%u, pts=%llu us",
                  (unsigned long long)capture->frame_count,
                  frame.mId,
                  frame.VFrame.mWidth,
                  frame.VFrame.mHeight,
                  (unsigned long long)frame.VFrame.mpts);
        }

        /* display 可为 NULL，因此 VI 模块也能独立做采集测试。 */
        if (capture->display != NULL &&
            video_display_submit(capture->display, &frame) < 0) {
            alogw("[VI] Display processing failed for frame=%llu",
                  (unsigned long long)capture->frame_count);
        }

        /* 显示模块已把像素旋转到自己的 MMZ 帧，所以现在可归还 VI 源帧。 */
        ret = AW_MPI_VI_ReleaseFrame(capture->device,
                                     capture->channel,
                                     &frame);
        if (ret != SUCCESS) {
            aloge("[VI] ReleaseFrame failed: ret=%d, frame_id=%u",
                  ret, frame.mId);
        }
    }

    alogd("[VI] Capture thread stopped, total frames=%llu",
          (unsigned long long)capture->frame_count);
    return NULL;
}

/*
 * 按启动的反顺序销毁 VI 通路。每个状态标志使该函数既能处理
 * 完整停止，也能处理“初始化进行到一半就失败”的回滚。
 */
static int video_capture_destroy_pipeline(VideoCaptureContext *capture)
{
    int result = 0;
    ERRORTYPE ret;

    if (capture->channel_enabled) {
        ret = AW_MPI_VI_DisableVirChn(capture->device, capture->channel);
        if (ret != SUCCESS) {
            aloge("[VI] DisableVirChn failed: ret=%d", ret);
            result = -1;
        }
        capture->channel_enabled = 0;
    }

    if (capture->channel_created) {
        ret = AW_MPI_VI_DestroyVirChn(capture->device, capture->channel);
        if (ret != SUCCESS) {
            aloge("[VI] DestroyVirChn failed: ret=%d", ret);
            result = -1;
        }
        capture->channel_created = 0;
    }

    if (capture->vipp_enabled) {
        ret = AW_MPI_VI_DisableVipp(capture->device);
        if (ret != SUCCESS) {
            aloge("[VI] DisableVipp failed: ret=%d", ret);
            result = -1;
        }
        capture->vipp_enabled = 0;
    }

    if (capture->isp_running) {
        ret = AW_MPI_ISP_Stop(capture->isp_device);
        if (ret != SUCCESS) {
            aloge("[VI] ISP stop failed: ret=%d", ret);
            result = -1;
        }
        capture->isp_running = 0;
    }

    if (capture->vipp_created) {
        ret = AW_MPI_VI_DestroyVipp(capture->device);
        if (ret != SUCCESS) {
            aloge("[VI] DestroyVipp failed: ret=%d", ret);
            result = -1;
        }
        capture->vipp_created = 0;
    }

    return result;
}

int video_capture_start(VideoCaptureContext *capture)
{
    VI_ATTR_S attributes;
    ERRORTYPE ret;

    if (capture == NULL || capture->width <= 0 || capture->height <= 0 ||
        capture->frame_rate <= 0 || capture->timeout_ms < 0) {
        aloge("[VI] Invalid capture configuration");
        return -1;
    }

    /* 使用 V4L2 多平面 + MMAP 缓冲，NV21 共 Y/VU 两个平面。 */
    memset(&attributes, 0, sizeof(attributes));
    attributes.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    attributes.memtype = V4L2_MEMORY_MMAP;
    attributes.format.pixelformat =
        map_PIXEL_FORMAT_E_to_V4L2_PIX_FMT(capture->pixel_format);
    attributes.format.field = V4L2_FIELD_NONE;
    attributes.format.colorspace = V4L2_COLORSPACE_JPEG;
    attributes.format.width = capture->width;
    attributes.format.height = capture->height;
    /* 5 个采集缓冲可以吸收短时处理抖动，同时限制内存占用。 */
    attributes.nbufs = 5;
    attributes.nplanes = 2;
    attributes.fps = capture->frame_rate;
    attributes.capturemode = V4L2_MODE_VIDEO;
    attributes.use_current_win = 0;
    attributes.wdr_mode = 0;
    attributes.drop_frame_num = 0;

    capture->stop_requested = 0;
    capture->frame_count = 0;

    alogd("[VI] Starting: vipp=%d, isp=%d, chn=%d, %dx%d@%dfps",
          capture->device,
          capture->isp_device,
          capture->channel,
          capture->width,
          capture->height,
          capture->frame_rate);

    /* 创建顺序不能随意调换：先 VIPP，再 ISP，然后使能并创建通道。 */
    ret = AW_MPI_VI_CreateVipp(capture->device);
    if (ret != SUCCESS) {
        aloge("[VI] CreateVipp failed: ret=%d", ret);
        return -1;
    }
    capture->vipp_created = 1;

    ret = AW_MPI_VI_SetVippAttr(capture->device, &attributes);
    if (ret != SUCCESS) {
        aloge("[VI] SetVippAttr failed: ret=%d", ret);
        goto error;
    }

    ret = AW_MPI_ISP_Run(capture->isp_device);
    if (ret != SUCCESS) {
        aloge("[VI] ISP run failed: ret=%d", ret);
        goto error;
    }
    capture->isp_running = 1;

    ret = AW_MPI_VI_EnableVipp(capture->device);
    if (ret != SUCCESS) {
        aloge("[VI] EnableVipp failed: ret=%d", ret);
        goto error;
    }
    capture->vipp_enabled = 1;

    ret = AW_MPI_VI_CreateVirChn(capture->device, capture->channel, NULL);
    if (ret != SUCCESS) {
        aloge("[VI] CreateVirChn failed: ret=%d", ret);
        goto error;
    }
    capture->channel_created = 1;

    ret = AW_MPI_VI_EnableVirChn(capture->device, capture->channel);
    if (ret != SUCCESS) {
        aloge("[VI] EnableVirChn failed: ret=%d", ret);
        goto error;
    }
    capture->channel_enabled = 1;

    ret = pthread_create(&capture->thread_id,
                         NULL,
                         video_capture_thread,
                         capture);
    if (ret != 0) {
        aloge("[VI] Create capture thread failed: ret=%d", ret);
        goto error;
    }
    capture->thread_started = 1;

    alogd("[VI] Video capture started successfully");
    return 0;

error:
    /* 所有启动失败都走同一条回滚路径，避免遗漏硬件资源。 */
    video_capture_destroy_pipeline(capture);
    return -1;
}

int video_capture_stop(VideoCaptureContext *capture)
{
    int result = 0;
    int ret;

    if (capture == NULL) {
        return -1;
    }

    /* 先通知线程停止，join 确认它不再访问 VI，然后才销毁通道。 */
    capture->stop_requested = 1;
    if (capture->thread_started) {
        ret = pthread_join(capture->thread_id, NULL);
        if (ret != 0) {
            aloge("[VI] Join capture thread failed: ret=%d", ret);
            result = -1;
        }
        capture->thread_started = 0;
    }

    if (video_capture_destroy_pipeline(capture) != 0) {
        result = -1;
    }

    alogd("[VI] Video capture stopped");
    return result;
}
