/*
 * 应用编排白盒测试：使用真实配置/生命周期代码及SDK类型，替换硬件模块。
 * 这里只检查依赖顺序、部分失败回滚和回调接线；不模拟板端驱动/推理。
 * 直接包含实现，避免为测试给生产接口增加“读取内部状态”的函数。
 */
#include <assert.h>
#include <stdio.h>
#include "../sample/ipc_camera/app/application.c"
#include "../sample/ipc_camera/app/config.c"

enum Stage {
    PLATFORM = 1, DISPLAY, CAPTURE, RTSP, VENC, OSD, MP4, AENC, NPU,
    OVERLAY, LCD_OVERLAY, ALARM, LINE, REGION, STAGE_END
};
enum Action { CREATE = 1, START, STOP, DESTROY, CLOSE_INPUT };
typedef struct Fake { int stage; int started; } Fake;
static int fail_stage, fail_action, fail_header, fail_idr, fail_push;
static int live, platform_live, capture_live, input_closed;
static int events[256], event_count, cases;
static int rtsp_video, mp4_video, rtsp_audio, mp4_audio, alarm_events;
static AudioAlarmEvent last_alarm;
static IpCameraContext *active;

static void record(int action, int stage)
{
    assert(event_count < (int)(sizeof(events) / sizeof(events[0])));
    events[event_count++] = action * 100 + stage;
}
static void reset(void)
{
    assert(live == 0 && !platform_live && !capture_live);
    fail_stage = fail_action = fail_header = fail_idr = fail_push = 0;
    input_closed = event_count = 0;
    rtsp_video = mp4_video = rtsp_audio = mp4_audio = alarm_events = 0;
    active = NULL;
    cases++;
}
static void *fake_create(size_t size, int stage)
{
    Fake *fake;
    record(CREATE, stage);
    if (stage == fail_stage && fail_action == CREATE) { return NULL; }
    fake = calloc(1, size);
    assert(fake != NULL);
    fake->stage = stage;
    live++;
    return fake;
}
static int fake_start(Fake *fake)
{
    assert(fake != NULL && !fake->started && platform_live);
    record(START, fake->stage);
    if (fake->stage == fail_stage && fail_action == START) { return -1; }
    fake->started = 1;
    return 0;
}
static int fake_stop(Fake *fake)
{
    if (fake == NULL) { return 0; }
    assert(fake->started && platform_live);
    record(STOP, fake->stage);
    fake->started = 0;
    return fake->stage == fail_stage && fail_action == STOP ? -1 : 0;
}
static void fake_destroy(Fake *fake)
{
    if (fake == NULL) { return; }
    assert(!fake->started);
    record(DESTROY, fake->stage);
    live--;
    free(fake);
}
/* 各fake保留真实配置副本，以验证回调opaque及跨模块对象是否正确接入。 */
#define DEFINE_CONTEXT(Type, Config) struct Type { Fake fake; Config config; }
DEFINE_CONTEXT(VideoDisplayContext, VideoDisplayConfig);
DEFINE_CONTEXT(RtspStreamContext, RtspStreamConfig);
DEFINE_CONTEXT(VideoEncoderContext, VideoEncoderConfig);
DEFINE_CONTEXT(TimeOsdContext, TimeOsdConfig);
DEFINE_CONTEXT(Mp4RecorderContext, Mp4RecorderConfig);
DEFINE_CONTEXT(AudioEncoderContext, AudioEncoderConfig);
DEFINE_CONTEXT(NpuDetectorContext, NpuDetectorConfig);
DEFINE_CONTEXT(DetectionOverlayContext, DetectionOverlayConfig);
DEFINE_CONTEXT(AudioAlarmContext, AudioAlarmConfig);
DEFINE_CONTEXT(LineCrossingContext, LineCrossingConfig);
DEFINE_CONTEXT(RegionIntrusionContext, RegionIntrusionConfig);

#define DEFINE_CREATE(prefix, Type, Config, stage) \
Type *prefix##_create(const Config *config) { \
    Type *context = fake_create(sizeof(*context), stage); \
    if (context != NULL) { context->config = *config; } \
    return context; \
}
#define DEFINE_STOP_DESTROY(prefix, Type) \
int prefix##_stop(Type *context) { \
    return fake_stop(context != NULL ? &context->fake : NULL); \
} \
void prefix##_destroy(Type *context) { \
    fake_destroy(context != NULL ? &context->fake : NULL); \
}
#define DEFINE_START(prefix, Type) \
int prefix##_start(Type *context) { return fake_start(&context->fake); }

DEFINE_CREATE(video_display, VideoDisplayContext, VideoDisplayConfig, DISPLAY)
DEFINE_CREATE(rtsp_stream, RtspStreamContext, RtspStreamConfig, RTSP)
DEFINE_CREATE(video_encoder, VideoEncoderContext, VideoEncoderConfig, VENC)
DEFINE_CREATE(time_osd, TimeOsdContext, TimeOsdConfig, OSD)
DEFINE_CREATE(mp4_recorder, Mp4RecorderContext, Mp4RecorderConfig, MP4)
DEFINE_CREATE(audio_encoder, AudioEncoderContext, AudioEncoderConfig, AENC)
DEFINE_CREATE(npu_detector, NpuDetectorContext, NpuDetectorConfig, NPU)
DEFINE_CREATE(audio_alarm, AudioAlarmContext, AudioAlarmConfig, ALARM)
DEFINE_CREATE(line_crossing, LineCrossingContext, LineCrossingConfig, LINE)
DEFINE_CREATE(region_intrusion, RegionIntrusionContext, RegionIntrusionConfig, REGION)
DEFINE_START(video_display, VideoDisplayContext)
DEFINE_START(rtsp_stream, RtspStreamContext)
DEFINE_START(time_osd, TimeOsdContext)
DEFINE_START(mp4_recorder, Mp4RecorderContext)
DEFINE_START(npu_detector, NpuDetectorContext)
DEFINE_START(detection_overlay, DetectionOverlayContext)
DEFINE_START(audio_alarm, AudioAlarmContext)
DEFINE_START(line_crossing, LineCrossingContext)
DEFINE_START(region_intrusion, RegionIntrusionContext)
DEFINE_STOP_DESTROY(video_display, VideoDisplayContext)
DEFINE_STOP_DESTROY(rtsp_stream, RtspStreamContext)
DEFINE_STOP_DESTROY(time_osd, TimeOsdContext)
DEFINE_STOP_DESTROY(mp4_recorder, Mp4RecorderContext)
DEFINE_STOP_DESTROY(npu_detector, NpuDetectorContext)
DEFINE_STOP_DESTROY(detection_overlay, DetectionOverlayContext)
DEFINE_STOP_DESTROY(audio_alarm, AudioAlarmContext)
DEFINE_STOP_DESTROY(line_crossing, LineCrossingContext)
DEFINE_STOP_DESTROY(region_intrusion, RegionIntrusionContext)

DetectionOverlayContext *detection_overlay_create(const DetectionOverlayConfig *config)
{
    DetectionOverlayContext *context = fake_create(sizeof(*context),
        config->region_handle_base == 100U ? OVERLAY : LCD_OVERLAY);
    if (context != NULL) {
        context->config = *config;
        assert(config->detector == active->npu_detector);
    }
    return context;
}
int platform_init(void)
{
    record(START, PLATFORM);
    if (fail_stage == PLATFORM && fail_action == START) { return -1; }
    assert(!platform_live);
    platform_live = 1;
    return 0;
}
int platform_deinit(void)
{
    assert(platform_live && live == 0 && !capture_live);
    record(STOP, PLATFORM);
    platform_live = 0;
    return fail_stage == PLATFORM && fail_action == STOP ? -1 : 0;
}
int video_capture_start(VideoCaptureContext *context)
{
    assert(platform_live && context->display != NULL);
    assert(context->display->fake.started);
    record(START, CAPTURE);
    if (fail_stage == CAPTURE && fail_action == START) { return -1; }
    capture_live = 1;
    return 0;
}
int video_capture_stop(VideoCaptureContext *context)
{
    assert(capture_live && context->display != NULL);
    record(STOP, CAPTURE);
    capture_live = 0;
    return fail_stage == CAPTURE && fail_action == STOP ? -1 : 0;
}
/* push应在取得指针快照后执行，而不是持消费者锁进入网络/MUX。 */
static void assert_consumer_lock_available(void)
{
    assert(active != NULL);
    assert(pthread_mutex_trylock(&active->consumers.lock) == 0);
    pthread_mutex_unlock(&active->consumers.lock);
}
int rtsp_stream_push_h264(RtspStreamContext *context,
    const unsigned char *header, size_t header_size,
    const unsigned char *data0, size_t size0,
    const unsigned char *data1, size_t size1,
    const unsigned char *data2, size_t size2, uint64_t pts, int key_frame)
{
    (void)header; (void)header_size; (void)data0; (void)size0;
    (void)data1; (void)size1; (void)data2; (void)size2;
    (void)pts; (void)key_frame;
    assert(context->fake.started);
    assert_consumer_lock_available();
    rtsp_video++;
    return 0;
}
int rtsp_stream_push_aac(RtspStreamContext *context,
    const unsigned char *data, size_t size, uint64_t pts)
{
    (void)data; (void)size; (void)pts;
    assert(context->fake.started);
    assert_consumer_lock_available();
    rtsp_audio++;
    return 0;
}
int mp4_recorder_push_video(Mp4RecorderContext *context,
    const VENC_STREAM_S *stream, int key_frame)
{
    (void)stream; (void)key_frame;
    assert(context->fake.started);
    assert_consumer_lock_available();
    mp4_video++;
    return 0;
}
int mp4_recorder_push_audio(Mp4RecorderContext *context, const AUDIO_STREAM_S *stream)
{
    (void)stream;
    assert(context->fake.started);
    assert_consumer_lock_available();
    mp4_audio++;
    return 0;
}
void mp4_recorder_close_input(Mp4RecorderContext *context)
{
    if (context != NULL) {
        record(CLOSE_INPUT, MP4);
        input_closed = 1;
    }
}
static void emit_video(VideoEncoderContext *context)
{
    VENC_PACK_S pack = {0};
    VENC_STREAM_S stream = {0};
    stream.mpPack = &pack;
    stream.mPackCount = 1;
    assert(context->config.frame_callback != NULL);
    assert(context->config.frame_callback(context->config.frame_callback_opaque,
        NULL, 0U, &stream, 1) == 0);
}
static void emit_audio(AudioEncoderContext *context)
{
    AUDIO_STREAM_S stream = {0};
    assert(context->config.frame_callback != NULL);
    assert(context->config.frame_callback(context->config.frame_callback_opaque,
        &stream) == 0);
}
int video_encoder_start(VideoEncoderContext *context)
{
    int result = fake_start(&context->fake);
    if (result == 0) { emit_video(context); } /* 此时只有RTSP，录像尚未接入。 */
    return result;
}
int audio_encoder_start(AudioEncoderContext *context)
{
    int result = fake_start(&context->fake);
    if (result == 0) { emit_audio(context); }
    return result;
}
int video_encoder_stop(VideoEncoderContext *context)
{
    assert(active->mp4_recorder == NULL || input_closed);
    emit_video(context); /* 模拟join前最后一次回调，消费者必须仍存在。 */
    return fake_stop(&context->fake);
}
int audio_encoder_stop(AudioEncoderContext *context)
{
    assert(input_closed);
    emit_audio(context);
    return fake_stop(&context->fake);
}
void video_encoder_destroy(VideoEncoderContext *context)
{
    fake_destroy(context != NULL ? &context->fake : NULL);
}
void audio_encoder_destroy(AudioEncoderContext *context)
{
    fake_destroy(context != NULL ? &context->fake : NULL);
}
int video_encoder_get_h264_header(const VideoEncoderContext *context,
    const unsigned char **header, size_t *size)
{
    static const unsigned char fake_header[] = {0, 0, 1, 0x67};
    assert(context->fake.started);
    *header = fake_header;
    *size = sizeof(fake_header);
    return fail_header ? -1 : 0;
}
int video_encoder_request_key_frame(VideoEncoderContext *context)
{
    assert(context->fake.started && active->consumers.recorder != NULL);
    return fail_idr ? -1 : 0;
}
int audio_alarm_push(AudioAlarmContext *context, const AudioAlarmEvent *event)
{
    assert(context != NULL && context->fake.started);
    last_alarm = *event;
    alarm_events++;
    return fail_push ? -1 : 0;
}
static IpCameraContext *new_application(void)
{
    IpCameraConfig config;
    ip_camera_config_defaults(&config);
    active = ip_camera_application_create(&config);
    assert(active != NULL);
    return active;
}
static void finish_application(int expected)
{
    int old_count;
    assert(ip_camera_application_stop(active) == expected);
    old_count = event_count;
    assert(ip_camera_application_stop(active) == expected); /* 幂等，不二次操作硬件。 */
    assert(event_count == old_count);
    ip_camera_application_destroy(active);
    active = NULL;
    assert(live == 0 && !platform_live && !capture_live);
}
static void test_defaults_and_options(void)
{
    IpCameraConfig config;
    IpCameraOptions options;
    char *plain[] = {"sample"};
    char *model[] = {"sample", "--npu-model", "/mnt/UDISK/test.nb"};
    char *npu[] = {"sample", "--npu-self-test", "model.nb", "frame.nv12", "extra"};
    char *audio[] = {"sample", "--audio-alarm-test", "alarm.wav", "extra"};
    char *unknown[] = {"sample", "--unknown"};
    char *empty[] = {"sample", "--npu-model", ""};
    char *null_path[] = {"sample", "--npu-model", NULL};
    reset();
    memset(&config, 0xff, sizeof(config));
    ip_camera_config_defaults(&config);
    ip_camera_config_defaults(NULL);
    assert(config.preview.device == 4 && config.preview.width == 1920);
    assert(config.preview.height == 1080 && config.preview.frame_rate == 20);
    assert(config.preview.timeout_ms == 200);
    assert(config.preview.pixel_format == MM_PIXEL_FORMAT_YVU_SEMIPLANAR_420);
    assert(config.display.rotation == 270 && config.display.display_width == 480);
    assert(config.display.display_height == 800 && config.rtsp.queue_capacity == 16);
    assert(config.video_encoder.vi_device == 0 && config.video_encoder.gop_size == 75);
    assert(config.video_encoder.bit_rate == 5242880);
    assert(config.recorder.segment_duration_seconds == 60);
    assert(config.recorder.max_segment_files == 10 && config.recorder.min_free_space_mb == 512);
    assert(config.recorder.h264_header == NULL && config.recorder.output_path == NULL);
    assert(config.audio_encoder.sample_rate == 16000 && config.audio_encoder.channels == 1);
    assert(config.audio_encoder.bit_width == 16 && config.audio_encoder.samples_per_frame == 1024);
    assert(config.npu.vi_device == 8 && config.npu.frame_rate == 10);
    assert(config.npu.width == 320 && config.npu.height == 320);
    assert(config.npu.mirror == 1 && config.npu.flip == 1);
    assert(config.npu.confidence_threshold == 0.25f && config.npu.nms_threshold == 0.45f);
    assert(config.overlay.target_vi_device == 0 && config.overlay.region_handle_base == 100);
    assert(config.lcd_overlay.target_vi_device == 4 && config.lcd_overlay.region_handle_base == 200);
    assert(!config.overlay.map_mirror && !config.overlay.map_flip);
    assert(config.overlay.detector == NULL && config.line.detector == NULL);
    assert(config.region.detector == NULL && config.video_encoder.frame_callback == NULL);
    assert(config.audio_encoder.frame_callback == NULL);
    assert(config.alarm.volume == 35 && config.alarm.cooldown_ms == 15000);
    assert(config.line.line_ax == 160 && config.line.line_bx == 160 && config.line.line_by == 320);
    assert(config.region.point_count == 4 && config.region.points[0].x == 160);
    assert(config.region.enter_confirm_snapshots == 3 && config.region.leave_confirm_snapshots == 3);
    assert(ip_camera_options_parse(1, plain, &options) == 0);
    assert(options.mode == IPC_CAMERA_MONITOR && options.input_path == NULL);
    assert(strcmp(options.model_path, IPC_CAMERA_DEFAULT_MODEL) == 0);
    assert(ip_camera_options_parse(3, model, &options) == 0 && options.model_path == model[2]);
    for (int count = 2; count <= 4; ++count) {
        assert(ip_camera_options_parse(count, npu, &options) == 0);
        assert(options.mode == IPC_CAMERA_NPU_SELF_TEST);
        assert(options.input_path == (count == 4 ? npu[3] : NULL));
    }
    for (int count = 2; count <= 3; ++count) {
        assert(ip_camera_options_parse(count, audio, &options) == 0);
        assert(options.mode == IPC_CAMERA_AUDIO_ALARM_TEST);
    }
    assert(ip_camera_options_parse(5, npu, &options) == -1);
    assert(ip_camera_options_parse(4, audio, &options) == -1);
    assert(ip_camera_options_parse(2, model, &options) == -1);
    assert(ip_camera_options_parse(2, unknown, &options) == -1);
    assert(ip_camera_options_parse(3, empty, &options) == -1);
    assert(ip_camera_options_parse(3, null_path, &options) == -1);
    assert(ip_camera_options_parse(0, plain, &options) == -1);
    assert(ip_camera_options_parse(1, NULL, &options) == -1);
    assert(ip_camera_options_parse(1, plain, NULL) == -1);
}
static void test_normal_and_callbacks(void)
{
    static const int start_order[] = {PLATFORM, DISPLAY, CAPTURE, RTSP, VENC,
        OSD, MP4, AENC, NPU, OVERLAY, LCD_OVERLAY, ALARM, LINE, REGION};
    static const int stop_order[] = {
        STOP*100+REGION, DESTROY*100+REGION, STOP*100+LINE, DESTROY*100+LINE,
        STOP*100+ALARM, DESTROY*100+ALARM, STOP*100+LCD_OVERLAY, DESTROY*100+LCD_OVERLAY,
        STOP*100+OVERLAY, DESTROY*100+OVERLAY, STOP*100+NPU, DESTROY*100+NPU,
        CLOSE_INPUT*100+MP4, STOP*100+AENC, DESTROY*100+AENC,
        STOP*100+OSD, DESTROY*100+OSD, STOP*100+VENC, DESTROY*100+VENC,
        STOP*100+MP4, DESTROY*100+MP4, STOP*100+RTSP, DESTROY*100+RTSP,
        STOP*100+CAPTURE, STOP*100+DISPLAY, DESTROY*100+DISPLAY, STOP*100+PLATFORM
    };
    LineCrossingEvent line = {0};
    RegionIntrusionEvent region = {0};
    int started = 0, begin;
    reset();
    new_application();
    assert(ip_camera_application_start(active, IPC_CAMERA_MONITOR) == 0);
    for (int index = 0; index < event_count; ++index) {
        if (events[index] / 100 == START) {
            assert(started < (int)(sizeof(start_order)/sizeof(start_order[0])));
            assert(events[index] % 100 == start_order[started++]);
        }
    }
    assert(started == (int)(sizeof(start_order)/sizeof(start_order[0])));
    assert(rtsp_video == 1 && mp4_video == 0 && rtsp_audio == 1 && mp4_audio == 1);
    assert(active->line_crossing->config.detector == active->npu_detector);
    assert(active->region_intrusion->config.detector == active->npu_detector);
    emit_video(active->video_encoder);
    assert(rtsp_video == 2 && mp4_video == 1);
    line.track_id = 7; line.sequence = 11; line.frame_pts_us = 1234;
    active->line_crossing->config.event_callback(
        active->line_crossing->config.event_callback_opaque, &line);
    assert(last_alarm.kind == AUDIO_ALARM_LINE_CROSSING && last_alarm.track_id == 7);
    assert(last_alarm.sequence == 11 && last_alarm.frame_pts_us == 1234);
    region.type = REGION_INTRUSION_LEAVE;
    active->region_intrusion->config.event_callback(
        active->region_intrusion->config.event_callback_opaque, &region);
    assert(alarm_events == 1);
    region.type = REGION_INTRUSION_ENTER; region.region_id = 2;
    active->region_intrusion->config.event_callback(
        active->region_intrusion->config.event_callback_opaque, &region);
    assert(alarm_events == 2 && last_alarm.kind == AUDIO_ALARM_REGION_ENTER && last_alarm.region_id == 2);
    assert(ip_camera_application_start(active, IPC_CAMERA_MONITOR) == -1);
    begin = event_count;
    finish_application(0);
    assert(event_count - begin == (int)(sizeof(stop_order)/sizeof(stop_order[0])));
    for (int index = 0; index < event_count - begin; ++index) {
        assert(events[begin + index] == stop_order[index]);
    }
    assert(rtsp_video == 3 && mp4_video == 2 && rtsp_audio == 2 && mp4_audio == 2);
}
static void test_failures(void)
{
    /* 每个服务的create/start失败点均测试，确保只清理实际拥有的资源。 */
    for (int action = CREATE; action <= START; ++action) {
        for (int stage = PLATFORM; stage < STAGE_END; ++stage) {
            int result;
            if (action == CREATE && (stage == PLATFORM || stage == CAPTURE)) { continue; }
            reset();
            fail_stage = stage; fail_action = action;
            new_application();
            result = ip_camera_application_start(active, IPC_CAMERA_MONITOR);
            assert(result == (stage == ALARM ? 0 : -1));
            if (stage != ALARM) {
                /* 失败点后不允许悄悄创建/启动其他服务。 */
                assert(event_count > 0 && events[event_count - 1] == action * 100 + stage);
            }
            if (stage == ALARM) {
                LineCrossingEvent event = {0};
                assert(active->audio_alarm == NULL && active->line_crossing != NULL);
                active->line_crossing->config.event_callback(
                    active->line_crossing->config.event_callback_opaque, &event);
                assert(alarm_events == 0);
            }
            finish_application(0);
        }
    }
    reset(); fail_header = 1; new_application();
    assert(ip_camera_application_start(active, IPC_CAMERA_MONITOR) == -1);
    finish_application(0);
    reset(); fail_idr = 1; new_application();
    assert(ip_camera_application_start(active, IPC_CAMERA_MONITOR) == 0);
    finish_application(0);
    /* stop错误不会跳过其他服务收尾，同时对重复stop保留失败结果。 */
    for (int stage = PLATFORM; stage < STAGE_END; ++stage) {
        reset(); new_application();
        assert(ip_camera_application_start(active, IPC_CAMERA_MONITOR) == 0);
        fail_stage = stage; fail_action = STOP;
        finish_application(-1);
    }
}
static void test_audio_mode_and_guards(void)
{
    reset(); new_application();
    assert(ip_camera_application_start(active, IPC_CAMERA_AUDIO_ALARM_TEST) == 0);
    assert(live == 1 && alarm_events == 1 && active->video_encoder == NULL);
    finish_application(0);
    for (int action = CREATE; action <= START; ++action) {
        reset(); fail_stage = ALARM; fail_action = action; new_application();
        assert(ip_camera_application_start(active, IPC_CAMERA_AUDIO_ALARM_TEST) == -1);
        finish_application(0);
    }
    reset(); fail_push = 1; new_application();
    assert(ip_camera_application_start(active, IPC_CAMERA_AUDIO_ALARM_TEST) == -1);
    finish_application(0);
    reset();
    assert(ip_camera_application_create(NULL) == NULL);
    assert(ip_camera_application_start(NULL, IPC_CAMERA_MONITOR) == -1);
    assert(ip_camera_application_stop(NULL) == 0);
    ip_camera_application_destroy(NULL);
    new_application();
    assert(ip_camera_application_start(active, IPC_CAMERA_NPU_SELF_TEST) == -1);
    assert(event_count == 0); /* 独立NPU自检不得走MPP应用生命周期。 */
    finish_application(0);
    reset(); new_application();
    assert(ip_camera_application_stop(active) == 0);
    assert(ip_camera_application_start(active, IPC_CAMERA_MONITOR) == -1);
    finish_application(0);
    reset(); new_application();
    assert(ip_camera_application_start(active, IPC_CAMERA_MONITOR) == 0);
    ip_camera_application_destroy(active); /* destroy兜底调用stop。 */
    active = NULL;
    assert(live == 0 && !platform_live && !capture_live);
}
int main(void)
{
    test_defaults_and_options();
    test_normal_and_callbacks();
    test_failures();
    test_audio_mode_and_guards();
    printf("Application/config tests passed: %d scenarios\n", cases);
    return 0;
}
