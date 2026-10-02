#define _POSIX_C_SOURCE 200809L

#include "audio_alarm.h"
#include "wav_reader.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mpi_ao.h>
#include <utils/plat_log.h>

#define ALARM_QUEUE_SIZE 8U
#define ALARM_FRAME_SAMPLES 1024U
#define ALARM_RELEASE_TIMEOUT_MS 2000U
#define ALARM_DRAIN_TIMEOUT_MS 3000U

struct AudioAlarmContext {
    AudioAlarmConfig config;
    char path[512];
    WavPcm pcm;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    int thread_started;
    int stopping;
    int disabled;
    int playing;
    int channel_created;
    int channel_started;
    int frame_pending;
    int eof_received;
    AUDIO_FRAME_S frame; /* AO异步消费时，描述符和PCM都必须保持有效。 */
    AudioAlarmEvent queue[ALARM_QUEUE_SIZE];
    unsigned int head, count;
    uint64_t next_allowed_ms;
    unsigned long long queued, suppressed, completed, failed, interrupted;
};

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* condition也绑定单调时钟，NTP大幅校时不会拉长冷却或回调等待。 */
static struct timespec deadline_after(unsigned int milliseconds)
{
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += milliseconds / 1000U;
    deadline.tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static int is_stopping(AudioAlarmContext *context)
{
    int stopping;
    pthread_mutex_lock(&context->mutex);
    stopping = context->stopping;
    pthread_mutex_unlock(&context->mutex);
    return stopping;
}

static ERRORTYPE alarm_ao_callback(void *opaque, MPP_CHN_S *channel,
                                  MPP_EVENT_TYPE event, void *data)
{
    AudioAlarmContext *context = opaque;
    if (context == NULL || channel == NULL || channel->mModId != MOD_ID_AO ||
        channel->mDevId != context->config.ao_device ||
        channel->mChnId != context->config.ao_channel) {
        return SUCCESS;
    }
    /* 回调只更新标志并唤醒线程，不能在MPP回调里Stop/Destroy同一通道。 */
    pthread_mutex_lock(&context->mutex);
    if (event == MPP_EVENT_RELEASE_AUDIO_BUFFER && data != NULL) {
        const AUDIO_FRAME_S *released = data;
        if (context->frame_pending && released->mId == context->frame.mId) {
            context->frame_pending = 0;
        }
    } else if (event == MPP_EVENT_NOTIFY_EOF) {
        context->eof_received = 1;
    }
    pthread_cond_broadcast(&context->condition);
    pthread_mutex_unlock(&context->mutex);
    return SUCCESS;
}

/* 返回0=收到条件，1=用户停止，-1=超时/条件变量故障。 */
static int wait_for_audio(AudioAlarmContext *context, int wait_eof)
{
    struct timespec deadline = deadline_after(wait_eof ? ALARM_DRAIN_TIMEOUT_MS
                                                       : ALARM_RELEASE_TIMEOUT_MS);
    int result = 0;
    pthread_mutex_lock(&context->mutex);
    while (!context->stopping &&
           (wait_eof ? !context->eof_received : context->frame_pending)) {
        int error = pthread_cond_timedwait(&context->condition, &context->mutex,
                                           &deadline);
        if (error != 0) {
            result = -1;
            break;
        }
    }
    if (context->stopping) {
        result = 1;
    }
    pthread_mutex_unlock(&context->mutex);
    return result;
}

/* 不持应用锁调用MPP：Stop/Destroy可能等待或触发我们的回调。 */
static int close_channel(AudioAlarmContext *context)
{
    if (context->channel_started) {
        ERRORTYPE error = AW_MPI_AO_StopChn(context->config.ao_device,
                                           context->config.ao_channel);
        if (error != SUCCESS) {
            aloge("[ALARM] AO StopChn failed: ret=%d", error);
            return -1;
        }
        context->channel_started = 0;
    }
    if (context->channel_created) {
        ERRORTYPE error = AW_MPI_AO_DestroyChn(context->config.ao_device,
                                              context->config.ao_channel);
        if (error != SUCCESS) {
            aloge("[ALARM] AO DestroyChn failed: ret=%d", error);
            return -1;
        }
        context->channel_created = 0;
    }
    return 0;
}

static int open_channel(AudioAlarmContext *context)
{
    const int device = context->config.ao_device;
    const int channel = context->config.ao_channel;
    MPPCallbackInfo callback = {context, alarm_ao_callback};
    ERRORTYPE error = AW_MPI_AO_CreateChn(device, channel);
    if (error != SUCCESS) {
        /* 通道被占用时不能Stop/Destroy别人的资源。 */
        aloge("[ALARM] AO CreateChn failed: dev=%d ch=%d ret=%d",
              device, channel, error);
        return -1;
    }
    context->channel_created = 1;
    if (AW_MPI_AO_SetPcmCardType(device, channel, PCM_CARD_TYPE_AUDIOCODEC) != SUCCESS ||
        AW_MPI_AO_RegisterCallback(device, channel, &callback) != SUCCESS ||
        AW_MPI_AO_SetDevVolume(device, context->config.volume) != SUCCESS ||
        AW_MPI_AO_SetSoftVolume(device, 0) != SUCCESS ||
        AW_MPI_AO_SetChnMute(device, channel, FALSE) != SUCCESS ||
        AW_MPI_AO_StartChn(device, channel) != SUCCESS) {
        aloge("[ALARM] AO card/callback/volume/start configuration failed");
        return -1;
    }
    context->channel_started = 1;
    return 0;
}

static int play_pcm(AudioAlarmContext *context)
{
    size_t offset;
    if (is_stopping(context)) {
        return 1;
    }
    pthread_mutex_lock(&context->mutex);
    context->eof_received = 0;
    context->frame_pending = 0;
    pthread_mutex_unlock(&context->mutex);
    if (open_channel(context) != 0) {
        return -1;
    }

    for (offset = 0U; offset < context->pcm.size; offset += context->frame.mLen) {
        size_t bytes = context->pcm.size - offset;
        int result;
        if (is_stopping(context)) {
            return 1;
        }
        if (bytes > ALARM_FRAME_SAMPLES * 2U) {
            bytes = ALARM_FRAME_SAMPLES * 2U;
        }
        pthread_mutex_lock(&context->mutex);
        memset(&context->frame, 0, sizeof(context->frame));
        context->frame.mSamplerate = (AUDIO_SAMPLE_RATE_E)context->pcm.sample_rate;
        /* MPP的16bit枚举值是AUDIO_BIT_WIDTH_16，不是整数16！ */
        context->frame.mBitwidth = AUDIO_BIT_WIDTH_16;
        context->frame.mSoundmode = AUDIO_SOUND_MODE_MONO;
        context->frame.mpAddr = context->pcm.data + offset;
        context->frame.mLen = (unsigned int)bytes;
        context->frame.mSeq = context->frame.mId = (unsigned int)(offset / 2048U);
        context->frame.mTimeStamp = (unsigned long long)(offset / 2U) *
                                    1000000U / context->pcm.sample_rate;
        /* 先置pending再Send，兼容回调在Send返回前就执行的情况。 */
        context->frame_pending = 1;
        pthread_mutex_unlock(&context->mutex);

        if (AW_MPI_AO_SendFrame(context->config.ao_device,
                               context->config.ao_channel, &context->frame, 100) != SUCCESS) {
            aloge("[ALARM] AO SendFrame failed: offset=%lu", (unsigned long)offset);
            return -1;
        }
        result = wait_for_audio(context, 0);
        if (result != 0) {
            if (result < 0) {
                aloge("[ALARM] Audio-buffer release timeout/error");
            }
            return result;
        }
        /* 一次仅一个在途帧，收到释放后才复用描述符；PCM直到Destroy后才释放。 */
    }
    if (AW_MPI_AO_SetStreamEof(context->config.ao_device,
                              context->config.ao_channel, TRUE, TRUE) != SUCCESS) {
        aloge("[ALARM] AO SetStreamEof failed");
        return -1;
    }
    /* buffer释放只表示MPP不再持有输入；EOF排空确认才算本次播放完成。 */
    {
        int result = wait_for_audio(context, 1);
        if (result < 0) {
            aloge("[ALARM] EOF drain timeout/error");
        }
        return result;
    }
}

static void *alarm_thread(void *opaque)
{
    AudioAlarmContext *context = opaque;
    for (;;) {
        AudioAlarmEvent event;
        int result, close_result;
        pthread_mutex_lock(&context->mutex);
        while (!context->stopping && context->count == 0U) {
            pthread_cond_wait(&context->condition, &context->mutex);
        }
        if (context->stopping) {
            pthread_mutex_unlock(&context->mutex);
            break;
        }
        event = context->queue[context->head];
        /* 同一瞬间越线+入侵只播一次，丢弃积压事件而不延迟播放过时报警。 */
        context->suppressed += context->count - 1U;
        context->head = (context->head + context->count) % ALARM_QUEUE_SIZE;
        context->count = 0U;
        context->playing = 1; /* 在解锁前设置，消除多来源同时触发的竞争窗口。 */
        context->next_allowed_ms = monotonic_ms() + context->config.cooldown_ms;
        pthread_mutex_unlock(&context->mutex);

        alogd("[ALARM] Play started: source=%s track=%u region=%u seq=%llu pts=%llu",
              event.kind == AUDIO_ALARM_LINE_CROSSING ? "line" : "region-enter",
              event.track_id, event.region_id, event.sequence, event.frame_pts_us);
        result = play_pcm(context);
        close_result = close_channel(context);
        pthread_mutex_lock(&context->mutex);
        context->playing = 0;
        if (result == 0 && close_result == 0) {
            context->completed++;
        } else if (result == 1 && close_result == 0) {
            context->interrupted++;
        } else {
            context->failed++;
            context->disabled = 1; /* 故障隔离：不反复打开异常AO，也不停止摄像头。 */
        }
        pthread_mutex_unlock(&context->mutex);
        alogd("[ALARM] Play finished: result=%d close=%d (0=complete,1=interrupted)",
              result, close_result);
    }
    return NULL;
}

AudioAlarmContext *audio_alarm_create(const AudioAlarmConfig *config)
{
    AudioAlarmContext *context;
    pthread_condattr_t attributes;
    if (config == NULL || config->wav_path == NULL || config->wav_path[0] == '\0' ||
        strlen(config->wav_path) >= 512U || config->ao_device < 0 ||
        config->ao_channel < 0 || config->volume < 0 || config->volume > 100) {
        return NULL;
    }
    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    context->config = *config;
    strcpy(context->path, config->wav_path);
    context->config.wav_path = context->path;
    if (pthread_mutex_init(&context->mutex, NULL) != 0) {
        free(context);
        return NULL;
    }
    if (pthread_condattr_init(&attributes) != 0) {
        pthread_mutex_destroy(&context->mutex);
        free(context);
        return NULL;
    }
    if (pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&context->condition, &attributes) != 0) {
        pthread_condattr_destroy(&attributes);
        pthread_mutex_destroy(&context->mutex);
        free(context);
        return NULL;
    }
    pthread_condattr_destroy(&attributes);
    return context;
}

int audio_alarm_start(AudioAlarmContext *context)
{
    FILE *file;
    if (context == NULL || context->thread_started || context->channel_created) {
        return -1;
    }
    file = fopen(context->path, "rb");
    if (file == NULL) {
        aloge("[ALARM] Open WAV failed: path=%s errno=%d", context->path, errno);
        return -1;
    }
    if (wav_pcm_read(file, &context->pcm) != 0) {
        fclose(file);
        aloge("[ALARM] Invalid WAV: need RIFF PCM16 mono, 8..48 kHz, <=60s/4MiB");
        return -1;
    }
    fclose(file);
    context->stopping = context->disabled = context->playing = 0;
    context->head = context->count = 0U;
    context->next_allowed_ms = 0U;
    if (pthread_create(&context->thread, NULL, alarm_thread, context) != 0) {
        wav_pcm_free(&context->pcm);
        return -1;
    }
    context->thread_started = 1;
    alogd("[ALARM] Ready: file=%s rate=%u PCM16 mono bytes=%lu AO=%d/%d cooldown=%ums",
          context->path, context->pcm.sample_rate, (unsigned long)context->pcm.size,
          context->config.ao_device, context->config.ao_channel, context->config.cooldown_ms);
    return 0;
}

int audio_alarm_push(AudioAlarmContext *context, const AudioAlarmEvent *event)
{
    int result = 0;
    if (context == NULL || event == NULL ||
        (event->kind != AUDIO_ALARM_LINE_CROSSING && event->kind != AUDIO_ALARM_REGION_ENTER)) {
        return -1;
    }
    pthread_mutex_lock(&context->mutex);
    if (!context->thread_started || context->stopping || context->disabled) {
        result = -1;
    } else if (context->playing || monotonic_ms() < context->next_allowed_ms ||
               context->count == ALARM_QUEUE_SIZE) {
        context->suppressed++;
        result = 1;
    } else {
        context->queue[(context->head + context->count) % ALARM_QUEUE_SIZE] = *event;
        context->count++;
        context->queued++;
        pthread_cond_signal(&context->condition);
    }
    pthread_mutex_unlock(&context->mutex);
    return result;
}

int audio_alarm_stop(AudioAlarmContext *context)
{
    if (context == NULL) {
        return 0;
    }
    pthread_mutex_lock(&context->mutex);
    context->stopping = 1;
    context->suppressed += context->count;
    context->count = 0U;
    pthread_cond_broadcast(&context->condition);
    pthread_mutex_unlock(&context->mutex);
    if (context->thread_started) {
        if (pthread_join(context->thread, NULL) != 0) {
            return -1;
        }
        context->thread_started = 0;
        alogd("[ALARM] Stopped: queued=%llu completed=%llu suppressed=%llu failed=%llu interrupted=%llu",
              context->queued, context->completed, context->suppressed,
              context->failed, context->interrupted);
    }
    if (close_channel(context) != 0) {
        return -1;
    }
    wav_pcm_free(&context->pcm);
    return 0;
}

void audio_alarm_destroy(AudioAlarmContext *context)
{
    if (context == NULL) {
        return;
    }
    if (audio_alarm_stop(context) != 0) {
        /* 宁可保留缓冲到进程退出，也不能让MPP访问已经free的PCM或cookie。 */
        aloge("[ALARM] Retaining context: AO/thread cleanup failed");
        return;
    }
    pthread_cond_destroy(&context->condition);
    pthread_mutex_destroy(&context->mutex);
    free(context);
}
