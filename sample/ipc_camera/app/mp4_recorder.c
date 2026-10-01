#include "mp4_recorder.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <media/mpi_mux.h>
#include <utils/plat_log.h>

#define MP4_RECORDER_PATH_SIZE 256
#define MP4_RECORDER_PREFIX_SIZE 64
#define MP4_SIMPLE_CACHE_SIZE (64 * 1024)
#define MP4_MAX_PACKS_PER_FRAME 8
#define MP4_VIDEO_STREAM_ID 0
#define MP4_AUDIO_STREAM_ID 1
#define AAC_ADTS_HEADER_SIZE 7U
#define AAC_ADTS_CRC_HEADER_SIZE 9U
#define BYTES_PER_MIB (1024ULL * 1024ULL)

typedef struct RecordingFileInfo {
    char path[MP4_RECORDER_PATH_SIZE];
    time_t modified_time;
} RecordingFileInfo;

struct Mp4RecorderContext {
    Mp4RecorderConfig config;
    char output_path[MP4_RECORDER_PATH_SIZE];
    char pending_output_path[MP4_RECORDER_PATH_SIZE];
    char output_directory[MP4_RECORDER_PATH_SIZE];
    char file_prefix[MP4_RECORDER_PREFIX_SIZE];
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

    /*
     * MUX回调只投递“需要下一个文件”的事件，专用线程负责open和SwitchFd。
     * 这样不会在MPP内部回调线程中重入MUX接口。
     */
    pthread_mutex_t rotation_lock;
    pthread_cond_t rotation_cond;
    pthread_t rotation_thread;
    int rotation_lock_initialized;
    int rotation_cond_initialized;
    int rotation_thread_created;
    int rotation_stop_requested;
    int next_fd_requested;
    int cleanup_requested;
    int pending_segment_ready;
    int rotation_failed;
    int storage_failed;
    unsigned int file_sequence;
    unsigned long long completed_files;
    unsigned long long deleted_files;
    unsigned long long discarded_empty_files;

    unsigned long long video_frames;
    unsigned long long audio_frames;
    unsigned long long skipped_video_frames;
    unsigned long long skipped_audio_frames;
    unsigned long long stripped_adts_frames;
};

/* 根据本地系统时间生成不会互相覆盖的MP4文件名。 */
static int generate_segment_path(Mp4RecorderContext *recorder,
                                 char *path,
                                 size_t path_size)
{
    time_t now;
    struct tm local_time;
    char time_text[32];
    const char *separator;
    int length;

    now = time(NULL);
    if (localtime_r(&now, &local_time) == NULL ||
        strftime(time_text,
                 sizeof(time_text),
                 "%Y%m%d_%H%M%S",
                 &local_time) == 0U) {
        return -1;
    }

    separator = recorder->output_directory[
                    strlen(recorder->output_directory) - 1U] == '/'
                    ? ""
                    : "/";
    length = snprintf(path,
                      path_size,
                      "%s%s%s_%s_%04u.mp4",
                      recorder->output_directory,
                      separator,
                      recorder->file_prefix,
                      time_text,
                      recorder->file_sequence++);
    if (length < 0 || (size_t)length >= path_size) {
        return -1;
    }
    return 0;
}

/*
 * O_EXCL保证应用重启或系统时间回拨时不会覆盖已有录像。若同名则递增序号
 * 继续尝试，成功后返回新文件描述符。
 */
static int open_new_segment(Mp4RecorderContext *recorder,
                            char *path,
                            size_t path_size)
{
    unsigned int attempt;
    int fd;

    for (attempt = 0U; attempt < 10000U; ++attempt) {
        if (generate_segment_path(recorder, path, path_size) != 0) {
            return -1;
        }
        fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) {
            return fd;
        }
        if (errno != EEXIST) {
            aloge("[MP4] Open segment failed: file=%s, errno=%d",
                  path,
                  errno);
            return -1;
        }
    }

    aloge("[MP4] Cannot allocate a unique segment name");
    return -1;
}

static int is_decimal_text(const char *text, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (!isdigit((unsigned char)text[index])) {
            return 0;
        }
    }
    return 1;
}

/*
 * 只接受“前缀_YYYYMMDD_HHMMSS_NNNN.mp4”。严格匹配是循环删除的安全边界，
 * 类似record_backup.mp4、sample_demo.mp4以及其他用户文件都不会进入候选集。
 */
static int is_managed_recording_name(const Mp4RecorderContext *recorder,
                                     const char *name)
{
    size_t prefix_length = strlen(recorder->file_prefix);
    const char *suffix;

    if (strncmp(name, recorder->file_prefix, prefix_length) != 0) {
        return 0;
    }
    suffix = name + prefix_length;
    if (strlen(suffix) != 25U || suffix[0] != '_' || suffix[9] != '_' ||
        suffix[16] != '_' || strcmp(suffix + 21, ".mp4") != 0) {
        return 0;
    }
    return is_decimal_text(suffix + 1, 8U) &&
           is_decimal_text(suffix + 10, 6U) &&
           is_decimal_text(suffix + 17, 4U);
}

static int compare_recording_files(const void *left, const void *right)
{
    const RecordingFileInfo *left_file = left;
    const RecordingFileInfo *right_file = right;

    if (left_file->modified_time < right_file->modified_time) {
        return -1;
    }
    if (left_file->modified_time > right_file->modified_time) {
        return 1;
    }
    return strcmp(left_file->path, right_file->path);
}

/* 扫描录像目录，仅收集严格匹配命名规则的普通文件，不跟随符号链接。 */
static int scan_managed_recordings(Mp4RecorderContext *recorder,
                                   RecordingFileInfo **files_out,
                                   size_t *count_out)
{
    DIR *directory;
    struct dirent *entry;
    RecordingFileInfo *files = NULL;
    size_t count = 0U;
    size_t capacity = 0U;

    directory = opendir(recorder->output_directory);
    if (directory == NULL) {
        aloge("[MP4] Open recording directory failed: dir=%s, errno=%d",
              recorder->output_directory,
              errno);
        return -1;
    }

    while ((entry = readdir(directory)) != NULL) {
        RecordingFileInfo *expanded_files;
        struct stat file_status;
        int length;

        if (!is_managed_recording_name(recorder, entry->d_name)) {
            continue;
        }
        if (count == capacity) {
            size_t next_capacity = capacity == 0U ? 16U : capacity * 2U;

            expanded_files = realloc(files,
                                     next_capacity * sizeof(files[0]));
            if (expanded_files == NULL) {
                free(files);
                closedir(directory);
                return -1;
            }
            files = expanded_files;
            capacity = next_capacity;
        }

        length = snprintf(files[count].path,
                          sizeof(files[count].path),
                          "%s%s%s",
                          recorder->output_directory,
                          recorder->output_directory[
                              strlen(recorder->output_directory) - 1U] == '/'
                              ? ""
                              : "/",
                          entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(files[count].path)) {
            alogw("[MP4] Skip recording with overlong path: %s",
                  entry->d_name);
            continue;
        }
        if (lstat(files[count].path, &file_status) != 0 ||
            !S_ISREG(file_status.st_mode)) {
            alogw("[MP4] Skip non-regular recording entry: %s",
                  files[count].path);
            continue;
        }
        files[count].modified_time = file_status.st_mtime;
        ++count;
    }
    closedir(directory);

    if (count > 1U) {
        qsort(files, count, sizeof(files[0]), compare_recording_files);
    }
    *files_out = files;
    *count_out = count;
    return 0;
}

static int get_available_space(const Mp4RecorderContext *recorder,
                               unsigned long long *available_bytes)
{
    struct statvfs file_system;

    if (statvfs(recorder->output_directory, &file_system) != 0) {
        aloge("[MP4] Query free space failed: dir=%s, errno=%d",
              recorder->output_directory,
              errno);
        return -1;
    }
    *available_bytes =
        (unsigned long long)file_system.f_bavail * file_system.f_frsize;
    return 0;
}

/*
 * 删除最旧的受管录像，直到满足数量和剩余空间要求。protected_path是当前仍在
 * 写入的文件，任何情况下都不会删除。reserve_slots用于为即将创建的新段预留
 * 一个名额；如果只有受保护文件导致暂时超限，会在文件切换完成后再次清理。
 */
static int prune_old_recordings(Mp4RecorderContext *recorder,
                                const char *protected_path,
                                unsigned int reserve_slots)
{
    RecordingFileInfo *files = NULL;
    size_t count = 0U;
    unsigned long long available_bytes = 0ULL;
    unsigned long long minimum_bytes =
        (unsigned long long)recorder->config.min_free_space_mb * BYTES_PER_MIB;
    int result = 0;

    if (recorder->config.max_segment_files <= 0 && minimum_bytes == 0ULL) {
        return 0;
    }
    if (scan_managed_recordings(recorder, &files, &count) != 0) {
        return -1;
    }
    if (minimum_bytes > 0ULL &&
        get_available_space(recorder, &available_bytes) != 0) {
        free(files);
        return -1;
    }

    for (;;) {
        int too_many = recorder->config.max_segment_files > 0 &&
                       count + reserve_slots >
                           (size_t)recorder->config.max_segment_files;
        int too_little_space = minimum_bytes > 0ULL &&
                               available_bytes < minimum_bytes;
        size_t candidate = (size_t)-1;
        size_t index;

        if (!too_many && !too_little_space) {
            break;
        }
        for (index = 0U; index < count; ++index) {
            if (protected_path == NULL ||
                strcmp(files[index].path, protected_path) != 0) {
                candidate = index;
                break;
            }
        }
        if (candidate == (size_t)-1) {
            if (too_little_space) {
                aloge("[MP4] Free-space target cannot be reached: "
                      "available=%llu MiB, required=%llu MiB",
                      available_bytes / BYTES_PER_MIB,
                      minimum_bytes / BYTES_PER_MIB);
                result = -1;
            } else {
                alogw("[MP4] File-count limit temporarily exceeded because "
                      "the only candidate is active");
            }
            break;
        }

        if (unlink(files[candidate].path) != 0) {
            aloge("[MP4] Delete old recording failed: file=%s, errno=%d",
                  files[candidate].path,
                  errno);
            result = -1;
            break;
        }
        ++recorder->deleted_files;
        alogd("[MP4] Deleted old recording: %s", files[candidate].path);

        if (candidate + 1U < count) {
            memmove(&files[candidate],
                    &files[candidate + 1U],
                    (count - candidate - 1U) * sizeof(files[0]));
        }
        --count;
        if (minimum_bytes > 0ULL &&
            get_available_space(recorder, &available_bytes) != 0) {
            result = -1;
            break;
        }
    }

    alogd("[MP4] Storage check: managed=%u, available=%llu MiB, "
          "limit=%d, reserve=%u",
          (unsigned int)count,
          minimum_bytes > 0ULL ? available_bytes / BYTES_PER_MIB : 0ULL,
          recorder->config.max_segment_files,
          reserve_slots);
    free(files);
    return result;
}

/* 打开下一个文件并交给MUX；MPP内部会持有自己的文件描述符引用。 */
static int switch_to_next_segment(Mp4RecorderContext *recorder)
{
    char next_path[MP4_RECORDER_PATH_SIZE];
    int next_fd;
    ERRORTYPE ret;

    /*
     * MUX会提前请求下一个文件。如果上一个备用文件还没有真正切换为当前文件，
     * 再提交一个fd会使文件状态无法对应，因此把它视为异常。
     */
    pthread_mutex_lock(&recorder->rotation_lock);
    if (recorder->pending_segment_ready) {
        pthread_mutex_unlock(&recorder->rotation_lock);
        aloge("[MP4] Previous pending segment has not become active: %s",
              recorder->pending_output_path);
        return -1;
    }
    pthread_mutex_unlock(&recorder->rotation_lock);

    /* 先清理再创建，避免磁盘已接近阈值时继续无条件增加文件。 */
    if (prune_old_recordings(recorder, recorder->output_path, 1U) != 0) {
        return -1;
    }
    next_fd = open_new_segment(recorder, next_path, sizeof(next_path));
    if (next_fd < 0) {
        return -1;
    }

    /* 与音视频Send*StreamSync串行，防止切换文件时同时向MUX提交码流。 */
    pthread_mutex_lock(&recorder->send_lock);
    if (!recorder->accepting_frames || !recorder->mux_started) {
        pthread_mutex_unlock(&recorder->send_lock);
        close(next_fd);
        unlink(next_path);
        return 0;
    }
    /*
     * 先登记备用路径再调用SDK：即使某个SDK版本在SwitchFd内部同步触发
     * RECORD_DONE，回调也能找到并正确晋升这个文件。
     */
    pthread_mutex_lock(&recorder->rotation_lock);
    memcpy(recorder->pending_output_path,
           next_path,
           strlen(next_path) + 1U);
    recorder->pending_segment_ready = 1;
    pthread_mutex_unlock(&recorder->rotation_lock);

    ret = AW_MPI_MUX_SwitchFd(recorder->config.mux_channel, next_fd, 0);
    pthread_mutex_unlock(&recorder->send_lock);

    /* 参考原项目：SwitchFd成功返回后，MUX已经保存了自己的fd引用。 */
    close(next_fd);
    if (ret != SUCCESS) {
        pthread_mutex_lock(&recorder->rotation_lock);
        if (recorder->pending_segment_ready &&
            strcmp(recorder->pending_output_path, next_path) == 0) {
            recorder->pending_output_path[0] = '\0';
            recorder->pending_segment_ready = 0;
        }
        pthread_mutex_unlock(&recorder->rotation_lock);
        /* SwitchFd失败时MUX没有使用该文件，删除刚创建的空占位文件。 */
        unlink(next_path);
        aloge("[MP4] Switch to next segment failed: file=%s, ret=%d",
              next_path,
              ret);
        return -1;
    }

    alogd("[MP4] Submitted pending segment: %s", next_path);
    return 0;
}

/*
 * 停止MUX后处理“已提交但尚未使用”的备用文件。
 * 空文件表示切片边界尚未到达，安全删除；非空文件表示MUX已开始使用，只是停止
 * 阶段没有再晋升状态，因此必须保留并把它记为最后的有效录像文件。
 */
static int resolve_pending_segment_on_stop(Mp4RecorderContext *recorder)
{
    struct stat file_stat;
    char pending_path[MP4_RECORDER_PATH_SIZE];

    pthread_mutex_lock(&recorder->rotation_lock);
    if (!recorder->pending_segment_ready) {
        pthread_mutex_unlock(&recorder->rotation_lock);
        return 0;
    }
    memcpy(pending_path,
           recorder->pending_output_path,
           strlen(recorder->pending_output_path) + 1U);
    pthread_mutex_unlock(&recorder->rotation_lock);

    if (lstat(pending_path, &file_stat) != 0) {
        if (errno != ENOENT) {
            aloge("[MP4] Inspect pending segment failed: file=%s, errno=%d",
                  pending_path,
                  errno);
            return -1;
        }
    } else if (!S_ISREG(file_stat.st_mode)) {
        aloge("[MP4] Pending segment is not a regular file: %s", pending_path);
        return -1;
    } else if (file_stat.st_size == 0) {
        if (unlink(pending_path) != 0) {
            aloge("[MP4] Remove unused pending segment failed: file=%s, errno=%d",
                  pending_path,
                  errno);
            return -1;
        }
        ++recorder->discarded_empty_files;
        alogd("[MP4] Removed unused empty pending segment: %s", pending_path);
    } else {
        memcpy(recorder->output_path,
               pending_path,
               strlen(pending_path) + 1U);
        alogd("[MP4] Kept active pending segment: file=%s, size=%lld bytes",
              pending_path,
              (long long)file_stat.st_size);
    }

    pthread_mutex_lock(&recorder->rotation_lock);
    recorder->pending_output_path[0] = '\0';
    recorder->pending_segment_ready = 0;
    pthread_mutex_unlock(&recorder->rotation_lock);
    return 0;
}

/* 处理MUX回调投递的文件切换请求。 */
static void *segment_rotation_thread(void *argument)
{
    Mp4RecorderContext *recorder = argument;

    alogd("[MP4] Segment rotation thread started");
    for (;;) {
        int need_next_file;
        int need_cleanup;

        pthread_mutex_lock(&recorder->rotation_lock);
        while (!recorder->rotation_stop_requested &&
               !recorder->next_fd_requested &&
               !recorder->cleanup_requested) {
            pthread_cond_wait(&recorder->rotation_cond,
                              &recorder->rotation_lock);
        }
        if (recorder->rotation_stop_requested) {
            pthread_mutex_unlock(&recorder->rotation_lock);
            break;
        }
        need_next_file = recorder->next_fd_requested;
        need_cleanup = recorder->cleanup_requested;
        recorder->next_fd_requested = 0;
        recorder->cleanup_requested = 0;
        pthread_mutex_unlock(&recorder->rotation_lock);

        if (need_next_file && switch_to_next_segment(recorder) != 0) {
            pthread_mutex_lock(&recorder->rotation_lock);
            recorder->rotation_failed = 1;
            pthread_mutex_unlock(&recorder->rotation_lock);
        }
        if (need_cleanup &&
            prune_old_recordings(recorder, recorder->output_path, 0U) != 0) {
            pthread_mutex_lock(&recorder->rotation_lock);
            recorder->storage_failed = 1;
            pthread_mutex_unlock(&recorder->rotation_lock);
        }
    }

    alogd("[MP4] Segment rotation thread stopped");
    return NULL;
}

/*
 * AENC在attachAACHeader=1时输出ADTS格式，便于直接保存成独立的.aac文件。
 * 但MP4的audio sample只能保存AAC原始访问单元，不能包含每帧的ADTS头。
 *
 * 本函数只修改AUDIO_STREAM_S描述符的副本：把数据指针越过7/9字节头部并
 * 缩短长度，不复制也不改写AENC原始缓冲。因此独立AAC文件和RTSP仍然收到
 * 完整ADTS帧，只有送入MP4 MUX的数据会去掉ADTS头。
 *
 * 返回值：1表示剥离了ADTS头，0表示输入已经是裸AAC，-1表示ADTS帧非法。
 */
static int prepare_aac_sample_for_mp4(const AUDIO_STREAM_S *source,
                                      AUDIO_STREAM_S *destination)
{
    unsigned int header_size;
    unsigned int frame_size;

    *destination = *source;

    /* ADTS同步字固定为12个1，即首字节0xff、次字节高4位0xf。 */
    if (source->mLen < 2U || source->pStream[0] != 0xffU ||
        (source->pStream[1] & 0xf0U) != 0xf0U) {
        return 0;
    }

    /* protection_absent为0时，ADTS头末尾还包含2字节CRC。 */
    header_size = (source->pStream[1] & 0x01U) != 0U
                      ? AAC_ADTS_HEADER_SIZE
                      : AAC_ADTS_CRC_HEADER_SIZE;
    if (source->mLen < header_size) {
        return -1;
    }

    /* frame_length是13位字段，长度包含ADTS头和AAC有效载荷。 */
    frame_size = ((unsigned int)(source->pStream[3] & 0x03U) << 11) |
                 ((unsigned int)source->pStream[4] << 3) |
                 ((unsigned int)(source->pStream[5] & 0xe0U) >> 5);
    if (frame_size != source->mLen || frame_size <= header_size) {
        return -1;
    }

    destination->pStream += header_size;
    destination->mLen -= header_size;
    return 1;
}

/*
 * 回调运行在MPP内部线程，只更新轻量状态并唤醒切片线程，不在这里打开文件或
 * 调用SwitchFd，避免阻塞MPP和发生回调重入。
 */
static ERRORTYPE mp4_mux_callback(void *cookie,
                                  MPP_CHN_S *channel,
                                  MPP_EVENT_TYPE event,
                                  void *event_data)
{
    Mp4RecorderContext *recorder = cookie;

    (void)channel;
    if (recorder == NULL) {
        return FAILURE;
    }

    if (event == MPP_EVENT_RECORD_DONE) {
        int muxer_id = event_data != NULL ? *(int *)event_data : -1;
        int segment_promoted = 0;
        char active_path[MP4_RECORDER_PATH_SIZE];

        active_path[0] = '\0';
        pthread_mutex_lock(&recorder->rotation_lock);
        ++recorder->completed_files;
        if (recorder->config.segment_duration_seconds > 0 &&
            !recorder->rotation_stop_requested) {
            if (recorder->pending_segment_ready) {
                memcpy(recorder->output_path,
                       recorder->pending_output_path,
                       strlen(recorder->pending_output_path) + 1U);
                memcpy(active_path,
                       recorder->output_path,
                       strlen(recorder->output_path) + 1U);
                recorder->pending_output_path[0] = '\0';
                recorder->pending_segment_ready = 0;
                segment_promoted = 1;
            }
            recorder->cleanup_requested = 1;
            pthread_cond_signal(&recorder->rotation_cond);
        }
        pthread_mutex_unlock(&recorder->rotation_lock);
        alogd("[MP4] MUX reported record done: muxer_id=%d", muxer_id);
        if (segment_promoted) {
            alogd("[MP4] Pending segment is now active: %s", active_path);
        }
    } else if (event == MPP_EVENT_NEED_NEXT_FD) {
        int muxer_id = event_data != NULL ? *(int *)event_data : -1;

        if (recorder->config.segment_duration_seconds <= 0) {
            alogw("[MP4] MUX requested next file in fixed-file mode");
            return SUCCESS;
        }
        if (muxer_id != 0) {
            aloge("[MP4] Unexpected muxer id in next-file event: %d",
                  muxer_id);
            return FAILURE;
        }

        pthread_mutex_lock(&recorder->rotation_lock);
        if (!recorder->rotation_stop_requested) {
            recorder->next_fd_requested = 1;
            pthread_cond_signal(&recorder->rotation_cond);
        }
        pthread_mutex_unlock(&recorder->rotation_lock);
        alogd("[MP4] MUX requested the next segment");
    }
    return SUCCESS;
}

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
    attributes->mMaxFileDuration =
        (int64_t)recorder->config.segment_duration_seconds * 1000;
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
    size_t directory_length;
    size_t prefix_length;

    if (config == NULL ||
        config->h264_header == NULL || config->h264_header_size == 0U ||
        config->width <= 0 || config->height <= 0 ||
        config->frame_rate <= 0 || config->sample_rate <= 0 ||
        config->audio_channels <= 0 || config->samples_per_frame <= 0 ||
        config->segment_duration_seconds < 0 ||
        config->max_segment_files < 0) {
        return NULL;
    }

    if (config->segment_duration_seconds > 0) {
        if (config->output_directory == NULL || config->file_prefix == NULL) {
            return NULL;
        }
        directory_length = strlen(config->output_directory);
        prefix_length = strlen(config->file_prefix);
        if (directory_length == 0U ||
            directory_length >= MP4_RECORDER_PATH_SIZE ||
            config->output_directory[0] != '/' ||
            strcmp(config->output_directory, "/") == 0 ||
            strstr(config->output_directory, "/../") != NULL ||
            prefix_length == 0U ||
            prefix_length >= MP4_RECORDER_PREFIX_SIZE) {
            return NULL;
        }
        {
            size_t index;

            for (index = 0U; index < prefix_length; ++index) {
                unsigned char character =
                    (unsigned char)config->file_prefix[index];

                if (!isalnum(character) && character != '-') {
                    return NULL;
                }
            }
        }
        path_length = 0U;
    } else {
        if (config->output_path == NULL) {
            return NULL;
        }
        path_length = strlen(config->output_path);
        if (path_length == 0U || path_length >= MP4_RECORDER_PATH_SIZE) {
            return NULL;
        }
        directory_length = 0U;
        prefix_length = 0U;
    }

    recorder = calloc(1, sizeof(*recorder));
    if (recorder == NULL) {
        return NULL;
    }

    recorder->config = *config;
    if (config->segment_duration_seconds > 0) {
        memcpy(recorder->output_directory,
               config->output_directory,
               directory_length + 1U);
        memcpy(recorder->file_prefix,
               config->file_prefix,
               prefix_length + 1U);
        recorder->config.output_directory = recorder->output_directory;
        recorder->config.file_prefix = recorder->file_prefix;
        recorder->config.output_path = NULL;
    } else {
        memcpy(recorder->output_path, config->output_path, path_length + 1U);
        recorder->config.output_path = recorder->output_path;
    }

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

    if (pthread_mutex_init(&recorder->rotation_lock, NULL) != 0) {
        pthread_mutex_destroy(&recorder->send_lock);
        free(recorder->h264_header);
        free(recorder);
        return NULL;
    }
    recorder->rotation_lock_initialized = 1;
    if (pthread_cond_init(&recorder->rotation_cond, NULL) != 0) {
        pthread_mutex_destroy(&recorder->rotation_lock);
        pthread_mutex_destroy(&recorder->send_lock);
        free(recorder->h264_header);
        free(recorder);
        return NULL;
    }
    recorder->rotation_cond_initialized = 1;
    return recorder;
}

int mp4_recorder_start(Mp4RecorderContext *recorder)
{
    MUX_CHN_ATTR_S attributes;
    VencHeaderData header;
    MPPCallbackInfo callback_info;
    struct stat directory_status;
    ERRORTYPE ret;

    if (recorder == NULL || recorder->mux_started) {
        return -1;
    }

    if (recorder->config.segment_duration_seconds > 0) {
        if (lstat(recorder->output_directory, &directory_status) != 0 ||
            !S_ISDIR(directory_status.st_mode)) {
            aloge("[MP4] Recording directory is unavailable or unsafe: %s",
                  recorder->output_directory);
            return -1;
        }
        /* 为即将创建的首段预留一个文件名额，并检查最低剩余空间。 */
        if (prune_old_recordings(recorder, NULL, 1U) != 0) {
            aloge("[MP4] Initial storage cleanup failed");
            return -1;
        }
        recorder->output_fd = open_new_segment(recorder,
                                               recorder->output_path,
                                               sizeof(recorder->output_path));
    } else {
        /* 固定文件模式保持阶段8.1行为，每次启动生成全新的测试文件。 */
        recorder->output_fd = open(recorder->output_path,
                                   O_RDWR | O_CREAT | O_TRUNC,
                                   0666);
    }
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

    if (recorder->config.segment_duration_seconds > 0) {
        /*
         * 最小时长策略会等到目标时长后的首个IDR再切换，保证每个新文件都
         * 从关键帧开始，代价是实际片长可能比配置值多一个GOP以内。
         */
        ret = AW_MPI_MUX_SetSwitchFileDurationPolicy(
            recorder->config.mux_channel,
            RecordFileDurationPolicy_MinDuration);
        if (ret != SUCCESS) {
            aloge("[MP4] Set segment duration policy failed: ret=%d", ret);
            goto error;
        }
    }

    memset(&callback_info, 0, sizeof(callback_info));
    callback_info.cookie = recorder;
    callback_info.callback = mp4_mux_callback;
    ret = AW_MPI_MUX_RegisterCallback(recorder->config.mux_channel,
                                      &callback_info);
    if (ret != SUCCESS) {
        aloge("[MP4] Register MUX callback failed: ret=%d", ret);
        goto error;
    }

    /*
     * 隧道Bind模式会由MPP自动建立“VENC通道 -> MUX视频流”映射；阶段8.1
     * 使用非隧道SendVideoStreamSync，因此必须显式把VENC 0映射到stream 0。
     * 如果缺少这一步，SetH264SpsPpsInfo找不到对应视频轨，部分SDK版本会在
     * 第一个关键帧初始化MP4时访问无效的SPS/PPS节点并崩溃。
     */
    ret = AW_MPI_MUX_SetVeChnBindStreamId(recorder->config.mux_channel,
                                           recorder->config.venc_channel,
                                           MP4_VIDEO_STREAM_ID);
    if (ret != SUCCESS) {
        aloge("[MP4] Bind VENC channel to MUX stream failed: "
              "venc=%d, stream=%d, ret=%d",
              recorder->config.venc_channel,
              MP4_VIDEO_STREAM_ID,
              ret);
        goto error;
    }
    alogd("[MP4] VENC-to-MUX mapping ready: venc=%d -> stream=%d",
          recorder->config.venc_channel,
          MP4_VIDEO_STREAM_ID);

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

    if (recorder->config.segment_duration_seconds > 0) {
        recorder->rotation_stop_requested = 0;
        recorder->next_fd_requested = 0;
        recorder->cleanup_requested = 0;
        recorder->rotation_failed = 0;
        recorder->storage_failed = 0;
        if (pthread_create(&recorder->rotation_thread,
                           NULL,
                           segment_rotation_thread,
                           recorder) != 0) {
            aloge("[MP4] Create segment rotation thread failed");
            goto error;
        }
        recorder->rotation_thread_created = 1;
    }

    ret = AW_MPI_MUX_StartChn(recorder->config.mux_channel);
    if (ret != SUCCESS) {
        aloge("[MP4] Start MUX channel failed: ret=%d", ret);
        goto error;
    }

    pthread_mutex_lock(&recorder->send_lock);
    recorder->mux_started = 1;
    recorder->accepting_frames = 1;
    recorder->waiting_for_key_frame = 1;
    pthread_mutex_unlock(&recorder->send_lock);

    alogd("[MP4] Recorder started: mux=%d, video=%dx%d@%dfps, "
          "audio=%dHz/%dch, segment=%ds, file=%s",
          recorder->config.mux_channel,
          recorder->config.width,
          recorder->config.height,
          recorder->config.frame_rate,
          recorder->config.sample_rate,
          recorder->config.audio_channels,
          recorder->config.segment_duration_seconds,
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
                                         MP4_VIDEO_STREAM_ID);
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
    int adts_result;
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

    adts_result = prepare_aac_sample_for_mp4(stream, &local_stream);
    if (adts_result < 0) {
        ++recorder->skipped_audio_frames;
        aloge("[MP4] Invalid ADTS frame: id=%d, bytes=%u",
              stream->mId,
              stream->mLen);
        pthread_mutex_unlock(&recorder->send_lock);
        return -1;
    }
    if (adts_result > 0) {
        ++recorder->stripped_adts_frames;
        if (recorder->stripped_adts_frames == 1ULL) {
            alogd("[MP4] ADTS header stripped before MUX: "
                  "id=%d, input=%u, sample=%u",
                  stream->mId,
                  stream->mLen,
                  local_stream.mLen);
        }
    }

    ret = AW_MPI_MUX_SendAudioStreamSync(recorder->config.mux_channel,
                                         &local_stream,
                                         MP4_AUDIO_STREAM_ID);
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

    /* 先停止切片线程，确保它不会与MUX停止/销毁过程并发调用SwitchFd。 */
    if (recorder->rotation_thread_created) {
        pthread_mutex_lock(&recorder->rotation_lock);
        recorder->rotation_stop_requested = 1;
        pthread_cond_signal(&recorder->rotation_cond);
        pthread_mutex_unlock(&recorder->rotation_lock);
        pthread_join(recorder->rotation_thread, NULL);
        recorder->rotation_thread_created = 0;
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

    /*
     * Ctrl+C可能发生在MUX预取下一个fd之后、实际分段之前。此时会残留一个
     * 0字节备用文件；必须等StopChn结束、MUX不再写文件后才能安全判定和删除。
     */
    if (recorder->config.segment_duration_seconds > 0 &&
        resolve_pending_segment_on_stop(recorder) != 0) {
        recorder->storage_failed = 1;
    }

    /* 最后一段已经封口，再同步执行一次清理；保留本次最后生成的文件。 */
    if (recorder->config.segment_duration_seconds > 0 &&
        prune_old_recordings(recorder, recorder->output_path, 0U) != 0) {
        recorder->storage_failed = 1;
    }

    if (recorder->rotation_failed || recorder->storage_failed) {
        result = -1;
    }

    alogd("[MP4] Recorder stopped: video=%llu, audio=%llu, "
          "skipped_video=%llu, skipped_audio=%llu, "
          "adts_stripped=%llu, completed_files=%llu, deleted_files=%llu, "
          "discarded_empty_files=%llu, "
          "file=%s",
          recorder->video_frames,
          recorder->audio_frames,
          recorder->skipped_video_frames,
          recorder->skipped_audio_frames,
          recorder->stripped_adts_frames,
          recorder->completed_files,
          recorder->deleted_files,
          recorder->discarded_empty_files,
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
    if (recorder->rotation_cond_initialized) {
        pthread_cond_destroy(&recorder->rotation_cond);
    }
    if (recorder->rotation_lock_initialized) {
        pthread_mutex_destroy(&recorder->rotation_lock);
    }
    free(recorder->h264_header);
    free(recorder);
}
