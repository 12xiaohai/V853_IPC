#include "mp4_recorder.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <media/mpi_mux.h>
#include <utils/plat_log.h>

#define MP4_RECORDER_PATH_SIZE 256
#define MP4_SIMPLE_CACHE_SIZE (64 * 1024)
#define MP4_MAX_PACKS_PER_FRAME 8

struct Mp4RecorderContext {
    Mp4RecorderConfig config;
    char output_path[MP4_RECORDER_PATH_SIZE];
    unsigned char *h264_header;
    size_t h264_header_size;

    int output_fd;
    int mux_created;
    int mux_started;
    int accepting_frames;
    int waiting_for_key_frame;

    /* 视频线程和音频线程都会调用push，使用同一把锁串行访问MUX。 */
    pthread_mutex_t send_lock;
    int lock_initialized;

    unsigned long long video_frames;
    unsigned long long audio_frames;
    unsigned long long skipped_video_frames;
    unsigned long long skipped_audio_frames;
};

/* 根据配置填写MP4中视频轨和音频轨的媒体参数。 */
static void fill_mux_attributes(const Mp4RecorderContext *recorder,
                                MUX_CHN_ATTR_S *attributes)
{
    VideoAttr *video;

    memset(attributes, 0, sizeof(*attributes));
    attributes->mVideoAttrValidNum = 1;
    video = &attributes->mVideoAttr[0];
    video->mWidth = recorder->config.width;
    video->mHeight = recorder->config.height;
    video->mVideoFrmRate = recorder->config.frame_rate * 1000;
    video->mCreateTime = (int)time(NULL);
    video->mMaxKeyInterval = recorder->config.gop_size;
    video->mVideoEncodeType = PT_H264;
    video->mRotateDegree = 0;
    video->mVeChn = recorder->config.venc_channel;

    attributes->mChannels = recorder->config.audio_channels;
    /* MUX接口沿用MPP的AUDIO_BIT_WIDTH_E枚举，而不是直接填写数字16。 */
    attributes->mBitsPerSample = AUDIO_BIT_WIDTH_16;
    attributes->mSamplesPerFrame = recorder->config.samples_per_frame;
    attributes->mSampleRate = recorder->config.sample_rate;
    attributes->mAudioEncodeType = PT_AAC;

    attributes->mMuxerId = 0;
    attributes->mMediaFileFormat = MEDIA_FILE_FORMAT_MP4;
    attributes->mMaxFileDuration = 0;  /* 阶段8.1按Ctrl+C结束，不自动分段。 */
    attributes->mMaxFileSizeBytes = 0;
    attributes->mCallbackOutFlag = FALSE;
    attributes->mFsWriteMode = FSWRITEMODE_SIMPLECACHE;
    attributes->mSimpleCacheSize = MP4_SIMPLE_CACHE_SIZE;
    attributes->mAddRepairInfo = 0;
    attributes->mMaxFrmsTagInterval = 100000;
}

Mp4RecorderContext *mp4_recorder_create(const Mp4RecorderConfig *config)
{
    Mp4RecorderContext *recorder;
    size_t path_length;

    if (config == NULL || config->output_path == NULL ||
        config->h264_header == NULL || config->h264_header_size == 0U ||
        config->width <= 0 || config->height <= 0 ||
        config->frame_rate <= 0 || config->sample_rate <= 0 ||
        config->audio_channels <= 0 || config->samples_per_frame <= 0) {
        return NULL;
    }

    path_length = strlen(config->output_path);
    if (path_length == 0U || path_length >= MP4_RECORDER_PATH_SIZE) {
        return NULL;
    }

    recorder = calloc(1, sizeof(*recorder));
    if (recorder == NULL) {
        return NULL;
    }

    recorder->config = *config;
    memcpy(recorder->output_path, config->output_path, path_length + 1U);
    recorder->config.output_path = recorder->output_path;

    /* SPS/PPS来自VENC内部缓冲，录像上下文保存自己的副本。 */
    recorder->h264_header = malloc(config->h264_header_size);
    if (recorder->h264_header == NULL) {
        free(recorder);
        return NULL;
    }
    memcpy(recorder->h264_header,
           config->h264_header,
           config->h264_header_size);
    recorder->h264_header_size = config->h264_header_size;
    recorder->config.h264_header = recorder->h264_header;

    recorder->output_fd = -1;
    if (pthread_mutex_init(&recorder->send_lock, NULL) != 0) {
        free(recorder->h264_header);
        free(recorder);
        return NULL;
    }
    recorder->lock_initialized = 1;
    return recorder;
}

int mp4_recorder_start(Mp4RecorderContext *recorder)
{
    MUX_CHN_ATTR_S attributes;
    VencHeaderData header;
    ERRORTYPE ret;

    if (recorder == NULL || recorder->mux_started) {
        return -1;
    }

    /* O_TRUNC保证每次阶段测试都生成一个全新的MP4文件。 */
    recorder->output_fd = open(recorder->output_path,
                               O_RDWR | O_CREAT | O_TRUNC,
                               0666);
    if (recorder->output_fd < 0) {
        aloge("[MP4] Open output file failed: %s", recorder->output_path);
        return -1;
    }

    fill_mux_attributes(recorder, &attributes);
    ret = AW_MPI_MUX_CreateChn(recorder->config.mux_channel,
                               &attributes,
                               recorder->output_fd,
                               0);
    if (ret != SUCCESS) {
        aloge("[MP4] Create MUX channel failed: chn=%d, ret=%d",
              recorder->config.mux_channel,
              ret);
        goto error;
    }
    recorder->mux_created = 1;

    /* MP4的avcC解码配置需要SPS/PPS，必须在送入第一帧之前交给MUX。 */
    memset(&header, 0, sizeof(header));
    header.pBuffer = recorder->h264_header;
    header.nLength = (unsigned int)recorder->h264_header_size;
    ret = AW_MPI_MUX_SetH264SpsPpsInfo(recorder->config.mux_channel,
                                       recorder->config.venc_channel,
                                       &header);
    if (ret != SUCCESS) {
        aloge("[MP4] Set H.264 SPS/PPS failed: ret=%d", ret);
        goto error;
    }

    ret = AW_MPI_MUX_StartChn(recorder->config.mux_channel);
    if (ret != SUCCESS) {
        aloge("[MP4] Start MUX channel failed: ret=%d", ret);
        goto error;
    }
    recorder->mux_started = 1;

    pthread_mutex_lock(&recorder->send_lock);
    recorder->accepting_frames = 1;
    recorder->waiting_for_key_frame = 1;
    pthread_mutex_unlock(&recorder->send_lock);

    alogd("[MP4] Recorder started: mux=%d, video=%dx%d@%dfps, "
          "audio=%dHz/%dch, file=%s",
          recorder->config.mux_channel,
          recorder->config.width,
          recorder->config.height,
          recorder->config.frame_rate,
          recorder->config.sample_rate,
          recorder->config.audio_channels,
          recorder->output_path);
    return 0;

error:
    mp4_recorder_stop(recorder);
    return -1;
}

int mp4_recorder_push_video(Mp4RecorderContext *recorder,
                            const VENC_STREAM_S *stream,
                            int key_frame)
{
    VENC_STREAM_S local_stream;
    VENC_PACK_S local_packs[MP4_MAX_PACKS_PER_FRAME];
    ERRORTYPE ret;
    int result = 0;

    if (recorder == NULL || stream == NULL || stream->mpPack == NULL ||
        stream->mPackCount == 0U ||
        stream->mPackCount > MP4_MAX_PACKS_PER_FRAME) {
        return -1;
    }

    pthread_mutex_lock(&recorder->send_lock);
    if (!recorder->accepting_frames) {
        pthread_mutex_unlock(&recorder->send_lock);
        return 0;
    }

    /* MP4必须从可独立解码的IDR帧开始，启动期间的P帧直接丢弃。 */
    if (recorder->waiting_for_key_frame && !key_frame) {
        ++recorder->skipped_video_frames;
        pthread_mutex_unlock(&recorder->send_lock);
        return 0;
    }
    if (recorder->waiting_for_key_frame) {
        recorder->waiting_for_key_frame = 0;
        alogd("[MP4] First H.264 key frame received: pts=%llu us",
              (unsigned long long)stream->mpPack[0].mPTS);
    }

    /* 复制的是描述符，不复制码流；Sync接口返回前原始缓冲始终有效。 */
    local_stream = *stream;
    memcpy(local_packs,
           stream->mpPack,
           stream->mPackCount * sizeof(local_packs[0]));
    local_stream.mpPack = local_packs;
    ret = AW_MPI_MUX_SendVideoStreamSync(recorder->config.mux_channel,
                                         &local_stream,
                                         0);
    if (ret != SUCCESS) {
        aloge("[MP4] Send video stream failed: seq=%u, ret=%d",
              stream->mSeq,
              ret);
        result = -1;
    } else {
        ++recorder->video_frames;
    }
    pthread_mutex_unlock(&recorder->send_lock);
    return result;
}

int mp4_recorder_push_audio(Mp4RecorderContext *recorder,
                            const AUDIO_STREAM_S *stream)
{
    AUDIO_STREAM_S local_stream;
    ERRORTYPE ret;
    int result = 0;

    if (recorder == NULL || stream == NULL ||
        stream->pStream == NULL || stream->mLen == 0U) {
        return -1;
    }

    pthread_mutex_lock(&recorder->send_lock);
    if (!recorder->accepting_frames) {
        pthread_mutex_unlock(&recorder->send_lock);
        return 0;
    }

    /* 在首个视频关键帧之前不写音频，避免MP4以无法解码的视频开头。 */
    if (recorder->waiting_for_key_frame) {
        ++recorder->skipped_audio_frames;
        pthread_mutex_unlock(&recorder->send_lock);
        return 0;
    }

    local_stream = *stream;
    ret = AW_MPI_MUX_SendAudioStreamSync(recorder->config.mux_channel,
                                         &local_stream,
                                         0);
    if (ret != SUCCESS) {
        aloge("[MP4] Send audio stream failed: id=%d, ret=%d",
              stream->mId,
              ret);
        result = -1;
    } else {
        ++recorder->audio_frames;
    }
    pthread_mutex_unlock(&recorder->send_lock);
    return result;
}

int mp4_recorder_stop(Mp4RecorderContext *recorder)
{
    ERRORTYPE ret;
    int result = 0;

    if (recorder == NULL) {
        return -1;
    }

    if (recorder->lock_initialized) {
        pthread_mutex_lock(&recorder->send_lock);
        recorder->accepting_frames = 0;
        pthread_mutex_unlock(&recorder->send_lock);
    }

    /* FALSE要求MUX正常收尾并写入MP4索引，不能在这里强制中断。 */
    if (recorder->mux_started) {
        ret = AW_MPI_MUX_StopChn(recorder->config.mux_channel, FALSE);
        if (ret != SUCCESS) {
            aloge("[MP4] Stop MUX channel failed: ret=%d", ret);
            result = -1;
        }
        recorder->mux_started = 0;
    }
    if (recorder->mux_created) {
        ret = AW_MPI_MUX_DestroyChn(recorder->config.mux_channel);
        if (ret != SUCCESS) {
            aloge("[MP4] Destroy MUX channel failed: ret=%d", ret);
            result = -1;
        }
        recorder->mux_created = 0;
    }
    if (recorder->output_fd >= 0) {
        /* StopChn已经要求MUX正常写完缓存和索引，随后关闭文件描述符。 */
        close(recorder->output_fd);
        recorder->output_fd = -1;
    }

    alogd("[MP4] Recorder stopped: video=%llu, audio=%llu, "
          "skipped_video=%llu, skipped_audio=%llu, file=%s",
          recorder->video_frames,
          recorder->audio_frames,
          recorder->skipped_video_frames,
          recorder->skipped_audio_frames,
          recorder->output_path);
    return result;
}

void mp4_recorder_destroy(Mp4RecorderContext *recorder)
{
    if (recorder == NULL) {
        return;
    }

    if (recorder->mux_started || recorder->mux_created ||
        recorder->output_fd >= 0) {
        mp4_recorder_stop(recorder);
    }
    if (recorder->lock_initialized) {
        pthread_mutex_destroy(&recorder->send_lock);
    }
    free(recorder->h264_header);
    free(recorder);
}
