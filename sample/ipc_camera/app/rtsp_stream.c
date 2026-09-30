#include "rtsp_stream.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <utils/plat_log.h>

typedef struct RtspVideoFrame {
    unsigned char *data;
    unsigned int size;
    uint64_t pts;
    int key_frame;
} RtspVideoFrame;

struct RtspStreamContext {
    RtspStreamConfig config;
    RtspVideoFrame *queue;
    unsigned int head;
    unsigned int tail;
    unsigned int count;
    pthread_mutex_t mutex;
    pthread_cond_t frame_available;
    pthread_t thread;
    int mutex_initialized;
    int condition_initialized;
    int server_opened;
    int server_started;
    int thread_started;
    int stop_requested;
    unsigned long long frames_queued;
    unsigned long long frames_sent;
    unsigned long long frames_dropped;
};

static void release_frame(RtspVideoFrame *frame)
{
    free(frame->data);
    memset(frame, 0, sizeof(*frame));
}

static void *rtsp_sender_thread(void *argument)
{
    RtspStreamContext *context = argument;

    alogd("[RTSP] Sender thread started");
    for (;;) {
        RtspVideoFrame frame;

        memset(&frame, 0, sizeof(frame));
        pthread_mutex_lock(&context->mutex);
        while (context->count == 0U && !context->stop_requested) {
            pthread_cond_wait(&context->frame_available, &context->mutex);
        }
        if (context->count == 0U && context->stop_requested) {
            pthread_mutex_unlock(&context->mutex);
            break;
        }

        frame = context->queue[context->head];
        memset(&context->queue[context->head], 0,
               sizeof(context->queue[context->head]));
        context->head = (context->head + 1U) % context->config.queue_capacity;
        --context->count;
        pthread_mutex_unlock(&context->mutex);

        if (rtsp_server_send_video(context->config.session_id,
                                   frame.data,
                                   frame.size,
                                   frame.pts,
                                   frame.key_frame ? RTSP_FRAME_TYPE_I
                                                   : RTSP_FRAME_TYPE_P) == 0) {
            ++context->frames_sent;
        }
        release_frame(&frame);
    }

    alogd("[RTSP] Sender thread stopped: queued=%llu, sent=%llu, dropped=%llu",
          context->frames_queued,
          context->frames_sent,
          context->frames_dropped);
    return NULL;
}

RtspStreamContext *rtsp_stream_create(const RtspStreamConfig *config)
{
    RtspStreamContext *context;

    if (config == NULL || config->session_id < 0 || config->frame_rate <= 0 ||
        config->queue_capacity < 2U) {
        return NULL;
    }

    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    context->config = *config;
    context->queue = calloc(config->queue_capacity, sizeof(*context->queue));
    if (context->queue == NULL) {
        free(context);
        return NULL;
    }
    if (pthread_mutex_init(&context->mutex, NULL) != 0) {
        free(context->queue);
        free(context);
        return NULL;
    }
    context->mutex_initialized = 1;
    if (pthread_cond_init(&context->frame_available, NULL) != 0) {
        pthread_mutex_destroy(&context->mutex);
        free(context->queue);
        free(context);
        return NULL;
    }
    context->condition_initialized = 1;
    return context;
}

int rtsp_stream_start(RtspStreamContext *context)
{
    RtspServerConfig server_config;

    if (context == NULL || context->thread_started) {
        return -1;
    }
    memset(&server_config, 0, sizeof(server_config));
    server_config.net_type = context->config.net_type;
    server_config.frame_rate = context->config.frame_rate;

    if (rtsp_server_open(context->config.session_id, &server_config) != 0) {
        aloge("[RTSP] Open server failed");
        return -1;
    }
    context->server_opened = 1;
    if (rtsp_server_start(context->config.session_id) != 0) {
        aloge("[RTSP] Start server failed");
        rtsp_stream_stop(context);
        return -1;
    }
    context->server_started = 1;
    context->stop_requested = 0;
    if (pthread_create(&context->thread, NULL, rtsp_sender_thread, context) != 0) {
        aloge("[RTSP] Create sender thread failed");
        rtsp_stream_stop(context);
        return -1;
    }
    context->thread_started = 1;
    alogd("[RTSP] Video service started: session=%d, queue=%u",
          context->config.session_id,
          context->config.queue_capacity);
    return 0;
}

int rtsp_stream_push_h264(RtspStreamContext *context,
                          const unsigned char *header,
                          size_t header_size,
                          const unsigned char *data0,
                          size_t size0,
                          const unsigned char *data1,
                          size_t size1,
                          const unsigned char *data2,
                          size_t size2,
                          uint64_t pts,
                          int key_frame)
{
    RtspVideoFrame frame;
    unsigned char *destination;
    size_t total_size = size0 + size1 + size2;

    if (context == NULL || !context->thread_started || total_size == 0U) {
        return -1;
    }
    if ((size0 > 0U && data0 == NULL) ||
        (size1 > 0U && data1 == NULL) ||
        (size2 > 0U && data2 == NULL) ||
        (key_frame && header_size > 0U && header == NULL)) {
        return -1;
    }
    if (key_frame) {
        total_size += header_size;
    }
    if (total_size > 0xffffffffU) {
        return -1;
    }

    memset(&frame, 0, sizeof(frame));
    frame.data = malloc(total_size);
    if (frame.data == NULL) {
        ++context->frames_dropped;
        return -1;
    }
    destination = frame.data;
    if (key_frame && header != NULL && header_size > 0U) {
        memcpy(destination, header, header_size);
        destination += header_size;
    }
    if (size0 > 0U) {
        memcpy(destination, data0, size0);
        destination += size0;
    }
    if (size1 > 0U) {
        memcpy(destination, data1, size1);
        destination += size1;
    }
    if (size2 > 0U) {
        memcpy(destination, data2, size2);
    }
    frame.size = (unsigned int)total_size;
    frame.pts = pts;
    frame.key_frame = key_frame;

    pthread_mutex_lock(&context->mutex);
    if (context->stop_requested) {
        pthread_mutex_unlock(&context->mutex);
        release_frame(&frame);
        return -1;
    }
    if (context->count == context->config.queue_capacity) {
        release_frame(&context->queue[context->head]);
        context->head = (context->head + 1U) % context->config.queue_capacity;
        --context->count;
        ++context->frames_dropped;
    }
    context->queue[context->tail] = frame;
    context->tail = (context->tail + 1U) % context->config.queue_capacity;
    ++context->count;
    ++context->frames_queued;
    pthread_cond_signal(&context->frame_available);
    pthread_mutex_unlock(&context->mutex);
    return 0;
}

int rtsp_stream_stop(RtspStreamContext *context)
{
    unsigned int index;

    if (context == NULL) {
        return -1;
    }
    if (context->thread_started) {
        pthread_mutex_lock(&context->mutex);
        context->stop_requested = 1;
        pthread_cond_broadcast(&context->frame_available);
        pthread_mutex_unlock(&context->mutex);
        pthread_join(context->thread, NULL);
        context->thread_started = 0;
    }
    if (context->server_started) {
        rtsp_server_stop(context->config.session_id);
        context->server_started = 0;
    }
    if (context->server_opened) {
        rtsp_server_close(context->config.session_id);
        context->server_opened = 0;
    }

    if (context->mutex_initialized) {
        pthread_mutex_lock(&context->mutex);
        for (index = 0; index < context->config.queue_capacity; ++index) {
            release_frame(&context->queue[index]);
        }
        context->head = 0;
        context->tail = 0;
        context->count = 0;
        pthread_mutex_unlock(&context->mutex);
    }
    return 0;
}

void rtsp_stream_destroy(RtspStreamContext *context)
{
    if (context == NULL) {
        return;
    }
    rtsp_stream_stop(context);
    if (context->condition_initialized) {
        pthread_cond_destroy(&context->frame_available);
    }
    if (context->mutex_initialized) {
        pthread_mutex_destroy(&context->mutex);
    }
    free(context->queue);
    free(context);
}
