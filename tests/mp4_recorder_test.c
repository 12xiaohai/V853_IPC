#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
/* 仅适配主机文件检查；生产代码继续使用lstat拒绝符号链接。 */
#define lstat stat
#endif
#include "../sample/ipc_camera/app/mp4_recorder.c"

/* 用真实SDK类型、替身函数测试，不加载ARM厂商库。 */
typedef struct FakePacket {
    int used, video;
    VENC_STREAM_S venc;
    VENC_PACK_S pack;
    AUDIO_STREAM_S aenc;
} FakePacket;
static MPPCallbackInfo fake_callback;
static FakePacket fake_packets[MP4_VIDEO_SLOTS + MP4_AUDIO_SLOTS];
static int fake_early, fake_pair, fake_send_error, fake_stop_error;
static int fake_destroy_error, fake_keep_on_stop;
static int fake_slow;
static unsigned int fake_sends, test_sequence;
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;

/* 回调重建结构体，模拟mpi_mux，而不是返回原来的结构体指针。 */
static void fake_release(FakePacket *packet)
{
    if (!packet->used) { return; }
    packet->used = 0;
    if (packet->video) {
        MUX_VENC_STREAM_S released;
        memset(&released, 0, sizeof(released));
        released.mVencStream = packet->venc;
        released.mVencStream.mpPack = &packet->pack;
        released.mStreamId = MP4_VIDEO_STREAM_ID;
        assert(fake_callback.callback(fake_callback.cookie, NULL,
               MPP_EVENT_RELEASE_VENC_STREAM, &released) == SUCCESS);
    } else {
        MUX_AENC_STREAM_S released;
        memset(&released, 0, sizeof(released));
        released.mAencStream = packet->aenc;
        released.mStreamId = MP4_AUDIO_STREAM_ID;
        assert(fake_callback.callback(fake_callback.cookie, NULL,
               MPP_EVENT_RELEASE_AENC_STREAM, &released) == SUCCESS);
    }
}

static void fake_release_all(void)
{
    unsigned int index;
    for (index = 0; index < sizeof(fake_packets) / sizeof(fake_packets[0]); ++index) {
        fake_release(&fake_packets[index]);
    }
}

static void fake_accept(const VENC_STREAM_S *video, const AUDIO_STREAM_S *audio)
{
    unsigned int index;
    int have_video = 0, have_audio = 0;
    pthread_mutex_lock(&fake_lock);
    for (index = 0; index < sizeof(fake_packets) / sizeof(fake_packets[0]); ++index) {
        FakePacket *packet = &fake_packets[index];
        if (!packet->used) {
            packet->used = 1;
            packet->video = video != NULL;
            if (video != NULL) {
                packet->venc = *video;
                packet->pack = video->mpPack[0];
                packet->venc.mpPack = &packet->pack;
            } else {
                packet->aenc = *audio;
            }
            ++fake_sends;
            if (fake_early) { fake_release(packet); }
            break;
        }
    }
    assert(index < sizeof(fake_packets) / sizeof(fake_packets[0]));
    if (fake_slow) {
        struct timespec pause = {0, 1000000L};
        nanosleep(&pause, NULL);
    }
    if (fake_pair) {
        /* 分段场景：先到的一轨必须等另一轨到达，但Send本身不等待。 */
        for (index = 0; index < sizeof(fake_packets) / sizeof(fake_packets[0]); ++index) {
            if (fake_packets[index].used) {
                have_video |= fake_packets[index].video;
                have_audio |= !fake_packets[index].video;
            }
        }
        if (have_video && have_audio) { fake_release_all(); }
    }
    pthread_mutex_unlock(&fake_lock);
}

ERRORTYPE AW_MPI_MUX_CreateChn(MUX_CHN ch, MUX_CHN_ATTR_S *attr, int fd, int length)
{ (void)ch; (void)attr; (void)fd; (void)length; return SUCCESS; }
ERRORTYPE AW_MPI_MUX_RegisterCallback(MUX_CHN ch, MPPCallbackInfo *callback)
{ (void)ch; fake_callback = *callback; return SUCCESS; }
ERRORTYPE AW_MPI_MUX_SetSwitchFileDurationPolicy(MUX_CHN ch, RecordFileDurationPolicy policy)
{ (void)ch; (void)policy; return SUCCESS; }
ERRORTYPE AW_MPI_MUX_SetVeChnBindStreamId(MUX_CHN ch, VENC_CHN venc, int id)
{ (void)ch; (void)venc; assert(id == 0); return SUCCESS; }
ERRORTYPE AW_MPI_MUX_SetH264SpsPpsInfo(MUX_CHN ch, VENC_CHN venc, VencHeaderData *header)
{ (void)ch; (void)venc; assert(header->nLength > 0); return SUCCESS; }
ERRORTYPE AW_MPI_MUX_StartChn(MUX_CHN ch)
{ (void)ch; return SUCCESS; }
ERRORTYPE AW_MPI_MUX_SendVideoStream(MUX_CHN ch, VENC_STREAM_S *stream, int id)
{ (void)ch; assert(id == 0); if (fake_send_error) { return FAILURE; }
  fake_accept(stream, NULL); return SUCCESS; }
ERRORTYPE AW_MPI_MUX_SendAudioStream(MUX_CHN ch, AUDIO_STREAM_S *stream, int id)
{ (void)ch; assert(id == 1); if (fake_send_error) { return FAILURE; }
  fake_accept(NULL, stream); return SUCCESS; }
/* 若生产实现意外又使用Sync，链接后运行测试立刻失败。 */
ERRORTYPE AW_MPI_MUX_SendVideoStreamSync(MUX_CHN ch, VENC_STREAM_S *stream, int id)
{ (void)ch; (void)stream; (void)id; assert(!"Sync video must not be called"); return FAILURE; }
ERRORTYPE AW_MPI_MUX_SendAudioStreamSync(MUX_CHN ch, AUDIO_STREAM_S *stream, int id)
{ (void)ch; (void)stream; (void)id; assert(!"Sync audio must not be called"); return FAILURE; }
ERRORTYPE AW_MPI_MUX_SwitchFd(MUX_CHN ch, int fd, int length)
{ (void)ch; (void)length; assert(fd >= 0); return SUCCESS; }
ERRORTYPE AW_MPI_MUX_StopChn(MUX_CHN ch, BOOL now)
{ (void)ch; assert(!now); if (fake_stop_error) { return FAILURE; }
  if (!fake_keep_on_stop) { fake_release_all(); } return SUCCESS; }
ERRORTYPE AW_MPI_MUX_DestroyChn(MUX_CHN ch)
{ (void)ch; if (fake_destroy_error) { return FAILURE; }
  /* Destroy成功意味着SDK不再访问缓冲，无回调时应用可安全收回。 */
  memset(fake_packets, 0, sizeof(fake_packets)); return SUCCESS; }

static Mp4RecorderContext *new_recorder(void)
{
    static const unsigned char header[] = {0, 0, 0, 1, 0x67};
    Mp4RecorderConfig config;
    Mp4RecorderContext *recorder;
    char path[256];
    memset(&config, 0, sizeof(config));
    snprintf(path, sizeof(path), "output/mp4test-%lu-%u.mp4",
             (unsigned long)getpid(), test_sequence++);
    config.output_path = path;
    config.width = 1920; config.height = 1080; config.frame_rate = 20;
    config.gop_size = 20; config.sample_rate = 16000; config.audio_channels = 1;
    config.audio_bit_width = 16; config.samples_per_frame = 1024;
    config.h264_header = header; config.h264_header_size = sizeof(header);
    memset(fake_packets, 0, sizeof(fake_packets));
    fake_early = fake_pair = fake_send_error = fake_stop_error = 0;
    fake_destroy_error = fake_keep_on_stop = 0; fake_sends = 0;
    fake_slow = 0;
    recorder = mp4_recorder_create(&config);
    assert(recorder != NULL && mp4_recorder_start(recorder) == 0);
    return recorder;
}

/* 新建且由测试拥有的文件，退出后清理，不匹配任何用户录像。 */
static void finish_recorder(Mp4RecorderContext *recorder, int expected)
{
    char path[256];
    strcpy(path, recorder->config.output_path);
    assert(mp4_recorder_stop(recorder) == expected);
    assert(recorder->pending_count == 0 && recorder->pending_bytes == 0);
    mp4_recorder_destroy(recorder);
    assert(unlink(path) == 0);
}

static int push_video(Mp4RecorderContext *recorder, int key)
{
    unsigned char part0[] = {0, 0, 0, 1, 0x65};
    unsigned char part1[] = {0x11, 0x22}, part2[] = {0x33};
    VENC_PACK_S pack;
    VENC_STREAM_S stream;
    memset(&pack, 0, sizeof(pack)); memset(&stream, 0, sizeof(stream));
    pack.mpAddr0 = part0; pack.mLen0 = sizeof(part0);
    pack.mpAddr1 = part1; pack.mLen1 = sizeof(part1);
    pack.mpAddr2 = part2; pack.mLen2 = sizeof(part2); pack.mPTS = 123456;
    stream.mpPack = &pack; stream.mPackCount = 1; stream.mSeq = 1;
    return mp4_recorder_push_video(recorder, &stream, key);
}

static int push_audio(Mp4RecorderContext *recorder)
{
    /* ADTS frame_length=11：7字节头+4字节AAC。原数据返回后立即失效。 */
    unsigned char adts[] = {0xff,0xf1,0x60,0x40,0x01,0x7f,0xfc,0xde,0xad,0xbe,0xef};
    unsigned char extra[] = {0x12,0x34};
    AUDIO_STREAM_S stream;
    int result;
    memset(&stream, 0, sizeof(stream));
    stream.pStream = adts; stream.mLen = sizeof(adts); stream.mId = 1;
    stream.pStreamExtra = extra; stream.mExtraLen = sizeof(extra);
    stream.mTimeStamp = 123456;
    result = mp4_recorder_push_audio(recorder, &stream);
    assert(adts[0] == 0xff && stream.mLen == sizeof(adts));
    memset(adts, 0, sizeof(adts)); memset(extra, 0, sizeof(extra));
    return result;
}

static void test_delayed_release_and_switch(void)
{
    Mp4RecorderContext *recorder = new_recorder();
    FakePacket *video, *audio;
    unsigned int before;
    assert(push_audio(recorder) == 0 && fake_sends == 0);
    assert(push_video(recorder, 0) == 0 && fake_sends == 0);
    assert(push_video(recorder, 1) == 0 && push_audio(recorder) == 0);
    assert(recorder->pending_count == 2);
    video = &fake_packets[0]; audio = &fake_packets[1];
    assert(video->pack.mpAddr0[4] == 0x65 && video->pack.mpAddr1[1] == 0x22);
    assert(video->pack.mpAddr2[0] == 0x33 && video->pack.mPTS == 123456);
    assert(audio->aenc.mLen == 4 && audio->aenc.pStream[0] == 0xde);
    assert(audio->aenc.pStreamExtra[0] == 0x12 && audio->aenc.mTimeStamp == 123456);
    /* 两轨未释放时仍能提交备用fd，退出时清理测试拥有的空备用文件。 */
    recorder->config.segment_duration_seconds = 60;
    strcpy(recorder->output_directory, "output");
    snprintf(recorder->file_prefix, sizeof(recorder->file_prefix), "mp4test%lu",
             (unsigned long)getpid());
    assert(switch_to_next_segment(recorder) == 0);
    assert(recorder->pending_segment_ready && recorder->pending_count == 2);
    mp4_recorder_close_input(recorder);
    before = fake_sends;
    assert(push_video(recorder, 1) == 0 && push_audio(recorder) == 0);
    assert(fake_sends == before);
    finish_recorder(recorder, 0);
}

static void *concurrent_producer(void *argument)
{
    Mp4RecorderContext *recorder = argument;
    unsigned int index;
    for (index = 0; index < 1000; ++index) {
        assert(push_video(recorder, 1) == 0);
        assert(push_audio(recorder) == 0);
    }
    return NULL;
}

static void test_pair_and_early_release(void)
{
    Mp4RecorderContext *recorder = new_recorder();
    unsigned int index;
    fake_pair = 1;
    for (index = 0; index < 500; ++index) {
        assert(push_video(recorder, 1) == 0 && recorder->pending_count == 1);
        assert(push_audio(recorder) == 0 && recorder->pending_count == 0);
    }
    finish_recorder(recorder, 0);
    recorder = new_recorder();
    fake_early = 1;
    {
        pthread_t first, second;
        assert(pthread_create(&first, NULL, concurrent_producer, recorder) == 0);
        assert(pthread_create(&second, NULL, concurrent_producer, recorder) == 0);
        pthread_join(first, NULL); pthread_join(second, NULL);
    }
    assert(recorder->pending_count == 0 && fake_sends == 4000);
    finish_recorder(recorder, 0);
}

static void test_bounded_failure_and_submit_failure(void)
{
    Mp4RecorderContext *recorder = new_recorder();
    unsigned int index, before;
    assert(push_video(recorder, 1) == 0);
    for (index = 0; index < MP4_AUDIO_SLOTS; ++index) {
        assert(push_audio(recorder) == 0);
    }
    assert(push_audio(recorder) == -1 && !recorder->accepting_frames);
    assert(recorder->pending_count == MP4_AUDIO_SLOTS + 1U);
    before = fake_sends;
    assert(push_audio(recorder) == 0 && fake_sends == before);
    finish_recorder(recorder, -1);
    recorder = new_recorder();
    for (index = 0; index < MP4_VIDEO_SLOTS; ++index) {
        assert(push_video(recorder, 1) == 0);
    }
    assert(push_video(recorder, 1) == -1 && !recorder->accepting_frames);
    finish_recorder(recorder, -1);
    recorder = new_recorder(); fake_send_error = 1;
    assert(push_video(recorder, 1) == -1 && recorder->pending_count == 0);
    finish_recorder(recorder, -1);
    recorder = new_recorder(); assert(push_video(recorder, 1) == 0);
    fake_send_error = 1;
    assert(push_audio(recorder) == -1 && recorder->pending_count == 1);
    finish_recorder(recorder, -1);
}

static void test_shutdown_failure_retains_ownership(void)
{
    Mp4RecorderContext *recorder = new_recorder();
    char path[256];
    strcpy(path, recorder->output_path);
    assert(push_video(recorder, 1) == 0);
    fake_stop_error = 1;
    assert(mp4_recorder_stop(recorder) == -1);
    mp4_recorder_destroy(recorder);
    assert(recorder->mux_started && recorder->pending_count == 1);
    assert(fake_packets[0].pack.mpAddr0[4] == 0x65);
    fake_stop_error = 0; fake_keep_on_stop = 1; fake_destroy_error = 1;
    assert(mp4_recorder_stop(recorder) == -1);
    mp4_recorder_destroy(recorder);
    assert(recorder->mux_created && recorder->pending_count == 1);
    assert(fake_packets[0].pack.mpAddr0[4] == 0x65);
    fake_destroy_error = 0;
    assert(mp4_recorder_stop(recorder) == -1); /* 缺失释放回调，记录故障。 */
    assert(recorder->pending_count == 0);
    mp4_recorder_destroy(recorder);
    assert(unlink(path) == 0);
}

static void test_byte_limit_and_unknown_release(void)
{
    Mp4RecorderContext *recorder = new_recorder();
    VENC_PACK_S pack;
    VENC_STREAM_S stream;
    unsigned int index;
    unsigned char *large = calloc(1U, 1024U * 1024U);
    assert(large != NULL);
    memset(&pack, 0, sizeof(pack)); memset(&stream, 0, sizeof(stream));
    pack.mpAddr0 = large; pack.mLen0 = 1024U * 1024U;
    stream.mpPack = &pack; stream.mPackCount = 1;
    for (index = 0; index < 8U; ++index) {
        assert(mp4_recorder_push_video(recorder, &stream, 1) == 0);
    }
    assert(recorder->pending_bytes == MP4_PENDING_BYTES_LIMIT);
    assert(push_audio(recorder) == -1 && !recorder->accepting_frames);
    assert(recorder->pending_bytes == MP4_PENDING_BYTES_LIMIT);
    free(large);
    finish_recorder(recorder, -1);

    recorder = new_recorder(); assert(push_video(recorder, 1) == 0);
    {
        MUX_VENC_STREAM_S bad;
        memset(&bad, 0, sizeof(bad)); bad.mVencStream.mSeq = 0xffffffffU;
        assert(fake_callback.callback(recorder, NULL,
               MPP_EVENT_RELEASE_VENC_STREAM, &bad) == SUCCESS);
    }
    assert(recorder->pending_count == 1U); /* 错误回调不能释放别的帧。 */
    assert(push_audio(recorder) == -1 && !recorder->accepting_frames);
    finish_recorder(recorder, -1);
}

static void test_close_input_during_production(void)
{
    Mp4RecorderContext *recorder = new_recorder();
    pthread_t first, second;
    unsigned int before, observed, attempts;
    fake_early = 1;
    fake_slow = 1; /* 确保关入口与生产重叠，不是生产已结束后的空测试。 */
    assert(pthread_create(&first, NULL, concurrent_producer, recorder) == 0);
    assert(pthread_create(&second, NULL, concurrent_producer, recorder) == 0);
    /* 先确认已有生产，再关入口；所有后续push应返回但不向SDK提交。 */
    for (attempts = 0; attempts < 1000U; ++attempts) {
        pthread_mutex_lock(&fake_lock); observed = fake_sends;
        pthread_mutex_unlock(&fake_lock);
        if (observed >= 10U) { break; }
        {
            struct timespec pause = {0, 1000000L};
            nanosleep(&pause, NULL);
        }
    }
    assert(attempts < 1000U);
    mp4_recorder_close_input(recorder);
    pthread_mutex_lock(&fake_lock); before = fake_sends;
    pthread_mutex_unlock(&fake_lock);
    pthread_join(first, NULL); pthread_join(second, NULL);
    assert(before >= 10U && before < 4000U);
    assert(fake_sends == before && recorder->pending_count == 0);
    finish_recorder(recorder, 0);
}

int main(void)
{
    test_delayed_release_and_switch();
    test_pair_and_early_release();
    test_bounded_failure_and_submit_failure();
    test_shutdown_failure_retains_ownership();
    test_byte_limit_and_unknown_release();
    test_close_input_during_production();
    puts("PASS: MP4 async copies, ADTS, delayed/early/pair releases, concurrency, limits and shutdown ownership");
    return 0;
}
