#include "video_capture.h"

#include <string.h>

#include <media/mpi_isp.h>
#include <media/mpi_vi.h>
#include <media/mpi_videoformat_conversion.h>
#include <utils/plat_log.h>

#include "video_display.h"

/* 统计只由采集线程读写，无需与VO回调共享锁。所有耗时单位都是微秒。 */
typedef struct Vipp4CaptureStats {
    Vipp4DurationStat get;
    Vipp4DurationStat hold;
    Vipp4DurationStat display;
    Vipp4DurationStat g2d;
    Vipp4DurationStat vo;
    Vipp4DurationStat release;
    uint64_t get_failures;
    uint64_t release_failures;
    uint64_t display_failures;
    uint64_t display_drops;
    uint64_t slow_holds;
} Vipp4CaptureStats;

/* 每100帧打印累计统计，而不是逐帧刷屏；退出时补打一份最终统计。 */
static void log_vipp4_timing(const VideoCaptureContext *capture,
                             const Vipp4CaptureStats *stats, int final)
{
    alogd("[VIPP4-DIAG] %s mode=%s frames=%llu "
          "get_fail=%llu release_fail=%llu display_fail=%llu "
          "display_drop=%llu slow_hold=%llu",
          final ? "final" : "summary",
          capture->diagnostic_capture_only ? "capture-only" : "preview-timing",
          (unsigned long long)capture->frame_count,
          (unsigned long long)stats->get_failures,
          (unsigned long long)stats->release_failures,
          (unsigned long long)stats->display_failures,
          (unsigned long long)stats->display_drops,
          (unsigned long long)stats->slow_holds);
    alogd("[VIPP4-DIAG] us avg/max: get=%llu/%llu hold=%llu/%llu "
          "display=%llu/%llu release=%llu/%llu",
          (unsigned long long)vipp4_duration_avg(&stats->get),
          (unsigned long long)stats->get.max_us,
          (unsigned long long)vipp4_duration_avg(&stats->hold),
          (unsigned long long)stats->hold.max_us,
          (unsigned long long)vipp4_duration_avg(&stats->display),
          (unsigned long long)stats->display.max_us,
          (unsigned long long)vipp4_duration_avg(&stats->release),
          (unsigned long long)stats->release.max_us);
    alogd("[VIPP4-DIAG] us avg/max: g2d=%llu/%llu samples=%llu "
          "vo_send=%llu/%llu samples=%llu",
          (unsigned long long)vipp4_duration_avg(&stats->g2d),
          (unsigned long long)stats->g2d.max_us,
          (unsigned long long)stats->g2d.count,
          (unsigned long long)vipp4_duration_avg(&stats->vo),
          (unsigned long long)stats->vo.max_us,
          (unsigned long long)stats->vo.count);
}

/*
 * VI 采集线程：循环从 VIPP 虚拟通道取帧，交给显示模块处理，
 * 然后立即归还 VI 帧。GetFrame 成功后的每一条路径都必须 ReleaseFrame。
 */
static void *video_capture_thread(void *argument)
{
    VideoCaptureContext *capture = argument;
    unsigned int consecutive_failures = 0;
    Vipp4CaptureStats stats;
    /* 20fps的一帧周期为50000us，仅作为慢处理提示，不是硬件FIFO阈值。 */
    uint64_t frame_budget_us = capture->frame_rate > 0
                                  ? 1000000ULL / capture->frame_rate : 0;
    memset(&stats, 0, sizeof(stats));

    alogd("[VI] Capture thread started");

    while (!capture->stop_requested) {
        VIDEO_FRAME_INFO_S frame;
        ERRORTYPE ret;
        int display_result = 0;
        int display_called = 0;
        int log_frame;
        uint64_t get_begin = 0, acquired = 0;
        uint64_t display_begin = 0, display_end = 0;
        uint64_t release_begin = 0, release_end = 0;
        uint64_t frame_pts = 0;
        VideoDisplayTiming display_timing = {0};

        memset(&frame, 0, sizeof(frame));
        /* 带超时取帧，使 stop_requested 可以在有限时间内被检查。 */
        if (capture->diagnostic_timing) {
            get_begin = vipp4_monotonic_us();
        }
        ret = AW_MPI_VI_GetFrame(capture->device,
                                 capture->channel,
                                 &frame,
                                 capture->timeout_ms);
        if (capture->diagnostic_timing) {
            acquired = vipp4_monotonic_us();
        }
        if (ret != SUCCESS) {
            ++stats.get_failures;
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
        frame_pts = frame.VFrame.mpts; /* 提前保存标量，归还后不依赖SDK保留描述。 */

        log_frame = capture->frame_count == 1U ||
                    (capture->frame_count % 100U) == 0U;
        /* 默认模式保持原日志顺序；诊断模式把日志放到归还源帧之后。 */
        if (log_frame && !capture->diagnostic_timing) {
            alogd("[VI] Frame=%llu, id=%u, size=%ux%u, pts=%llu us",
                  (unsigned long long)capture->frame_count,
                  frame.mId,
                  frame.VFrame.mWidth,
                  frame.VFrame.mHeight,
                  (unsigned long long)frame.VFrame.mpts);
        }

        /*
         * capture-only不是关闭VIPP4：照常GetFrame，但跳过G2D/VO逐帧处理。
         * 主线程仍初始化相同的VO/MMZ资源，也保留VIPP4上的ORL区域。
         */
        if (capture->display != NULL && !capture->diagnostic_capture_only) {
            display_called = 1;
            if (capture->diagnostic_timing) {
                display_begin = vipp4_monotonic_us();
                display_result = video_display_submit_timed(
                    capture->display, &frame, &display_timing);
                display_end = vipp4_monotonic_us();
            } else {
                display_result = video_display_submit(capture->display, &frame);
            }
        }

        /* 正常显示已完成源图读取；capture-only没有读取像素。两者都必须归还。 */
        if (capture->diagnostic_timing) {
            release_begin = vipp4_monotonic_us();
        }
        ret = AW_MPI_VI_ReleaseFrame(capture->device,
                                     capture->channel,
                                     &frame);
        if (capture->diagnostic_timing) {
            release_end = vipp4_monotonic_us();
        }
        if (ret != SUCCESS) {
            ++stats.release_failures;
            aloge("[VI] ReleaseFrame failed: ret=%d, frame_id=%u",
                  ret, frame.mId);
        }
        if (display_result < 0) {
            ++stats.display_failures;
            alogw("[VI] Display processing failed for frame=%llu",
                  (unsigned long long)capture->frame_count);
        } else if (display_called && display_result > 0) {
            ++stats.display_drops;
        }

        if (capture->diagnostic_timing) {
            uint64_t hold_us = vipp4_elapsed_us(acquired, release_end);
            vipp4_duration_add(&stats.get, get_begin, acquired);
            vipp4_duration_add(&stats.hold, acquired, release_end);
            vipp4_duration_add(&stats.release, release_begin, release_end);
            vipp4_duration_add(&stats.display, display_begin, display_end);
            vipp4_duration_add(&stats.g2d, display_timing.g2d_begin_us,
                               display_timing.g2d_end_us);
            vipp4_duration_add(&stats.vo, display_timing.vo_begin_us,
                               display_timing.vo_end_us);
            if (frame_budget_us != 0 && hold_us >= frame_budget_us) {
                ++stats.slow_holds;
                /* 限流告警，避免串口打印本身扩大处理抖动。 */
                if (stats.slow_holds <= 3 ||
                    (stats.slow_holds % 100ULL) == 0) {
                    alogw("[VIPP4-DIAG] Slow hold: frame=%llu hold_us=%llu "
                          "budget_us=%llu g2d_us=%llu vo_send_us=%llu "
                          "release_us=%llu display_ret=%d",
                          (unsigned long long)capture->frame_count,
                          (unsigned long long)hold_us,
                          (unsigned long long)frame_budget_us,
                          (unsigned long long)vipp4_elapsed_us(
                              display_timing.g2d_begin_us,
                              display_timing.g2d_end_us),
                          (unsigned long long)vipp4_elapsed_us(
                              display_timing.vo_begin_us,
                              display_timing.vo_end_us),
                          (unsigned long long)vipp4_elapsed_us(
                              release_begin, release_end), display_result);
                }
            }
            if (log_frame) {
                /* 使用归还前保存的PTS，不在ReleaseFrame后读取像素内存。 */
                alogd("[VIPP4-DIAG] frame=%llu pts=%llu us",
                      (unsigned long long)capture->frame_count,
                      (unsigned long long)frame_pts);
                log_vipp4_timing(capture, &stats, 0);
            }
        }
    }

    if (capture->diagnostic_timing) {
        log_vipp4_timing(capture, &stats, 1);
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
