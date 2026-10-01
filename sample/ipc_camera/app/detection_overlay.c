#define _POSIX_C_SOURCE 200809L

#include "detection_overlay.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <media/mpi_region.h>
#include <utils/plat_log.h>

#define DETECTION_OVERLAY_MAX_REGIONS 32U

/*
 * 编码和LCD会各启动一个overlay上下文。MPP region句柄属于全局资源，
 * 因此两个线程在Create/Attach/Detach/Destroy时使用同一把锁，避免并发
 * 修改MPP内部的region表。
 */
static pthread_mutex_t g_region_api_lock = PTHREAD_MUTEX_INITIALIZER;

struct DetectionOverlayContext {
    DetectionOverlayConfig config;
    MPP_CHN_S target_channel;
    pthread_t thread_id;
    volatile int stop_requested;
    int thread_started;
    unsigned int active_regions;
    unsigned long long last_sequence;
    unsigned long long last_snapshot_ms;
    unsigned long long update_count;
    unsigned int last_logged_regions;
};

/* 使用单调时间判断结果是否过期，避免NTP校时导致超时计算跳变。 */
static unsigned long long monotonic_time_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return (unsigned long long)now.tv_sec * 1000ULL +
           (unsigned long long)now.tv_nsec / 1000000ULL;
}

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec delay;

    delay.tv_sec = (time_t)(milliseconds / 1000U);
    delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    nanosleep(&delay, NULL);
}

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

/* ORL矩形坐标和宽高使用2像素对齐，与V853 MPP的约束保持一致。 */
static int align_down_2(int value)
{
    return value & ~1;
}

static int align_up_2(int value)
{
    return (value + 1) & ~1;
}

/*
 * 先删除上一次的框。MPP的ORL region必须先Detach，再Destroy。
 * 这和原项目paint_objects()的资源回收顺序一致。
 */
static int clear_regions_unlocked(DetectionOverlayContext *context)
{
    unsigned int index;
    int result = 0;

    for (index = 0U; index < context->active_regions; ++index) {
        RGN_HANDLE handle = context->config.region_handle_base + index;
        ERRORTYPE ret;

        ret = AW_MPI_RGN_DetachFromChn(handle, &context->target_channel);
        if (ret != SUCCESS) {
            alogw("[ORL] Detach region failed: handle=%u, ret=%d",
                  handle,
                  ret);
            result = -1;
        }
        ret = AW_MPI_RGN_Destroy(handle);
        if (ret != SUCCESS) {
            alogw("[ORL] Destroy region failed: handle=%u, ret=%d",
                  handle,
                  ret);
            result = -1;
        }
    }
    context->active_regions = 0U;
    return result;
}

static int clear_regions(DetectionOverlayContext *context)
{
    int result;

    pthread_mutex_lock(&g_region_api_lock);
    result = clear_regions_unlocked(context);
    pthread_mutex_unlock(&g_region_api_lock);
    return result;
}

/*
 * 将模型坐标映射为目标VIPP坐标。
 *
 * map_mirror/map_flip只用于NPU与目标视频方向不同的情况。V853板端
 * 实测SetVippMirror/Flip会修改共享sensor方向，VIPP 0和VIPP 8同时生效，
 * 所以当前主程序把这两项设为0，只做尺寸缩放。
 */
static int map_detection_rect(const DetectionOverlayContext *context,
                              const YoloDetection *detection,
                              RECT_S *rectangle)
{
    int xmin;
    int ymin;
    int xmax;
    int ymax;
    int original_min;
    int original_max;
    int left;
    int top;
    int right;
    int bottom;

    xmin = clamp_int(detection->xmin, 0, context->config.model_width);
    ymin = clamp_int(detection->ymin, 0, context->config.model_height);
    xmax = clamp_int(detection->xmax, 0, context->config.model_width);
    ymax = clamp_int(detection->ymax, 0, context->config.model_height);
    if (xmax <= xmin || ymax <= ymin) {
        return -1;
    }

    if (context->config.map_mirror) {
        original_min = xmin;
        original_max = xmax;
        xmin = context->config.model_width - original_max;
        xmax = context->config.model_width - original_min;
    }
    if (context->config.map_flip) {
        original_min = ymin;
        original_max = ymax;
        ymin = context->config.model_height - original_max;
        ymax = context->config.model_height - original_min;
    }

    left = xmin * context->config.target_width / context->config.model_width;
    top = ymin * context->config.target_height / context->config.model_height;
    right = xmax * context->config.target_width /
            context->config.model_width;
    bottom = ymax * context->config.target_height /
             context->config.model_height;

    left = align_down_2(clamp_int(left, 0, context->config.target_width - 2));
    top = align_down_2(clamp_int(top, 0, context->config.target_height - 2));
    right = align_up_2(clamp_int(right, left + 2, context->config.target_width));
    bottom = align_up_2(
        clamp_int(bottom, top + 2, context->config.target_height));

    rectangle->X = left;
    rectangle->Y = top;
    rectangle->Width = right - left;
    rectangle->Height = bottom - top;
    return 0;
}

/* 重建当前一帧的ORL集合，只保留person检测框。 */
static int draw_snapshot(DetectionOverlayContext *context,
                         const NpuDetectionSnapshot *snapshot)
{
    unsigned int requested;
    unsigned int index;
    unsigned int drawn = 0U;
    RECT_S first_video_rectangle;
    YoloDetection first_model_detection;
    int first_rectangle_valid = 0;
    int result = 0;

    pthread_mutex_lock(&g_region_api_lock);
    if (clear_regions_unlocked(context) != 0) {
        result = -1;
    }

    requested = snapshot->detection_count > 0
                    ? (unsigned int)snapshot->detection_count
                    : 0U;
    if (requested > context->config.max_regions) {
        requested = context->config.max_regions;
    }

    for (index = 0U; index < requested; ++index) {
        const YoloDetection *detection = &snapshot->detections[index];
        RGN_ATTR_S region_attributes;
        RGN_CHN_ATTR_S channel_attributes;
        RGN_HANDLE handle;
        ERRORTYPE ret;

        if (detection->label != 0) {
            continue;
        }

        memset(&channel_attributes, 0, sizeof(channel_attributes));
        if (map_detection_rect(context,
                               detection,
                               &channel_attributes.unChnAttr.stOrlChn.stRect) !=
            0) {
            continue;
        }

        /* 无效框可能被跳过，所以句柄按drawn连续编号，便于后续回收。 */
        handle = context->config.region_handle_base + drawn;

        memset(&region_attributes, 0, sizeof(region_attributes));
        region_attributes.enType = ORL_RGN;
        ret = AW_MPI_RGN_Create(handle, &region_attributes);
        if (ret != SUCCESS) {
            aloge("[ORL] Create region failed: handle=%u, ret=%d",
                  handle,
                  ret);
            result = -1;
            continue;
        }

        channel_attributes.bShow = TRUE;
        channel_attributes.enType = ORL_RGN;
        channel_attributes.unChnAttr.stOrlChn.enAreaType = AREA_RECT;
        channel_attributes.unChnAttr.stOrlChn.mColor = context->config.color;
        channel_attributes.unChnAttr.stOrlChn.mThick =
            context->config.thickness;
        channel_attributes.unChnAttr.stOrlChn.mLayer = drawn;

        ret = AW_MPI_RGN_AttachToChn(handle,
                                     &context->target_channel,
                                     &channel_attributes);
        if (ret != SUCCESS) {
            aloge("[ORL] Attach region failed: handle=%u, ret=%d",
                  handle,
                  ret);
            AW_MPI_RGN_Destroy(handle);
            result = -1;
            continue;
        }

        ++drawn;
        context->active_regions = drawn;
        if (!first_rectangle_valid) {
            first_video_rectangle =
                channel_attributes.unChnAttr.stOrlChn.stRect;
            first_model_detection = *detection;
            first_rectangle_valid = 1;
        }
    }

    ++context->update_count;
    /*
     * 人数变化或每50次更新输出一次坐标，既方便校准，也避免10 FPS
     * 持续刷日志影响媒体线程。
     */
    if (drawn != context->last_logged_regions ||
        (context->update_count % 50U) == 0U) {
        if (first_rectangle_valid) {
            alogd("[ORL] Box map: target=VIPP%d:%d, sequence=%llu, "
                  "model=(%d,%d)-(%d,%d), "
                  "video=(%d,%d)-(%d,%d), drawn=%u",
                  context->config.target_vi_device,
                  context->config.target_vi_channel,
                  snapshot->sequence,
                  first_model_detection.xmin,
                  first_model_detection.ymin,
                  first_model_detection.xmax,
                  first_model_detection.ymax,
                  first_video_rectangle.X,
                  first_video_rectangle.Y,
                  first_video_rectangle.X + first_video_rectangle.Width,
                  first_video_rectangle.Y + first_video_rectangle.Height,
                  drawn);
        } else {
            alogd("[ORL] Boxes cleared: target=VIPP%d:%d, sequence=%llu, "
                  "detected=%d",
                  context->config.target_vi_device,
                  context->config.target_vi_channel,
                  snapshot->sequence,
                  snapshot->detection_count);
        }
        context->last_logged_regions = drawn;
    }
    pthread_mutex_unlock(&g_region_api_lock);
    return result;
}

static void *detection_overlay_thread(void *argument)
{
    DetectionOverlayContext *context = argument;

    alogd("[ORL] Detection overlay thread started: target=VIPP%d:%d",
          context->config.target_vi_device,
          context->config.target_vi_channel);
    while (!context->stop_requested) {
        NpuDetectionSnapshot snapshot;
        unsigned long long now_ms = monotonic_time_ms();

        memset(&snapshot, 0, sizeof(snapshot));
        if (npu_detector_get_latest(context->config.detector, &snapshot) != 0) {
            aloge("[ORL] Read NPU result snapshot failed");
            break;
        }

        if (snapshot.sequence != 0U &&
            snapshot.sequence != context->last_sequence) {
            draw_snapshot(context, &snapshot);
            context->last_sequence = snapshot.sequence;
            context->last_snapshot_ms = now_ms;
        } else if (context->active_regions > 0U &&
                   context->last_snapshot_ms != 0U &&
                   now_ms >= context->last_snapshot_ms &&
                   now_ms - context->last_snapshot_ms >=
                       context->config.stale_timeout_ms) {
            /* NPU停止产生新结果时不能把最后一个框永久留在画面上。 */
            clear_regions(context);
            alogw("[ORL] Stale detection boxes cleared: target=VIPP%d:%d, "
                  "timeout=%u ms",
                  context->config.target_vi_device,
                  context->config.target_vi_channel,
                  context->config.stale_timeout_ms);
        }

        sleep_ms(context->config.poll_interval_ms);
    }

    clear_regions(context);
    alogd("[ORL] Detection overlay thread stopped: target=VIPP%d:%d, "
          "updates=%llu",
          context->config.target_vi_device,
          context->config.target_vi_channel,
          context->update_count);
    return NULL;
}

DetectionOverlayContext *detection_overlay_create(
    const DetectionOverlayConfig *config)
{
    DetectionOverlayContext *context;

    if (config == NULL || config->detector == NULL ||
        config->target_vi_device < 0 || config->target_vi_channel < 0 ||
        config->target_width <= 0 || config->target_height <= 0 ||
        config->model_width <= 0 || config->model_height <= 0 ||
        config->max_regions == 0U ||
        config->max_regions > DETECTION_OVERLAY_MAX_REGIONS ||
        config->region_handle_base >= RGN_HANDLE_MAX ||
        config->max_regions > RGN_HANDLE_MAX - config->region_handle_base) {
        return NULL;
    }

    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    context->config = *config;
    context->last_logged_regions = (unsigned int)-1;
    context->target_channel.mModId = MOD_ID_VIU;
    context->target_channel.mDevId = config->target_vi_device;
    context->target_channel.mChnId = config->target_vi_channel;
    return context;
}

int detection_overlay_start(DetectionOverlayContext *context)
{
    int thread_ret;

    if (context == NULL || context->thread_started) {
        return -1;
    }

    context->stop_requested = 0;
    thread_ret = pthread_create(&context->thread_id,
                                NULL,
                                detection_overlay_thread,
                                context);
    if (thread_ret != 0) {
        aloge("[ORL] Create overlay thread failed: ret=%d", thread_ret);
        return -1;
    }
    context->thread_started = 1;

    alogd("[ORL] Detection overlay started: target=VIPP%d:%d, "
          "model=%dx%d, video=%dx%d, handles=%u..%u",
          context->config.target_vi_device,
          context->config.target_vi_channel,
          context->config.model_width,
          context->config.model_height,
          context->config.target_width,
          context->config.target_height,
          context->config.region_handle_base,
          context->config.region_handle_base +
              context->config.max_regions - 1U);
    return 0;
}

int detection_overlay_stop(DetectionOverlayContext *context)
{
    int result = 0;

    if (context == NULL) {
        return -1;
    }

    context->stop_requested = 1;
    if (context->thread_started) {
        if (pthread_join(context->thread_id, NULL) != 0) {
            aloge("[ORL] Join overlay thread failed");
            result = -1;
        }
        context->thread_started = 0;
    } else if (clear_regions(context) != 0) {
        result = -1;
    }
    return result;
}

void detection_overlay_destroy(DetectionOverlayContext *context)
{
    if (context == NULL) {
        return;
    }
    if (context->thread_started || context->active_regions > 0U) {
        detection_overlay_stop(context);
    }
    free(context);
}
