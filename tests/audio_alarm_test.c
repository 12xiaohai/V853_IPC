#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <pthread.h>
#ifdef _WIN32
/*
 * Windows winpthreads不支持condattr的MONOTONIC（返回EINVAL）。仅在主机
 * 测试中将单调截止时间换算为它接受的实时截止时间；板端生产代码不做回退。
 */
static int host_condattr_setclock(pthread_condattr_t *attr, clockid_t clock)
{
    assert(clock == CLOCK_MONOTONIC);
    return pthread_condattr_setclock(attr, CLOCK_REALTIME);
}
static int host_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex,
                               const struct timespec *deadline)
{
    struct timespec mono, real, converted;
    long long relative_ns;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    relative_ns = (long long)(deadline->tv_sec - mono.tv_sec) * 1000000000LL +
                  deadline->tv_nsec - mono.tv_nsec;
    if (relative_ns < 0) { relative_ns = 0; }
    converted.tv_sec = real.tv_sec + (time_t)(relative_ns / 1000000000LL);
    converted.tv_nsec = real.tv_nsec + (long)(relative_ns % 1000000000LL);
    if (converted.tv_nsec >= 1000000000L) {
        converted.tv_sec++; converted.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(condition, mutex, &converted);
}
#define pthread_condattr_setclock host_condattr_setclock
#define pthread_cond_timedwait host_cond_timedwait
#endif

/* 将实现放入测试翻译单元，才能在不增加生产API的前提下检查私有队列状态。 */
#include "../sample/ipc_camera/app/wav_reader.c"
#include "../sample/ipc_camera/app/audio_alarm.c"
#ifdef _WIN32
#undef pthread_condattr_setclock
#undef pthread_cond_timedwait
#endif

enum { FAKE_NORMAL, FAKE_SEND_ERROR, FAKE_NO_RELEASE, FAKE_CREATE_ERROR,
       FAKE_NO_EOF, FAKE_DESTROY_ERROR };
static atomic_int fake_mode;
static atomic_int fake_owned;
static atomic_int fake_frames;
static MPPCallbackInfo fake_callback;

int AW_MPI_AO_CreateChn(int device, int channel)
{
    assert(device == 0 && channel == 0);
    if (atomic_load(&fake_mode) == FAKE_CREATE_ERROR) {
        return -1;
    }
    assert(atomic_exchange(&fake_owned, 1) == 0); /* 不能同时播放两次。 */
    return SUCCESS;
}
int AW_MPI_AO_DestroyChn(int device, int channel)
{
    (void)device; (void)channel;
    if (atomic_load(&fake_mode) == FAKE_DESTROY_ERROR) {
        return -1;
    }
    assert(atomic_exchange(&fake_owned, 0) == 1);
    return SUCCESS;
}
int AW_MPI_AO_RegisterCallback(int device, int channel, MPPCallbackInfo *callback)
{
    (void)device; (void)channel;
    fake_callback = *callback;
    return SUCCESS;
}
int AW_MPI_AO_SetPcmCardType(int device, int channel, int card)
{ (void)device; (void)channel; (void)card; return SUCCESS; }
int AW_MPI_AO_StartChn(int device, int channel)
{ (void)device; (void)channel; return SUCCESS; }
int AW_MPI_AO_StopChn(int device, int channel)
{ (void)device; (void)channel; return SUCCESS; }
int AW_MPI_AO_SetDevVolume(int device, int volume)
{ (void)device; assert(volume == 35); return SUCCESS; }
int AW_MPI_AO_SetSoftVolume(int device, int volume)
{ (void)device; assert(volume == 0); return SUCCESS; }
int AW_MPI_AO_SetChnMute(int device, int channel, int mute)
{ (void)device; (void)channel; assert(!mute); return SUCCESS; }
int AW_MPI_AO_SendFrame(int device, int channel, AUDIO_FRAME_S *frame, int timeout)
{
    MPP_CHN_S chn = {MOD_ID_AO, device, channel};
    assert(timeout == 100 && frame->mBitwidth == AUDIO_BIT_WIDTH_16);
    assert(frame->mSamplerate == 16000 && frame->mSoundmode == AUDIO_SOUND_MODE_MONO);
    assert(frame->mLen > 0 && frame->mLen <= 2048 && frame->mLen % 2 == 0);
    assert(((unsigned char *)frame->mpAddr)[0] == 0x55); /* 不是RIFF/WAVE头。 */
    atomic_fetch_add(&fake_frames, 1);
    if (atomic_load(&fake_mode) == FAKE_SEND_ERROR) {
        return -1;
    }
    if (atomic_load(&fake_mode) != FAKE_NO_RELEASE) {
        /* 故意在SendFrame返回之前执行释放回调，覆盖容易遗漏的竞态。 */
        fake_callback.callback(fake_callback.cookie, &chn,
                               MPP_EVENT_RELEASE_AUDIO_BUFFER, frame);
    }
    return SUCCESS;
}
int AW_MPI_AO_SetStreamEof(int device, int channel, int eof, int drain)
{
    MPP_CHN_S chn = {MOD_ID_AO, device, channel};
    assert(eof && drain);
    if (atomic_load(&fake_mode) != FAKE_NO_EOF) {
        fake_callback.callback(fake_callback.cookie, &chn, MPP_EVENT_NOTIFY_EOF, NULL);
    }
    return SUCCESS;
}

static void write_le32(unsigned char *p, unsigned int value)
{
    for (unsigned int i = 0; i < 4; ++i) { p[i] = (unsigned char)(value >> (i * 8)); }
}

/* 构造带奇数长度JUNK块的WAV，data偏移不是44字节；最后一帧也非1024样本。 */
static unsigned char fixture[56 + 3000];
static void initialize_fixture(void)
{
    memset(fixture, 0, sizeof(fixture));
    memcpy(fixture, "RIFF", 4); write_le32(fixture + 4, sizeof(fixture) - 8);
    memcpy(fixture + 8, "WAVEfmt ", 8); write_le32(fixture + 16, 16);
    fixture[20] = fixture[22] = 1;
    write_le32(fixture + 24, 16000); write_le32(fixture + 28, 32000);
    fixture[32] = 2; fixture[34] = 16;
    memcpy(fixture + 36, "JUNK", 4); write_le32(fixture + 40, 3);
    memcpy(fixture + 48, "data", 4); write_le32(fixture + 52, 3000);
    memset(fixture + 56, 0x55, 3000);
}

static int parse_fixture(size_t size, WavPcm *pcm)
{
    FILE *file = tmpfile();
    int result;
    assert(file != NULL && fwrite(fixture, 1, size, file) == size);
    result = wav_pcm_read(file, pcm);
    fclose(file);
    return result;
}

static void test_parser(void)
{
    WavPcm pcm;
    initialize_fixture();
    assert(parse_fixture(sizeof(fixture), &pcm) == 0);
    assert(pcm.size == 3000 && pcm.sample_rate == 16000 && pcm.data[0] == 0x55);
    wav_pcm_free(&pcm);
    assert(parse_fixture(sizeof(fixture) - 1, &pcm) == -1); /* 截断 */
    fixture[20] = 3; assert(parse_fixture(sizeof(fixture), &pcm) == -1); /* float */
    fixture[20] = 1; fixture[22] = 2;
    assert(parse_fixture(sizeof(fixture), &pcm) == -1); /* 立体声 */
    initialize_fixture(); write_le32(fixture + 24, 12345);
    assert(parse_fixture(sizeof(fixture), &pcm) == -1); /* 不支持的采样率 */
    initialize_fixture(); write_le32(fixture + 52, 0);
    assert(parse_fixture(sizeof(fixture), &pcm) == -1); /* 空data */
    initialize_fixture(); write_le32(fixture + 52, 0xffffffffU);
    assert(parse_fixture(sizeof(fixture), &pcm) == -1); /* 巨大chunk/溢出 */
    initialize_fixture(); fixture[34] = 8;
    assert(parse_fixture(sizeof(fixture), &pcm) == -1);
    initialize_fixture();
}

static void pause_ms(unsigned int milliseconds)
{
    struct timespec delay = {(time_t)(milliseconds / 1000),
                             (long)(milliseconds % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}

static AudioAlarmEvent event = {AUDIO_ALARM_LINE_CROSSING, 7, 0, 123, 456};
static AudioAlarmContext *new_alarm(void)
{
    AudioAlarmConfig config = {"output/alarm_test.wav", 0, 0, 35, 15000};
    AudioAlarmContext *context = audio_alarm_create(&config);
    assert(context != NULL && audio_alarm_start(context) == 0);
    return context;
}

static void wait_state(AudioAlarmContext *context, int wanted)
{
    for (unsigned int attempt = 0; attempt < 5000; ++attempt) {
        int ready;
        pthread_mutex_lock(&context->mutex);
        ready = wanted == 0 ? context->completed > 0 :
                wanted == 1 ? context->disabled : context->frame_pending;
        pthread_mutex_unlock(&context->mutex);
        if (ready) { return; }
        pause_ms(1);
    }
    assert(!"alarm state timeout");
}

static void *producer(void *opaque)
{
    AudioAlarmContext *context = opaque;
    for (unsigned int i = 0; i < 1000; ++i) {
        assert(audio_alarm_push(context, &event) >= 0);
    }
    return NULL;
}

static void test_playback(void)
{
    AudioAlarmContext *context;
    pthread_t first, second;
    uint64_t before;
    atomic_store(&fake_mode, FAKE_NORMAL);
    context = new_alarm();
    assert(pthread_create(&first, NULL, producer, context) == 0);
    assert(pthread_create(&second, NULL, producer, context) == 0);
    pthread_join(first, NULL); pthread_join(second, NULL);
    wait_state(context, 0);
    assert(audio_alarm_push(context, &event) == 1); /* 冷却抑制 */
    assert(audio_alarm_stop(context) == 0);
    assert(context->completed == 1 && context->suppressed > 0);
    assert(atomic_load(&fake_frames) == 2 && atomic_load(&fake_owned) == 0);
    assert(audio_alarm_push(context, &event) == -1);
    audio_alarm_destroy(context);

    atomic_store(&fake_mode, FAKE_SEND_ERROR);
    context = new_alarm(); assert(audio_alarm_push(context, &event) == 0);
    wait_state(context, 1);
    assert(audio_alarm_push(context, &event) == -1 && context->failed == 1);
    audio_alarm_destroy(context); assert(atomic_load(&fake_owned) == 0);

    atomic_store(&fake_mode, FAKE_CREATE_ERROR);
    context = new_alarm(); assert(audio_alarm_push(context, &event) == 0);
    wait_state(context, 1); audio_alarm_destroy(context); /* 不销毁未拥有的通道 */

    atomic_store(&fake_mode, FAKE_NO_RELEASE);
    context = new_alarm(); assert(audio_alarm_push(context, &event) == 0);
    wait_state(context, 2);
    before = monotonic_ms();
    assert(audio_alarm_stop(context) == 0 && monotonic_ms() - before < 500);
    assert(context->interrupted == 1 && atomic_load(&fake_owned) == 0);
    audio_alarm_destroy(context);

    /* 无回调时应超时故障隔离，而不是永久卡死；EOF通知丢失同理。 */
    context = new_alarm(); assert(audio_alarm_push(context, &event) == 0);
    wait_state(context, 1); assert(context->failed == 1); audio_alarm_destroy(context);
    atomic_store(&fake_mode, FAKE_NO_EOF);
    context = new_alarm(); assert(audio_alarm_push(context, &event) == 0);
    wait_state(context, 1); assert(context->failed == 1); audio_alarm_destroy(context);

    /* Destroy失败必须保留PCM/cookie；后续重试成功后才真正释放。 */
    atomic_store(&fake_mode, FAKE_DESTROY_ERROR);
    context = new_alarm(); assert(audio_alarm_push(context, &event) == 0);
    wait_state(context, 1);
    audio_alarm_destroy(context);
    assert(context->pcm.data != NULL && context->channel_created);
    atomic_store(&fake_mode, FAKE_NORMAL);
    audio_alarm_destroy(context); assert(atomic_load(&fake_owned) == 0);
}

int main(void)
{
    FILE *file;
    AudioAlarmConfig missing = {"output/not-present-alarm.wav", 0, 0, 35, 15000};
    AudioAlarmContext *context;
    test_parser();
    file = fopen("output/alarm_test.wav", "wb");
    assert(file != NULL && fwrite(fixture, 1, sizeof(fixture), file) == sizeof(fixture));
    fclose(file);
    context = audio_alarm_create(&missing);
    assert(context != NULL && audio_alarm_start(context) == -1);
    audio_alarm_destroy(context);
    test_playback();
    remove("output/alarm_test.wav"); /* 只删除本测试生成的文件，不触碰任何录音资产。 */
    puts("audio_alarm_test: WAV parser, concurrency, cooldown, AO errors, callback timeout and safe exit passed");
    return 0;
}
