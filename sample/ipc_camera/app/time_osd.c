#define _POSIX_C_SOURCE 200809L

#include "time_osd.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <media/mpi_region.h>
#include <rgb_ctrl.h>
#include <utils/plat_log.h>

struct TimeOsdContext {
    TimeOsdConfig config;
    MPP_CHN_S venc_mpp_channel;
    pthread_t update_thread;
    volatile int stop_requested;

    /* 记录每项资源是否创建成功，方便初始化失败时按实际状态回滚。 */
    int font_loaded;
    int region_created;
    int region_attached;
    int thread_started;

    unsigned int bitmap_width;
    unsigned int bitmap_height;
    unsigned long long update_count;
};

/* 向上对齐。RGN尺寸使用16对齐，显示坐标使用4对齐。 */
static unsigned int align_up(unsigned int value, unsigned int alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

/*
 * 使用原项目的 rgb_ctrl 字库生成时间图片。
 * 字库由目标系统的 /usr/share/osd/fonts/asc64.lz4 提供。
 */
static int create_time_picture(RGB_PIC_S *picture,
                               char *time_text,
                               size_t time_text_capacity)
{
    FONT_RGBPIC_S font_picture;
    struct tm local_time;
    time_t now;
    int ret;

    if (picture == NULL || time_text == NULL) {
        return -1;
    }

    now = time(NULL);
    if (localtime_r(&now, &local_time) == NULL ||
        strftime(time_text,
                 time_text_capacity,
                 "%Y-%m-%d %H:%M:%S",
                 &local_time) == 0U) {
        return -1;
    }

    /*
     * 与原项目保持一致：64号ASCII字库、32位RGB、白色文字、透明背景。
     * foreground/background 是字节数组，全0xff形成不透明白色。
     */
    memset(&font_picture, 0, sizeof(font_picture));
    font_picture.font_type = FONT_SIZE_64;
    font_picture.rgb_type = OSD_RGB_32;
    font_picture.enable_bg = 0;
    font_picture.foreground[0] = 0xff;
    font_picture.foreground[1] = 0xff;
    font_picture.foreground[2] = 0xff;
    font_picture.foreground[3] = 0xff;

    memset(picture, 0, sizeof(*picture));
    picture->enable_mosaic = 0;
    picture->rgb_type = OSD_RGB_32;
    ret = create_font_rectangle(time_text, &font_picture, picture);
    if (ret != 0 || picture->pic_addr == NULL || picture->wide == 0U ||
        picture->high == 0U) {
        aloge("[OSD] create_font_rectangle failed: ret=%d", ret);
        return -1;
    }
    return 0;
}

/*
 * 把 rgb_ctrl 生成的图片设置到MPP区域。
 * SetBitMap返回后MPP已接收位图，调用者便可释放RGB_PIC_S内存。
 */
static int set_region_picture(TimeOsdContext *context,
                              const RGB_PIC_S *picture)
{
    BITMAP_S bitmap;
    unsigned int width;
    unsigned int height;
    ERRORTYPE ret;

    width = align_up(picture->wide, 16U);
    height = align_up(picture->high, 16U);
    if (context->bitmap_width != 0U &&
        (width != context->bitmap_width || height != context->bitmap_height)) {
        aloge("[OSD] Bitmap size changed unexpectedly: %ux%u -> %ux%u",
              context->bitmap_width,
              context->bitmap_height,
              width,
              height);
        return -1;
    }

    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.mPixelFormat = MM_PIXEL_FORMAT_RGB_8888;
    bitmap.mWidth = width;
    bitmap.mHeight = height;
    bitmap.mpData = picture->pic_addr;
    ret = AW_MPI_RGN_SetBitMap(context->config.handle, &bitmap);
    if (ret != SUCCESS) {
        aloge("[OSD] Set bitmap failed: ret=%d", ret);
        return -1;
    }
    return 0;
}

/* 生成当前时间图片、更新RGN并及时释放用户态图片。 */
static int update_time_bitmap(TimeOsdContext *context)
{
    RGB_PIC_S picture;
    char time_text[32];
    int result;

    if (create_time_picture(&picture, time_text, sizeof(time_text)) != 0) {
        return -1;
    }
    result = set_region_picture(context, &picture);
    release_rgb_picture(&picture);
    if (result != 0) {
        return -1;
    }

    ++context->update_count;
    if (context->update_count == 1ULL ||
        (context->update_count % 60ULL) == 0ULL) {
        alogd("[OSD] Time updated: %s, count=%llu",
              time_text,
              context->update_count);
    }
    return 0;
}

static void *time_osd_update_thread(void *argument)
{
    TimeOsdContext *context = argument;

    alogd("[OSD] Update thread started");
    while (!context->stop_requested) {
        unsigned int elapsed;

        /* 每100 ms检查一次退出标志，避免Ctrl+C后等待完整的一秒。 */
        for (elapsed = 0;
             elapsed < context->config.update_seconds * 10U &&
             !context->stop_requested;
             ++elapsed) {
            struct timespec delay;
            delay.tv_sec = 0;
            delay.tv_nsec = 100000000L;
            nanosleep(&delay, NULL);
        }
        if (!context->stop_requested && update_time_bitmap(context) != 0) {
            alogw("[OSD] This update failed; will retry next period");
        }
    }

    alogd("[OSD] Update thread stopped, updates=%llu", context->update_count);
    return NULL;
}

TimeOsdContext *time_osd_create(const TimeOsdConfig *config)
{
    TimeOsdContext *context;

    if (config == NULL || config->venc_channel < 0 ||
        config->update_seconds == 0U || config->x < 0 || config->y < 0) {
        return NULL;
    }

    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    context->config = *config;
    context->config.x = (int)align_up((unsigned int)config->x, 4U);
    context->config.y = (int)align_up((unsigned int)config->y, 4U);
    context->venc_mpp_channel.mModId = MOD_ID_VENC;
    context->venc_mpp_channel.mDevId = 0;
    context->venc_mpp_channel.mChnId = config->venc_channel;
    return context;
}

int time_osd_start(TimeOsdContext *context)
{
    RGN_ATTR_S region_attributes;
    RGN_CHN_ATTR_S channel_attributes;
    RGB_PIC_S first_picture;
    char first_time_text[32];
    ERRORTYPE ret;
    int thread_ret;

    if (context == NULL || context->thread_started || context->region_created) {
        return -1;
    }

    /* 原项目字库接口必须先加载对应字号，再生成文字图片。 */
    if (load_font_file(FONT_SIZE_64) < 0) {
        aloge("[OSD] Load FONT_SIZE_64 failed; check "
              "/usr/share/osd/fonts/asc64.lz4");
        return -1;
    }
    context->font_loaded = 1;

    if (create_time_picture(&first_picture,
                            first_time_text,
                            sizeof(first_time_text)) != 0) {
        goto error;
    }
    context->bitmap_width = align_up(first_picture.wide, 16U);
    context->bitmap_height = align_up(first_picture.high, 16U);

    /* 以下区域参数与原项目相同：OVERLAY_RGN + RGB8888。 */
    memset(&region_attributes, 0, sizeof(region_attributes));
    region_attributes.enType = OVERLAY_RGN;
    region_attributes.unAttr.stOverlay.mPixelFmt = MM_PIXEL_FORMAT_RGB_8888;
    region_attributes.unAttr.stOverlay.mBgColor = 0;
    region_attributes.unAttr.stOverlay.mSize.Width = context->bitmap_width;
    region_attributes.unAttr.stOverlay.mSize.Height = context->bitmap_height;

    ret = AW_MPI_RGN_Create(context->config.handle, &region_attributes);
    if (ret != SUCCESS) {
        aloge("[OSD] Create region failed: handle=%u, ret=%d",
              context->config.handle,
              ret);
        release_rgb_picture(&first_picture);
        goto error;
    }
    context->region_created = 1;

    if (set_region_picture(context, &first_picture) != 0) {
        release_rgb_picture(&first_picture);
        goto error;
    }
    release_rgb_picture(&first_picture);
    context->update_count = 1;
    alogd("[OSD] Time updated: %s, count=1", first_time_text);

    memset(&channel_attributes, 0, sizeof(channel_attributes));
    channel_attributes.bShow = TRUE;
    channel_attributes.enType = OVERLAY_RGN;
    channel_attributes.unChnAttr.stOverlayChn.stPoint.X = context->config.x;
    channel_attributes.unChnAttr.stOverlayChn.stPoint.Y = context->config.y;
    channel_attributes.unChnAttr.stOverlayChn.mLayer = 0;
    channel_attributes.unChnAttr.stOverlayChn.mFgAlpha = 0x40;
    channel_attributes.unChnAttr.stOverlayChn.stInvertColor.stInvColArea.Width =
        16;
    channel_attributes.unChnAttr.stOverlayChn.stInvertColor.stInvColArea.Height =
        16;
    channel_attributes.unChnAttr.stOverlayChn.stInvertColor.mLumThresh = 60;
    channel_attributes.unChnAttr.stOverlayChn.stInvertColor.enChgMod =
        LESSTHAN_LUMDIFF_THRESH;
    channel_attributes.unChnAttr.stOverlayChn.stInvertColor.bInvColEn = TRUE;

    ret = AW_MPI_RGN_AttachToChn(context->config.handle,
                                 &context->venc_mpp_channel,
                                 &channel_attributes);
    if (ret != SUCCESS) {
        aloge("[OSD] Attach region to VENC failed: ret=%d", ret);
        goto error;
    }
    context->region_attached = 1;

    context->stop_requested = 0;
    thread_ret = pthread_create(&context->update_thread,
                                NULL,
                                time_osd_update_thread,
                                context);
    if (thread_ret != 0) {
        aloge("[OSD] Create update thread failed: ret=%d", thread_ret);
        goto error;
    }
    context->thread_started = 1;

    alogd("[OSD] Time watermark started: venc=%d, handle=%u, "
          "position=(%d,%d), bitmap=%ux%u, font=FONT_SIZE_64",
          context->config.venc_channel,
          context->config.handle,
          context->config.x,
          context->config.y,
          context->bitmap_width,
          context->bitmap_height);
    return 0;

error:
    time_osd_stop(context);
    return -1;
}

int time_osd_stop(TimeOsdContext *context)
{
    ERRORTYPE ret;
    int result = 0;
    int thread_ret;

    if (context == NULL) {
        return -1;
    }

    context->stop_requested = 1;
    if (context->thread_started) {
        thread_ret = pthread_join(context->update_thread, NULL);
        if (thread_ret != 0) {
            aloge("[OSD] Join update thread failed: ret=%d", thread_ret);
            result = -1;
        }
        context->thread_started = 0;
    }
    if (context->region_attached) {
        ret = AW_MPI_RGN_DetachFromChn(context->config.handle,
                                       &context->venc_mpp_channel);
        if (ret != SUCCESS) {
            aloge("[OSD] Detach region failed: ret=%d", ret);
            result = -1;
        }
        context->region_attached = 0;
    }
    if (context->region_created) {
        ret = AW_MPI_RGN_Destroy(context->config.handle);
        if (ret != SUCCESS) {
            aloge("[OSD] Destroy region failed: ret=%d", ret);
            result = -1;
        }
        context->region_created = 0;
    }
    if (context->font_loaded) {
        if (unload_font_file(FONT_SIZE_64) < 0) {
            aloge("[OSD] Unload FONT_SIZE_64 failed");
            result = -1;
        }
        context->font_loaded = 0;
    }

    alogd("[OSD] Time watermark stopped: updates=%llu",
          context->update_count);
    return result;
}

void time_osd_destroy(TimeOsdContext *context)
{
    if (context == NULL) {
        return;
    }
    if (context->thread_started || context->region_attached ||
        context->region_created || context->font_loaded) {
        time_osd_stop(context);
    }
    free(context);
}
