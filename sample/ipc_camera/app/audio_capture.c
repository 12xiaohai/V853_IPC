#include "audio_capture.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <media/mpi_ai.h>
#include <utils/plat_log.h>

struct AudioCaptureContext {
    AudioCaptureConfig config;
    char output_path[256];       /* 保存路径副本，避免外部字符串失效。 */
    FILE *output_file;
    pthread_t thread;
    volatile int stop_requested;

    /* 每成功创建一个资源就置 1，使 stop 能处理部分初始化失败。 */
    int device_enabled;
    int channel_created;
    int channel_enabled;
    int thread_started;

    unsigned long long captured_frames;
    unsigned long long captured_bytes;
    unsigned long long first_pts;
    unsigned long long last_pts;
};

/*
 * AI 工作线程：从 Audio Input 通道取得 PCM 帧，写入文件后立即归还。
 * GetFrame 成功与 ReleaseFrame 必须一一对应，否则 AI 内部缓冲会逐渐耗尽。
 */
static void *audio_capture_thread(void *argument)
{
    AudioCaptureContext *capture = argument;
    unsigned int consecutive_failures = 0;

    alogd("[AI] Capture thread started");
    while (!capture->stop_requested) {
        AUDIO_FRAME_S frame;
        ERRORTYPE ret;

        memset(&frame, 0, sizeof(frame));
        ret = AW_MPI_AI_GetFrame(capture->config.device,
                                 capture->config.channel,
                                 &frame,
                                 NULL,
                                 capture->config.timeout_ms);
        if (ret != SUCCESS) {
            if (capture->stop_requested) {
                break;
            }
            ++consecutive_failures;
            /* 异常时限制日志频率，防止连续超时刷屏。 */
            if (consecutive_failures == 1U ||
                (consecutive_failures % 50U) == 0U) {
                alogw("[AI] GetFrame failed: ret=%d, consecutive=%u",
                      ret,
                      consecutive_failures);
            }
            continue;
        }

        consecutive_failures = 0;
        if (capture->captured_frames == 0ULL) {
            capture->first_pts = frame.mTimeStamp;
        }
        capture->last_pts = frame.mTimeStamp;

        /*
         * 单声道时 mLen 就是当前帧的字节数。阶段 6.1 保存裸 PCM，
         * 文件本身不包含采样率/位宽等头信息，播放时需要手动指定。
         */
        if (frame.mpAddr == NULL || frame.mLen == 0U ||
            fwrite(frame.mpAddr, 1, frame.mLen, capture->output_file) !=
                frame.mLen) {
            aloge("[AI] Write PCM file failed: %s", capture->output_path);
            capture->stop_requested = 1;
        } else {
            ++capture->captured_frames;
            capture->captured_bytes += frame.mLen;
            if (capture->captured_frames == 1ULL ||
                (capture->captured_frames % 100ULL) == 0ULL) {
                alogd("[AI] Frame=%llu, seq=%u, bytes=%u, pts=%llu us",
                      capture->captured_frames,
                      frame.mSeq,
                      frame.mLen,
                      (unsigned long long)frame.mTimeStamp);
                fflush(capture->output_file);
            }
        }

        ret = AW_MPI_AI_ReleaseFrame(capture->config.device,
                                     capture->config.channel,
                                     &frame,
                                     NULL);
        if (ret != SUCCESS) {
            aloge("[AI] ReleaseFrame failed: ret=%d, seq=%u", ret, frame.mSeq);
        }
    }

    fflush(capture->output_file);
    alogd("[AI] Capture thread stopped: frames=%llu, bytes=%llu, "
          "first_pts=%llu us, last_pts=%llu us",
          capture->captured_frames,
          capture->captured_bytes,
          capture->first_pts,
          capture->last_pts);
    return NULL;
}

AudioCaptureContext *audio_capture_create(const AudioCaptureConfig *config)
{
    AudioCaptureContext *capture;

    /* 阶段 6.1 只验证原项目的 16-bit 单声道参数，避免引入未验证分支。 */
    if (config == NULL || config->device < 0 || config->channel < 0 ||
        config->sample_rate <= 0 || config->bit_width != 16 ||
        config->channels != 1 || config->samples_per_frame <= 0 ||
        config->timeout_ms < 0 || config->output_path == NULL) {
        return NULL;
    }

    capture = calloc(1, sizeof(*capture));
    if (capture == NULL) {
        return NULL;
    }
    capture->config = *config;
    snprintf(capture->output_path,
             sizeof(capture->output_path),
             "%s",
             config->output_path);
    capture->config.output_path = capture->output_path;
    return capture;
}

int audio_capture_start(AudioCaptureContext *capture)
{
    AIO_ATTR_S attributes;
    AI_CHN_PARAM_S channel_parameters;
    ERRORTYPE ret;
    int thread_ret;

    if (capture == NULL || capture->thread_started) {
        return -1;
    }

    memset(&attributes, 0, sizeof(attributes));
    attributes.enSamplerate = (AUDIO_SAMPLE_RATE_E)capture->config.sample_rate;
    attributes.enBitwidth = AUDIO_BIT_WIDTH_16;
    attributes.enWorkmode = AIO_MODE_I2S_MASTER;
    attributes.enSoundmode = AUDIO_SOUND_MODE_MONO;
    attributes.u32FrmNum = 5;
    attributes.mPtNumPerFrm = (unsigned int)capture->config.samples_per_frame;
    attributes.mChnCnt = (unsigned int)capture->config.channels;
    attributes.mPcmCardId = PCM_CARD_TYPE_AUDIOCODEC;
    attributes.mMicNum = 1;

    /* 公共属性必须在 Enable 设备和 CreateChn 之前设置。 */
    ret = AW_MPI_AI_SetPubAttr(capture->config.device, &attributes);
    if (ret != SUCCESS) {
        aloge("[AI] SetPubAttr failed: ret=%d", ret);
        return -1;
    }

    ret = AW_MPI_AI_Enable(capture->config.device);
    if (ret != SUCCESS) {
        aloge("[AI] Enable device failed: ret=%d", ret);
        return -1;
    }
    capture->device_enabled = 1;

    ret = AW_MPI_AI_SetDevVolume(capture->config.device,
                                 capture->config.volume);
    if (ret != SUCCESS) {
        aloge("[AI] Set device volume failed: ret=%d", ret);
        goto error;
    }

    ret = AW_MPI_AI_CreateChn(capture->config.device,
                              capture->config.channel,
                              NULL);
    if (ret != SUCCESS) {
        aloge("[AI] Create channel failed: ret=%d", ret);
        goto error;
    }
    capture->channel_created = 1;

    /* 允许用户态最多持有 5 帧；本线程每帧写完后会立即归还。 */
    memset(&channel_parameters, 0, sizeof(channel_parameters));
    channel_parameters.u32UsrFrmDepth = 5;
    ret = AW_MPI_AI_SetChnParam(capture->config.device,
                                capture->config.channel,
                                &channel_parameters);
    if (ret != SUCCESS) {
        aloge("[AI] Set channel parameters failed: ret=%d", ret);
        goto error;
    }

    capture->output_file = fopen(capture->output_path, "wb");
    if (capture->output_file == NULL) {
        aloge("[AI] Open PCM output failed: %s", capture->output_path);
        goto error;
    }

    ret = AW_MPI_AI_EnableChn(capture->config.device,
                              capture->config.channel);
    if (ret != SUCCESS) {
        aloge("[AI] Enable channel failed: ret=%d", ret);
        goto error;
    }
    capture->channel_enabled = 1;

    capture->stop_requested = 0;
    thread_ret = pthread_create(&capture->thread,
                                NULL,
                                audio_capture_thread,
                                capture);
    if (thread_ret != 0) {
        aloge("[AI] Create capture thread failed: ret=%d", thread_ret);
        goto error;
    }
    capture->thread_started = 1;

    alogd("[AI] PCM capture started: dev=%d, chn=%d, %d Hz, %d bit, "
          "%d channel, frame_samples=%d, file=%s",
          capture->config.device,
          capture->config.channel,
          capture->config.sample_rate,
          capture->config.bit_width,
          capture->config.channels,
          capture->config.samples_per_frame,
          capture->output_path);
    return 0;

error:
    audio_capture_stop(capture);
    return -1;
}

int audio_capture_stop(AudioCaptureContext *capture)
{
    int result = 0;
    int thread_ret;
    ERRORTYPE ret;

    if (capture == NULL) {
        return -1;
    }

    /*
     * 先设置退出标志并等待线程结束。GetFrame 的超时只有 200 ms，
     * 因此 join 最多只需等待一个很短的取帧周期。在线程退出前不能先
     * DisableChn，否则线程可能还在归还最后一帧，造成资源并发销毁。
     */
    capture->stop_requested = 1;
    if (capture->thread_started) {
        thread_ret = pthread_join(capture->thread, NULL);
        if (thread_ret != 0) {
            aloge("[AI] Join capture thread failed: ret=%d", thread_ret);
            result = -1;
        }
        capture->thread_started = 0;
    }

    if (capture->channel_enabled) {
        ret = AW_MPI_AI_DisableChn(capture->config.device,
                                   capture->config.channel);
        if (ret != SUCCESS) {
            aloge("[AI] Disable channel failed: ret=%d", ret);
            result = -1;
        }
        capture->channel_enabled = 0;
    }

    if (capture->channel_created) {
        ret = AW_MPI_AI_ResetChn(capture->config.device,
                                 capture->config.channel);
        if (ret != SUCCESS) {
            aloge("[AI] Reset channel failed: ret=%d", ret);
            result = -1;
        }
        ret = AW_MPI_AI_DestroyChn(capture->config.device,
                                   capture->config.channel);
        if (ret != SUCCESS) {
            aloge("[AI] Destroy channel failed: ret=%d", ret);
            result = -1;
        }
        capture->channel_created = 0;
    }

    if (capture->device_enabled) {
        ret = AW_MPI_AI_Disable(capture->config.device);
        if (ret != SUCCESS) {
            aloge("[AI] Disable device failed: ret=%d", ret);
            result = -1;
        }
        capture->device_enabled = 0;
    }

    if (capture->output_file != NULL) {
        fflush(capture->output_file);
        fclose(capture->output_file);
        capture->output_file = NULL;
    }

    alogd("[AI] PCM capture stopped: frames=%llu, bytes=%llu",
          capture->captured_frames,
          capture->captured_bytes);
    return result;
}

void audio_capture_destroy(AudioCaptureContext *capture)
{
    if (capture == NULL) {
        return;
    }
    /* destroy 可安全处理调用者忘记 stop 或 start 只成功一部分的情况。 */
    if (capture->thread_started || capture->channel_enabled ||
        capture->channel_created || capture->device_enabled ||
        capture->output_file != NULL) {
        audio_capture_stop(capture);
    }
    free(capture);
}
