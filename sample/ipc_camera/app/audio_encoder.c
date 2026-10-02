#include "audio_encoder.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <media/mpi_aenc.h>
#include <media/mpi_ai.h>
#include <media/mpi_sys.h>
#include <utils/plat_log.h>

struct AudioEncoderContext {
    AudioEncoderConfig config;
    char output_path[256];
    FILE *output_file;
    pthread_t stream_thread;
    volatile int stop_requested;

    /*
     * 每创建一个 MPP 资源就记录其状态。这样无论在哪一步失败，
     * audio_encoder_stop() 都只销毁确实已经创建成功的资源。
     */
    int ai_device_enabled;
    int ai_channel_created;
    int aenc_channel_created;
    int channels_bound;
    int ai_channel_enabled;
    int aenc_receiving;
    int thread_started;

    unsigned long long encoded_frames;
    unsigned long long encoded_bytes;
    unsigned long long first_pts;
    unsigned long long last_pts;
};

/* 填写 AI 和 AENC 两端在 MPP 中的通道标识，供 Bind/UnBind 共用。 */
static void make_mpp_channels(const AudioEncoderContext *encoder,
                              MPP_CHN_S *ai_channel,
                              MPP_CHN_S *aenc_channel)
{
    ai_channel->mModId = MOD_ID_AI;
    ai_channel->mDevId = encoder->config.ai_device;
    ai_channel->mChnId = encoder->config.ai_channel;

    aenc_channel->mModId = MOD_ID_AENC;
    aenc_channel->mDevId = 0;
    aenc_channel->mChnId = encoder->config.aenc_channel;
}

/*
 * AENC 输出线程只处理已经编码好的 AAC 数据。
 * 原始 PCM 由 MPP Bind 在 AI 和 AENC 之间内部传递，所以应用不再调用
 * AI_GetFrame/ReleaseFrame，也不需要复制每一帧 PCM。
 */
static void *audio_encoder_stream_thread(void *argument)
{
    AudioEncoderContext *encoder = argument;
    unsigned int consecutive_failures = 0;

    alogd("[AENC] Stream thread started");
    while (!encoder->stop_requested) {
        AUDIO_STREAM_S stream;
        ERRORTYPE ret;

        memset(&stream, 0, sizeof(stream));
        ret = AW_MPI_AENC_GetStream(encoder->config.aenc_channel,
                                    &stream,
                                    encoder->config.timeout_ms);
        if (ret != SUCCESS) {
            if (encoder->stop_requested) {
                break;
            }
            ++consecutive_failures;
            /* 只偶尔报告连续超时，避免没有声音数据时刷满终端。 */
            if (consecutive_failures == 1U ||
                (consecutive_failures % 50U) == 0U) {
                alogw("[AENC] GetStream failed: ret=%d, consecutive=%u",
                      ret,
                      consecutive_failures);
            }
            continue;
        }

        consecutive_failures = 0;
        if (encoder->encoded_frames == 0ULL) {
            encoder->first_pts = stream.mTimeStamp;
        }
        encoder->last_pts = stream.mTimeStamp;

        /*
         * attachAACHeader=1 时，每个 pStream 都带 ADTS 头。按顺序直接拼接
         * 即可得到 ffplay/VLC 能识别的 .aac 文件。
         */
        if (stream.pStream == NULL || stream.mLen == 0U ||
            fwrite(stream.pStream, 1, stream.mLen, encoder->output_file) !=
                stream.mLen) {
            aloge("[AENC] Write AAC file failed: %s", encoder->output_path);
            encoder->stop_requested = 1;
        } else {
            ++encoder->encoded_frames;
            encoder->encoded_bytes += stream.mLen;
            if (encoder->encoded_frames == 1ULL ||
                (encoder->encoded_frames % 100ULL) == 0ULL) {
                alogd("[AENC] Frame=%llu, id=%d, bytes=%u, pts=%llu us",
                      encoder->encoded_frames,
                      stream.mId,
                      stream.mLen,
                      (unsigned long long)stream.mTimeStamp);
                fflush(encoder->output_file);
            }

            /*
             * 消费回调必须发生在ReleaseStream之前。RTSP会立即深拷贝，
             * MP4也深拷贝后异步送流，因此归还AENC缓冲后两者都能安全工作。
             */
            if (encoder->config.frame_callback != NULL &&
                encoder->config.frame_callback(
                    encoder->config.frame_callback_opaque,
                    &stream) != 0) {
                alogw("[AENC] Encoded-frame consumer rejected frame: id=%d",
                      stream.mId);
            }
        }

        /* GetStream 成功后必须归还码流，否则 AENC 输出缓冲会耗尽。 */
        ret = AW_MPI_AENC_ReleaseStream(encoder->config.aenc_channel, &stream);
        if (ret != SUCCESS) {
            aloge("[AENC] ReleaseStream failed: ret=%d", ret);
        }
    }

    fflush(encoder->output_file);
    alogd("[AENC] Stream thread stopped: frames=%llu, bytes=%llu, "
          "first_pts=%llu us, last_pts=%llu us",
          encoder->encoded_frames,
          encoder->encoded_bytes,
          encoder->first_pts,
          encoder->last_pts);
    return NULL;
}

AudioEncoderContext *audio_encoder_create(const AudioEncoderConfig *config)
{
    AudioEncoderContext *encoder;

    /* 当前阶段只开放已经在阶段 6.1 实机验证过的 PCM 输入格式。 */
    if (config == NULL || config->ai_device < 0 || config->ai_channel < 0 ||
        config->aenc_channel < 0 || config->sample_rate <= 0 ||
        config->bit_width != 16 || config->channels != 1 ||
        config->samples_per_frame <= 0 || config->bit_rate < 0 ||
        config->timeout_ms < 0 || config->output_path == NULL) {
        return NULL;
    }

    encoder = calloc(1, sizeof(*encoder));
    if (encoder == NULL) {
        return NULL;
    }
    encoder->config = *config;
    snprintf(encoder->output_path,
             sizeof(encoder->output_path),
             "%s",
             config->output_path);
    encoder->config.output_path = encoder->output_path;
    return encoder;
}

int audio_encoder_start(AudioEncoderContext *encoder)
{
    AIO_ATTR_S ai_attributes;
    AENC_CHN_ATTR_S aenc_attributes;
    MPP_CHN_S ai_channel;
    MPP_CHN_S aenc_channel;
    ERRORTYPE ret;
    int thread_ret;

    if (encoder == NULL || encoder->thread_started) {
        return -1;
    }

    /* AI 参数必须与 AENC 输入参数完全一致，否则编码速度或音调会异常。 */
    memset(&ai_attributes, 0, sizeof(ai_attributes));
    ai_attributes.enSamplerate =
        (AUDIO_SAMPLE_RATE_E)encoder->config.sample_rate;
    ai_attributes.enBitwidth = AUDIO_BIT_WIDTH_16;
    ai_attributes.enWorkmode = AIO_MODE_I2S_MASTER;
    ai_attributes.enSoundmode = AUDIO_SOUND_MODE_MONO;
    ai_attributes.u32FrmNum = 5;
    ai_attributes.mPtNumPerFrm =
        (unsigned int)encoder->config.samples_per_frame;
    ai_attributes.mChnCnt = (unsigned int)encoder->config.channels;
    ai_attributes.mPcmCardId = PCM_CARD_TYPE_AUDIOCODEC;
    ai_attributes.mMicNum = 1;

    ret = AW_MPI_AI_SetPubAttr(encoder->config.ai_device, &ai_attributes);
    if (ret != SUCCESS) {
        aloge("[AENC] Set AI attributes failed: ret=%d", ret);
        return -1;
    }
    ret = AW_MPI_AI_Enable(encoder->config.ai_device);
    if (ret != SUCCESS) {
        aloge("[AENC] Enable AI device failed: ret=%d", ret);
        return -1;
    }
    encoder->ai_device_enabled = 1;

    ret = AW_MPI_AI_SetDevVolume(encoder->config.ai_device,
                                 encoder->config.volume);
    if (ret != SUCCESS) {
        aloge("[AENC] Set AI volume failed: ret=%d", ret);
        goto error;
    }
    ret = AW_MPI_AI_CreateChn(encoder->config.ai_device,
                              encoder->config.ai_channel,
                              NULL);
    if (ret != SUCCESS) {
        aloge("[AENC] Create AI channel failed: ret=%d", ret);
        goto error;
    }
    encoder->ai_channel_created = 1;

    /* PT_AAC + attachAACHeader=1 表示输出带 ADTS 帧头的 AAC-LC 码流。 */
    memset(&aenc_attributes, 0, sizeof(aenc_attributes));
    aenc_attributes.AeAttr.Type = PT_AAC;
    aenc_attributes.AeAttr.sampleRate = encoder->config.sample_rate;
    aenc_attributes.AeAttr.channels = encoder->config.channels;
    aenc_attributes.AeAttr.bitRate = encoder->config.bit_rate;
    aenc_attributes.AeAttr.bitsPerSample = encoder->config.bit_width;
    aenc_attributes.AeAttr.attachAACHeader = 1;

    ret = AW_MPI_AENC_CreateChn(encoder->config.aenc_channel,
                                &aenc_attributes);
    if (ret != SUCCESS) {
        aloge("[AENC] Create AAC channel failed: ret=%d", ret);
        goto error;
    }
    encoder->aenc_channel_created = 1;

    make_mpp_channels(encoder, &ai_channel, &aenc_channel);
    ret = AW_MPI_SYS_Bind(&ai_channel, &aenc_channel);
    if (ret != SUCCESS) {
        aloge("[AENC] Bind AI to AENC failed: ret=%d", ret);
        goto error;
    }
    encoder->channels_bound = 1;

    encoder->output_file = fopen(encoder->output_path, "wb");
    if (encoder->output_file == NULL) {
        aloge("[AENC] Open AAC output failed: %s", encoder->output_path);
        goto error;
    }

    ret = AW_MPI_AI_EnableChn(encoder->config.ai_device,
                              encoder->config.ai_channel);
    if (ret != SUCCESS) {
        aloge("[AENC] Enable AI channel failed: ret=%d", ret);
        goto error;
    }
    encoder->ai_channel_enabled = 1;

    ret = AW_MPI_AENC_StartRecvPcm(encoder->config.aenc_channel);
    if (ret != SUCCESS) {
        aloge("[AENC] Start receiving PCM failed: ret=%d", ret);
        goto error;
    }
    encoder->aenc_receiving = 1;

    encoder->stop_requested = 0;
    thread_ret = pthread_create(&encoder->stream_thread,
                                NULL,
                                audio_encoder_stream_thread,
                                encoder);
    if (thread_ret != 0) {
        aloge("[AENC] Create stream thread failed: ret=%d", thread_ret);
        goto error;
    }
    encoder->thread_started = 1;

    alogd("[AENC] AAC encoder started: ai=%d/%d, aenc=%d, "
          "%d Hz, %d bit, %d channel, ADTS=1, file=%s",
          encoder->config.ai_device,
          encoder->config.ai_channel,
          encoder->config.aenc_channel,
          encoder->config.sample_rate,
          encoder->config.bit_width,
          encoder->config.channels,
          encoder->output_path);
    return 0;

error:
    audio_encoder_stop(encoder);
    return -1;
}

int audio_encoder_stop(AudioEncoderContext *encoder)
{
    MPP_CHN_S ai_channel;
    MPP_CHN_S aenc_channel;
    ERRORTYPE ret;
    int result = 0;
    int thread_ret;

    if (encoder == NULL) {
        return -1;
    }

    /*
     * GetStream等待有timeout_ms上限；消费者也必须及时返回（MUX已改异步）。
     * 这不代表驱动/文件IO具有硬超时；join前保留正在使用的码流。
     */
    encoder->stop_requested = 1;
    if (encoder->thread_started) {
        thread_ret = pthread_join(encoder->stream_thread, NULL);
        if (thread_ret != 0) {
            aloge("[AENC] Join stream thread failed: ret=%d", thread_ret);
            result = -1;
        }
        encoder->thread_started = 0;
    }

    if (encoder->ai_channel_enabled) {
        ret = AW_MPI_AI_DisableChn(encoder->config.ai_device,
                                   encoder->config.ai_channel);
        if (ret != SUCCESS) {
            aloge("[AENC] Disable AI channel failed: ret=%d", ret);
            result = -1;
        }
        encoder->ai_channel_enabled = 0;
    }
    if (encoder->aenc_receiving) {
        ret = AW_MPI_AENC_StopRecvPcm(encoder->config.aenc_channel);
        if (ret != SUCCESS) {
            aloge("[AENC] Stop receiving PCM failed: ret=%d", ret);
            result = -1;
        }
        encoder->aenc_receiving = 0;
    }

    if (encoder->channels_bound) {
        make_mpp_channels(encoder, &ai_channel, &aenc_channel);
        ret = AW_MPI_SYS_UnBind(&ai_channel, &aenc_channel);
        if (ret != SUCCESS) {
            aloge("[AENC] Unbind AI from AENC failed: ret=%d", ret);
            result = -1;
        }
        encoder->channels_bound = 0;
    }

    if (encoder->ai_channel_created) {
        ret = AW_MPI_AI_ResetChn(encoder->config.ai_device,
                                 encoder->config.ai_channel);
        if (ret != SUCCESS) {
            aloge("[AENC] Reset AI channel failed: ret=%d", ret);
            result = -1;
        }
        ret = AW_MPI_AI_DestroyChn(encoder->config.ai_device,
                                   encoder->config.ai_channel);
        if (ret != SUCCESS) {
            aloge("[AENC] Destroy AI channel failed: ret=%d", ret);
            result = -1;
        }
        encoder->ai_channel_created = 0;
    }
    if (encoder->ai_device_enabled) {
        ret = AW_MPI_AI_Disable(encoder->config.ai_device);
        if (ret != SUCCESS) {
            aloge("[AENC] Disable AI device failed: ret=%d", ret);
            result = -1;
        }
        encoder->ai_device_enabled = 0;
    }

    if (encoder->aenc_channel_created) {
        ret = AW_MPI_AENC_ResetChn(encoder->config.aenc_channel);
        if (ret != SUCCESS) {
            aloge("[AENC] Reset AAC channel failed: ret=%d", ret);
            result = -1;
        }
        ret = AW_MPI_AENC_DestroyChn(encoder->config.aenc_channel);
        if (ret != SUCCESS) {
            aloge("[AENC] Destroy AAC channel failed: ret=%d", ret);
            result = -1;
        }
        encoder->aenc_channel_created = 0;
    }

    if (encoder->output_file != NULL) {
        fflush(encoder->output_file);
        fclose(encoder->output_file);
        encoder->output_file = NULL;
    }

    alogd("[AENC] AAC encoder stopped: frames=%llu, bytes=%llu",
          encoder->encoded_frames,
          encoder->encoded_bytes);
    return result;
}

void audio_encoder_destroy(AudioEncoderContext *encoder)
{
    if (encoder == NULL) {
        return;
    }
    /* destroy 也能回收只完成部分初始化的上下文。 */
    if (encoder->thread_started || encoder->aenc_receiving ||
        encoder->ai_channel_enabled || encoder->channels_bound ||
        encoder->aenc_channel_created || encoder->ai_channel_created ||
        encoder->ai_device_enabled || encoder->output_file != NULL) {
        audio_encoder_stop(encoder);
    }
    free(encoder);
}
