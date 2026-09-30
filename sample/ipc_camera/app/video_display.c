#include "video_display.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <media/mpi_sys.h>
#include <media/mpi_vo.h>
#include <utils/plat_log.h>
#include <vo/hwdisplay.h>

#include "g2d.h"

#define VIDEO_DISPLAY_BUFFER_COUNT 5
#define VIDEO_DISPLAY_VO_DEVICE 0
#define VIDEO_DISPLAY_UI_LAYER HLAY(2, 0)
#define VIDEO_DISPLAY_LAYER 0
#define VIDEO_DISPLAY_CHANNEL 0

/* 一个池节点：frame 描述 MMZ 内存，in_use 表示 VO 是否仍持有它。 */
typedef struct VideoDisplayBuffer {
    VIDEO_FRAME_INFO_S frame;
    int in_use;
} VideoDisplayBuffer;

struct VideoDisplayContext {
    VideoDisplayConfig config;
    G2dContext g2d;
    VideoDisplayBuffer buffers[VIDEO_DISPLAY_BUFFER_COUNT];
    /* 采集线程取缓冲、VO 回调释放缓冲，两者必须互斥。 */
    pthread_mutex_t buffer_lock;
    int mutex_initialized;

    VO_DEV device;
    VO_LAYER ui_layer;
    VO_LAYER layer;
    VO_CHN channel;

    /* 资源状态标志用于失败回滚和幂等 stop。 */
    int g2d_opened;
    int buffers_allocated;
    int device_enabled;
    int ui_layer_added;
    int video_layer_enabled;
    int channel_created;
    int channel_started;

    unsigned long long submitted_frames;
    unsigned long long released_frames;
    unsigned long long dropped_frames;
};

/* 将一个 MMZ 帧放回帧池；可由 VO 回调或提交失败路径调用。 */
static void video_display_release_buffer(VideoDisplayContext *display,
                                         unsigned int frame_id,
                                         int released_by_vo)
{
    if (frame_id >= VIDEO_DISPLAY_BUFFER_COUNT) {
        aloge("[VO] Invalid released frame id: %u", frame_id);
        return;
    }

    pthread_mutex_lock(&display->buffer_lock);
    if (display->buffers[frame_id].in_use) {
        display->buffers[frame_id].in_use = 0;
        if (released_by_vo) {
            ++display->released_frames;
        }
    } else {
        alogw("[VO] Frame %u was released more than once", frame_id);
    }
    pthread_mutex_unlock(&display->buffer_lock);
}

/* MPP 在 VO 完成一帧显示后回调，将该帧的所有权还给应用。 */
static ERRORTYPE video_display_callback(void *cookie,
                                        MPP_CHN_S *channel,
                                        MPP_EVENT_TYPE event,
                                        void *event_data)
{
    VideoDisplayContext *display = cookie;

    if (display == NULL || channel == NULL || channel->mModId != MOD_ID_VOU) {
        return FAILURE;
    }

    switch (event) {
    case MPP_EVENT_RELEASE_VIDEO_BUFFER: {
        VIDEO_FRAME_INFO_S *frame = event_data;
        if (frame != NULL) {
            video_display_release_buffer(display, frame->mId, 1);
        }
        break;
    }
    case MPP_EVENT_SET_VIDEO_SIZE:
    case MPP_EVENT_RENDERING_START:
        break;
    default:
        alogw("[VO] Unhandled callback event: 0x%x", event);
        break;
    }

    return SUCCESS;
}

/* 只有确保 VO 不再访问帧池后，才能释放这些 MMZ 物理连续内存。 */
static void video_display_free_buffers(VideoDisplayContext *display)
{
    int i;
    int plane;

    for (i = 0; i < VIDEO_DISPLAY_BUFFER_COUNT; ++i) {
        for (plane = 0; plane < 3; ++plane) {
            if (display->buffers[i].frame.VFrame.mpVirAddr[plane] != NULL) {
                AW_MPI_SYS_MmzFree(
                    display->buffers[i].frame.VFrame.mPhyAddr[plane],
                    display->buffers[i].frame.VFrame.mpVirAddr[plane]);
                display->buffers[i].frame.VFrame.mPhyAddr[plane] = 0;
                display->buffers[i].frame.VFrame.mpVirAddr[plane] = NULL;
            }
        }
        display->buffers[i].in_use = 0;
    }
    display->buffers_allocated = 0;
}

/*
 * 为旋转后的 NV21 图像创建 5 个 MMZ 帧。NV21 大小为 width*height*3/2：
 * Y 平面占 width*height，VU 平面占一半。
 */
static int video_display_allocate_buffers(VideoDisplayContext *display)
{
    int output_width = display->g2d.destination_width;
    int output_height = display->g2d.destination_height;
    int y_size = output_width * output_height;
    int chroma_size = y_size / 2;
    int i;
    ERRORTYPE ret;

    for (i = 0; i < VIDEO_DISPLAY_BUFFER_COUNT; ++i) {
        VideoDisplayBuffer *buffer = &display->buffers[i];

        memset(buffer, 0, sizeof(*buffer));
        buffer->frame.mId = (unsigned int)i;
        buffer->frame.VFrame.mWidth = (unsigned int)output_width;
        buffer->frame.VFrame.mHeight = (unsigned int)output_height;
        buffer->frame.VFrame.mPixelFormat = display->config.pixel_format;
        buffer->frame.VFrame.mField = VIDEO_FIELD_FRAME;
        buffer->frame.VFrame.mStride[0] = (unsigned int)output_width;
        buffer->frame.VFrame.mStride[1] = (unsigned int)output_width;

        /* G2D/VO 需要物理地址，CPU 调试时则可使用对应虚拟地址。 */
        ret = AW_MPI_SYS_MmzAlloc_Cached(
            &buffer->frame.VFrame.mPhyAddr[0],
            &buffer->frame.VFrame.mpVirAddr[0],
            y_size);
        if (ret != SUCCESS) {
            aloge("[VO] Allocate Y plane failed: buffer=%d, ret=%d", i, ret);
            video_display_free_buffers(display);
            return -1;
        }

        ret = AW_MPI_SYS_MmzAlloc_Cached(
            &buffer->frame.VFrame.mPhyAddr[1],
            &buffer->frame.VFrame.mpVirAddr[1],
            chroma_size);
        if (ret != SUCCESS) {
            aloge("[VO] Allocate VU plane failed: buffer=%d, ret=%d", i, ret);
            video_display_free_buffers(display);
            return -1;
        }
    }

    display->buffers_allocated = 1;
    alogd("[VO] Allocated %d MMZ buffers, each %dx%d NV21",
          VIDEO_DISPLAY_BUFFER_COUNT, output_width, output_height);
    return 0;
}

/* 从帧池中非阻塞地取一个空闲帧；全忙时返回 NULL 并丢弃当前预览帧。 */
static VideoDisplayBuffer *video_display_acquire_buffer(
    VideoDisplayContext *display)
{
    VideoDisplayBuffer *buffer = NULL;
    int i;

    pthread_mutex_lock(&display->buffer_lock);
    for (i = 0; i < VIDEO_DISPLAY_BUFFER_COUNT; ++i) {
        if (!display->buffers[i].in_use) {
            display->buffers[i].in_use = 1;
            buffer = &display->buffers[i];
            break;
        }
    }
    pthread_mutex_unlock(&display->buffer_lock);
    return buffer;
}

VideoDisplayContext *video_display_create(const VideoDisplayConfig *config)
{
    VideoDisplayContext *display;

    if (config == NULL || config->source_width <= 0 ||
        config->source_height <= 0 || config->display_width <= 0 ||
        config->display_height <= 0) {
        return NULL;
    }

    /* create 不访问硬件，只分配上下文和初始化锁。 */
    display = calloc(1, sizeof(*display));
    if (display == NULL) {
        return NULL;
    }

    display->config = *config;
    display->g2d.fd = -1;
    display->device = VIDEO_DISPLAY_VO_DEVICE;
    display->ui_layer = VIDEO_DISPLAY_UI_LAYER;
    display->layer = VIDEO_DISPLAY_LAYER;
    display->channel = VIDEO_DISPLAY_CHANNEL;

    if (pthread_mutex_init(&display->buffer_lock, NULL) != 0) {
        free(display);
        return NULL;
    }
    display->mutex_initialized = 1;
    return display;
}

int video_display_start(VideoDisplayContext *display)
{
    VO_PUB_ATTR_S public_attributes;
    VO_VIDEO_LAYER_ATTR_S layer_attributes;
    MPPCallbackInfo callback;
    ERRORTYPE ret;

    if (display == NULL) {
        return -1;
    }

    /* 先准备转换器和输出帧，再启动 VO，确保通道启动后立即可送帧。 */
    if (g2d_open(&display->g2d,
                 display->config.source_width,
                 display->config.source_height,
                 display->config.rotation) != 0) {
        goto error;
    }
    display->g2d_opened = 1;

    if (video_display_allocate_buffers(display) != 0) {
        goto error;
    }

    ret = AW_MPI_VO_Enable(display->device);
    if (ret != SUCCESS) {
        aloge("[VO] Enable device failed: ret=%d", ret);
        goto error;
    }
    display->device_enabled = 1;

    /* 关闭默认 UI layer，避免它覆盖摄像头视频 layer。 */
    ret = AW_MPI_VO_AddOutsideVideoLayer(display->ui_layer);
    if (ret != SUCCESS) {
        aloge("[VO] Add UI layer failed: ret=%d", ret);
        goto error;
    }
    display->ui_layer_added = 1;

    ret = AW_MPI_VO_CloseVideoLayer(display->ui_layer);
    if (ret != SUCCESS) {
        aloge("[VO] Close UI layer failed: ret=%d", ret);
        goto error;
    }

    memset(&public_attributes, 0, sizeof(public_attributes));
    ret = AW_MPI_VO_GetPubAttr(display->device, &public_attributes);
    if (ret != SUCCESS) {
        aloge("[VO] Get public attributes failed: ret=%d", ret);
        goto error;
    }
    /* 输出设备是板载 LCD；实际时序由底层 LCD panel 配置决定。 */
    public_attributes.enIntfType = VO_INTF_LCD;
    public_attributes.enIntfSync = VO_OUTPUT_NTSC;
    ret = AW_MPI_VO_SetPubAttr(display->device, &public_attributes);
    if (ret != SUCCESS) {
        aloge("[VO] Set public attributes failed: ret=%d", ret);
        goto error;
    }

    ret = AW_MPI_VO_EnableVideoLayer(display->layer);
    if (ret != SUCCESS) {
        aloge("[VO] Enable video layer failed: ret=%d", ret);
        goto error;
    }
    display->video_layer_enabled = 1;

    memset(&layer_attributes, 0, sizeof(layer_attributes));
    ret = AW_MPI_VO_GetVideoLayerAttr(display->layer, &layer_attributes);
    if (ret != SUCCESS) {
        aloge("[VO] Get layer attributes failed: ret=%d", ret);
        goto error;
    }
    layer_attributes.stDispRect.X = display->config.display_x;
    layer_attributes.stDispRect.Y = display->config.display_y;
    layer_attributes.stDispRect.Width = display->config.display_width;
    layer_attributes.stDispRect.Height = display->config.display_height;
    ret = AW_MPI_VO_SetVideoLayerAttr(display->layer, &layer_attributes);
    if (ret != SUCCESS) {
        aloge("[VO] Set layer attributes failed: ret=%d", ret);
        goto error;
    }

    ret = AW_MPI_VO_CreateChn(display->layer, display->channel);
    if (ret != SUCCESS) {
        aloge("[VO] Create channel failed: ret=%d", ret);
        goto error;
    }
    display->channel_created = 1;

    memset(&callback, 0, sizeof(callback));
    callback.cookie = display;
    callback.callback = video_display_callback;
    ret = AW_MPI_VO_RegisterCallback(display->layer,
                                     display->channel,
                                     &callback);
    if (ret != SUCCESS) {
        aloge("[VO] Register callback failed: ret=%d", ret);
        goto error;
    }

    /* VO 内部使用双缓冲，应用侧仍保留 5 帧 MMZ 池吸收并发抖动。 */
    ret = AW_MPI_VO_SetChnDispBufNum(display->layer, display->channel, 2);
    if (ret != SUCCESS) {
        aloge("[VO] Set display buffer count failed: ret=%d", ret);
        goto error;
    }

    ret = AW_MPI_VO_StartChn(display->layer, display->channel);
    if (ret != SUCCESS) {
        aloge("[VO] Start channel failed: ret=%d", ret);
        goto error;
    }
    display->channel_started = 1;

    alogd("[VO] Display started: layer=%d, chn=%d, lcd=%dx%d",
          display->layer,
          display->channel,
          display->config.display_width,
          display->config.display_height);
    return 0;

error:
    /* stop 会根据状态标志只释放已成功创建的部分。 */
    video_display_stop(display);
    return -1;
}

int video_display_submit(VideoDisplayContext *display,
                         const VIDEO_FRAME_INFO_S *source)
{
    VideoDisplayBuffer *buffer;
    ERRORTYPE ret;

    if (display == NULL || source == NULL || !display->channel_started) {
        return -1;
    }

    /* 帧池全忙时不阻塞 VI，而是丢当前预览帧保证实时性。 */
    buffer = video_display_acquire_buffer(display);
    if (buffer == NULL) {
        ++display->dropped_frames;
        if (display->dropped_frames == 1ULL ||
            (display->dropped_frames % 100ULL) == 0ULL) {
            alogw("[VO] No idle output buffer, dropped=%llu",
                  display->dropped_frames);
        }
        return 1;
    }

    if (g2d_convert_frame(&display->g2d, source, &buffer->frame) != 0) {
        video_display_release_buffer(display, buffer->frame.mId, 0);
        return -1;
    }

    /*
     * SendFrame 成功只表示 VO 接管了帧，不表示显示已完成。
     * 在 RELEASE_VIDEO_BUFFER 回调到来前，in_use 必须保持为 1。
     */
    ret = AW_MPI_VO_SendFrame(display->layer,
                              display->channel,
                              &buffer->frame,
                              0);
    if (ret != SUCCESS) {
        alogw("[VO] SendFrame failed: ret=%d", ret);
        video_display_release_buffer(display, buffer->frame.mId, 0);
        return -1;
    }

    ++display->submitted_frames;
    if (display->submitted_frames == 1ULL ||
        (display->submitted_frames % 100ULL) == 0ULL) {
        alogd("[VO] Submitted frame=%llu, pts=%llu us",
              display->submitted_frames,
              (unsigned long long)buffer->frame.VFrame.mpts);
    }
    return 0;
}

int video_display_stop(VideoDisplayContext *display)
{
    int result = 0;
    ERRORTYPE ret;

    if (display == NULL) {
        return -1;
    }

    /*
     * 先停止/销毁 VO 通道，让 VO 放弃对输出帧的引用；然后关 layer
     * 和 device，最后才释放 MMZ 帧池。提前释放 MMZ 可能造成花屏或崩溃。
     */
    if (display->channel_started) {
        ret = AW_MPI_VO_StopChn(display->layer, display->channel);
        if (ret != SUCCESS) {
            aloge("[VO] Stop channel failed: ret=%d", ret);
            result = -1;
        }
        display->channel_started = 0;
    }

    if (display->channel_created) {
        ret = AW_MPI_VO_DestroyChn(display->layer, display->channel);
        if (ret != SUCCESS) {
            aloge("[VO] Destroy channel failed: ret=%d", ret);
            result = -1;
        }
        display->channel_created = 0;
    }

    if (display->video_layer_enabled) {
        ret = AW_MPI_VO_DisableVideoLayer(display->layer);
        if (ret != SUCCESS) {
            aloge("[VO] Disable video layer failed: ret=%d", ret);
            result = -1;
        }
        display->video_layer_enabled = 0;
    }

    if (display->ui_layer_added) {
        ret = AW_MPI_VO_RemoveOutsideVideoLayer(display->ui_layer);
        if (ret != SUCCESS) {
            aloge("[VO] Remove UI layer failed: ret=%d", ret);
            result = -1;
        }
        display->ui_layer_added = 0;
    }

    if (display->device_enabled) {
        ret = AW_MPI_VO_Disable(display->device);
        if (ret != SUCCESS) {
            aloge("[VO] Disable device failed: ret=%d", ret);
            result = -1;
        }
        display->device_enabled = 0;
    }

    if (display->buffers_allocated) {
        video_display_free_buffers(display);
    }

    if (display->g2d_opened) {
        g2d_close(&display->g2d);
        display->g2d_opened = 0;
    }

    alogd("[VO] Display stopped: submitted=%llu, released=%llu, dropped=%llu",
          display->submitted_frames,
          display->released_frames,
          display->dropped_frames);
    return result;
}

void video_display_destroy(VideoDisplayContext *display)
{
    if (display == NULL) {
        return;
    }

    /* destroy 可以直接用在未 start、完整 start 或部分 start 的对象上。 */
    if (display->channel_started || display->channel_created ||
        display->video_layer_enabled || display->ui_layer_added ||
        display->device_enabled || display->buffers_allocated ||
        display->g2d_opened) {
        video_display_stop(display);
    }
    if (display->mutex_initialized) {
        pthread_mutex_destroy(&display->buffer_lock);
    }
    free(display);
}
