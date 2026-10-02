#include "application.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "video_capture.h"

#include <utils/plat_log.h>

/* 规则线程只复制事件入队，耗时的WAV播放交给独立AO线程。 */
static void dispatch_line_alarm(void *opaque, const LineCrossingEvent *event)
{
    AudioAlarmEvent alarm;
    if (opaque == NULL || event == NULL) {
        return;
    }
    memset(&alarm, 0, sizeof(alarm));
    alarm.kind = AUDIO_ALARM_LINE_CROSSING;
    alarm.track_id = event->track_id;
    alarm.sequence = event->sequence;
    alarm.frame_pts_us = event->frame_pts_us;
    (void)audio_alarm_push(opaque, &alarm);
}

static void dispatch_region_alarm(void *opaque, const RegionIntrusionEvent *event)
{
    AudioAlarmEvent alarm;
    /* 离开仍由区域模块记录，但不是危险进入事件，不触发声音。 */
    if (opaque == NULL || event == NULL || event->type != REGION_INTRUSION_ENTER) {
        return;
    }
    memset(&alarm, 0, sizeof(alarm));
    alarm.kind = AUDIO_ALARM_REGION_ENTER;
    alarm.track_id = event->track_id;
    alarm.region_id = event->region_id;
    alarm.sequence = event->sequence;
    alarm.frame_pts_us = event->frame_pts_us;
    (void)audio_alarm_push(opaque, &alarm);
}

/*
 * 一帧编码数据可能同时被多个消费者使用。编码线程只调用一次适配回调，
 * 这里再把同一块只读缓冲分别交给RTSP和MP4；两个消费者都必须在返回前
 * 完成深拷贝，因为回调返回后编码缓冲就会归还MPP。
 */
typedef struct MediaConsumers {
    pthread_mutex_t lock; /* 仅保护消费者指针，不在锁内调用RTSP/MUX。 */
    RtspStreamContext *rtsp;
    Mp4RecorderContext *recorder;
} MediaConsumers;

/*
 * 这是VENC与各码流消费者之间的适配函数。RTSP会深拷贝帧，MP4录像器
 * 同样深拷贝帧，交给异步MUX；不等待分段处理完成才归还编码缓冲。
 */
static int dispatch_encoded_video(
    void *opaque,
    const unsigned char *header,
    size_t header_size,
    const VENC_STREAM_S *stream,
    int key_frame)
{
    MediaConsumers *consumers = opaque;
    RtspStreamContext *rtsp;
    Mp4RecorderContext *recorder;
    const VENC_PACK_S *pack;
    int result = 0;

    if (consumers == NULL || stream == NULL || stream->mpPack == NULL ||
        stream->mPackCount == 0U) {
        return -1;
    }
    pack = &stream->mpPack[0];

    /* 主线程可能在启动阶段接入录像器，先在锁内取得稳定指针快照。 */
    pthread_mutex_lock(&consumers->lock);
    rtsp = consumers->rtsp;
    recorder = consumers->recorder;
    pthread_mutex_unlock(&consumers->lock);

    if (rtsp != NULL &&
        rtsp_stream_push_h264(rtsp,
                              header,
                              header_size,
                              pack->mpAddr0,
                              pack->mLen0,
                              pack->mpAddr1,
                              pack->mLen1,
                              pack->mpAddr2,
                              pack->mLen2,
                              (unsigned long long)pack->mPTS,
                              key_frame) != 0) {
        result = -1;
    }

    if (recorder != NULL &&
        mp4_recorder_push_video(recorder,
                                stream,
                                key_frame) != 0) {
        result = -1;
    }
    return result;
}

/* 把同一帧AAC分发给RTSP和MP4，并保留AENC给出的原始PTS。 */
static int dispatch_encoded_audio(void *opaque,
                                  const AUDIO_STREAM_S *stream)
{
    MediaConsumers *consumers = opaque;
    RtspStreamContext *rtsp;
    Mp4RecorderContext *recorder;
    int result = 0;

    if (consumers == NULL || stream == NULL) {
        return -1;
    }
    pthread_mutex_lock(&consumers->lock);
    rtsp = consumers->rtsp;
    recorder = consumers->recorder;
    pthread_mutex_unlock(&consumers->lock);

    if (rtsp != NULL &&
        rtsp_stream_push_aac(rtsp,
                             stream->pStream,
                             stream->mLen,
                             (unsigned long long)stream->mTimeStamp) != 0) {
        result = -1;
    }
    if (recorder != NULL &&
        mp4_recorder_push_audio(recorder, stream) != 0) {
        result = -1;
    }
    return result;
}


/*
 * 应用级资源所有者：配置、服务指针、启动状态和消费者锁集中管理。
 * main只看见生命周期接口；这里不重复实现媒体处理和模块工作线程。
 */
struct IpCameraContext {
    IpCameraConfig config;
    VideoCaptureContext video_capture;
    MediaConsumers consumers;
    VideoDisplayContext *video_display;
    RtspStreamContext *rtsp_stream;
    VideoEncoderContext *video_encoder;
    TimeOsdContext *time_osd;
    Mp4RecorderContext *mp4_recorder;
    AudioEncoderContext *audio_encoder;
    NpuDetectorContext *npu_detector;
    DetectionOverlayContext *detection_overlay;
    DetectionOverlayContext *lcd_detection_overlay;
    LineCrossingContext *line_crossing;
    RegionIntrusionContext *region_intrusion;
    AudioAlarmContext *audio_alarm;
    int platform_initialized;
    int video_capture_started;
    int video_display_started;
    int rtsp_stream_started;
    int video_encoder_started;
    int time_osd_started;
    int mp4_recorder_started;
    int audio_encoder_started;
    int npu_detector_started;
    int detection_overlay_started;
    int lcd_detection_overlay_started;
    int line_crossing_started;
    int region_intrusion_started;
    int start_attempted; /* 同一份上下文不允许重复启动。 */
    int stop_complete;
    int stop_result;
};

IpCameraContext *ip_camera_application_create(const IpCameraConfig *config)
{
    IpCameraContext *ctx;
    if (config == NULL) {
        return NULL;
    }
    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }
    ctx->config = *config; /* 路径借用静态常量/argv，必须存活到destroy。 */
    if (pthread_mutex_init(&ctx->consumers.lock, NULL) != 0) {
        free(ctx);
        return NULL;
    }
    /* 纯配置复制到VI运行时结构，线程/硬件状态保持全0。 */
    ctx->video_capture.device = config->preview.device;
    ctx->video_capture.isp_device = config->preview.isp_device;
    ctx->video_capture.channel = config->preview.channel;
    ctx->video_capture.width = config->preview.width;
    ctx->video_capture.height = config->preview.height;
    ctx->video_capture.frame_rate = config->preview.frame_rate;
    ctx->video_capture.timeout_ms = config->preview.timeout_ms;
    ctx->video_capture.pixel_format = config->preview.pixel_format;

    ctx->config.video_encoder.frame_callback = dispatch_encoded_video;
    ctx->config.video_encoder.frame_callback_opaque = &ctx->consumers;
    ctx->config.audio_encoder.frame_callback = dispatch_encoded_audio;
    ctx->config.audio_encoder.frame_callback_opaque = &ctx->consumers;
    return ctx;
}

/* 显示先于采集启动，避免VI线程拿到帧后没有显示目标。 */
static int start_preview(IpCameraContext *ctx)
{
    ctx->video_display = video_display_create(&ctx->config.display);
    if (ctx->video_display == NULL) {
        aloge("[Main] Video display context allocation failed");
        return -1;
    }

    if (video_display_start(ctx->video_display) != 0) {
        aloge("[Main] Video display initialization failed");
        return -1;
    }
    ctx->video_display_started = 1;
    ctx->video_capture.display = ctx->video_display;

    if (video_capture_start(&ctx->video_capture) != 0) {
        aloge("[Main] Video capture initialization failed");
        return -1;
    }
    ctx->video_capture_started = 1;
    return 0;
}

/* RTSP消费者先准备好，VENC启动后即可分发首个关键帧。 */
static int start_rtsp(IpCameraContext *ctx)
{
    ctx->rtsp_stream = rtsp_stream_create(&ctx->config.rtsp);
    if (ctx->rtsp_stream == NULL) {
        aloge("[Main] RTSP context allocation failed");
        return -1;
    }
    if (rtsp_stream_start(ctx->rtsp_stream) != 0) {
        aloge("[Main] RTSP video service initialization failed");
        return -1;
    }
    ctx->rtsp_stream_started = 1;
    ctx->consumers.rtsp = ctx->rtsp_stream; /* VENC线程尚未创建，无并发访问。 */
    return 0;
}

/* 回调opaque必须存活到编码线程退出。 */
static int start_video_encoder(IpCameraContext *ctx)
{
    ctx->video_encoder = video_encoder_create(&ctx->config.video_encoder);
    if (ctx->video_encoder == NULL) {
        aloge("[Main] Video encoder context allocation failed");
        return -1;
    }

    if (video_encoder_start(ctx->video_encoder) != 0) {
        aloge("[Main] Video encoder initialization failed");
        return -1;
    }
    ctx->video_encoder_started = 1;
    return 0;
}

/* RGN依赖VENC，启动晚于VENC，停止早于VENC。 */
static int start_time_osd(IpCameraContext *ctx)
{
    ctx->time_osd = time_osd_create(&ctx->config.time_osd);
    if (ctx->time_osd == NULL) {
        aloge("[Main] Time OSD context allocation failed");
        return -1;
    }
    if (time_osd_start(ctx->time_osd) != 0) {
        aloge("[Main] Time OSD initialization failed");
        return -1;
    }
    ctx->time_osd_started = 1;
    return 0;
}

/* SPS/PPS来自已启动的VENC；录像器接入后请求新IDR。 */
static int start_recorder(IpCameraContext *ctx)
{
    const unsigned char *h264_header = NULL;
    size_t h264_header_size = 0U;

    if (video_encoder_get_h264_header(ctx->video_encoder,
                                      &h264_header,
                                      &h264_header_size) != 0) {
        aloge("[Main] Get H.264 SPS/PPS for MP4 failed");
        return -1;
    }
    ctx->config.recorder.h264_header = h264_header;
    ctx->config.recorder.h264_header_size = h264_header_size;
    ctx->mp4_recorder = mp4_recorder_create(&ctx->config.recorder);
    if (ctx->mp4_recorder == NULL) {
        aloge("[Main] MP4 recorder context allocation failed");
        return -1;
    }
    if (mp4_recorder_start(ctx->mp4_recorder) != 0) {
        aloge("[Main] MP4 recorder initialization failed");
        return -1;
    }
    ctx->mp4_recorder_started = 1;
    pthread_mutex_lock(&ctx->consumers.lock);
    ctx->consumers.recorder = ctx->mp4_recorder;
    pthread_mutex_unlock(&ctx->consumers.lock);
    /* 避免录像器等待完整GOP，接入后立即请求一个新的MP4起始关键帧。 */
    if (video_encoder_request_key_frame(ctx->video_encoder) != 0) {
        alogw("[Main] Request key frame for MP4 failed; waiting for next GOP");
    }
    return 0;
}

/* AAC与H.264共享消费者，保留原始PTS及各消费者的深拷贝策略。 */
static int start_audio_encoder(IpCameraContext *ctx)
{
    ctx->audio_encoder = audio_encoder_create(&ctx->config.audio_encoder);
    if (ctx->audio_encoder == NULL) {
        aloge("[Main] Audio encoder context allocation failed");
        return -1;
    }
    if (audio_encoder_start(ctx->audio_encoder) != 0) {
        aloge("[Main] AAC audio encoder initialization failed");
        return -1;
    }
    ctx->audio_encoder_started = 1;
    return 0;
}

/* VIPP8和AWNN的创建/线程细节继续由NPU模块管理。 */
static int start_detector(IpCameraContext *ctx)
{
    ctx->npu_detector = npu_detector_create(&ctx->config.npu);
    if (ctx->npu_detector == NULL) {
        aloge("[Main] NPU detector context allocation failed");
        return -1;
    }
    if (npu_detector_start(ctx->npu_detector) != 0) {
        aloge("[Main] Realtime NPU person detector initialization failed");
        return -1;
    }
    ctx->npu_detector_started = 1;
    return 0;
}

/* 两个ORL消费者共享NPU快照，各自附着编码和LCD的VIPP。 */
static int start_overlays(IpCameraContext *ctx)
{
    ctx->detection_overlay = detection_overlay_create(&ctx->config.overlay);
    if (ctx->detection_overlay == NULL) {
        aloge("[Main] Detection overlay context allocation failed");
        return -1;
    }
    if (detection_overlay_start(ctx->detection_overlay) != 0) {
        aloge("[Main] Detection overlay initialization failed");
        return -1;
    }
    ctx->detection_overlay_started = 1;

    /*
     * LCD预览来自VIPP 4，不会继承VIPP 0上的ORL。因此用独立句柄
     * 200..215把同一份检测结果附着到VIPP 4。检测框会先成为
     * 1920x1080预览帧的一部分，再和图像一起被G2D旋转270度并送往LCD，
     * 所以不需要额外计算480x800的坐标。
     */
    ctx->lcd_detection_overlay =
        detection_overlay_create(&ctx->config.lcd_overlay);
    if (ctx->lcd_detection_overlay == NULL) {
        aloge("[Main] LCD detection overlay context allocation failed");
        return -1;
    }
    if (detection_overlay_start(ctx->lcd_detection_overlay) != 0) {
        aloge("[Main] LCD detection overlay initialization failed");
        return -1;
    }
    ctx->lcd_detection_overlay_started = 1;
    return 0;
}

/* WAV不可用只降级声音；声音自检则要求该服务必须成功。 */
static int start_audio_alarm(IpCameraContext *ctx)
{
    ctx->audio_alarm = audio_alarm_create(&ctx->config.alarm);
    if (ctx->audio_alarm == NULL || audio_alarm_start(ctx->audio_alarm) != 0) {
        audio_alarm_destroy(ctx->audio_alarm);
        ctx->audio_alarm = NULL;
        return -1;
    }
    return 0;
}

/* 规则晚于NPU和报警消费者启动；事件回调只复制入队。 */
static int start_rules(IpCameraContext *ctx)
{
    ctx->line_crossing = line_crossing_create(&ctx->config.line);
    if (ctx->line_crossing == NULL) {
        aloge("[Main] Line-crossing context allocation failed");
        return -1;
    }
    if (line_crossing_start(ctx->line_crossing) != 0) {
        aloge("[Main] Line-crossing detector initialization failed");
        return -1;
    }
    ctx->line_crossing_started = 1;

    /*
     * 阶段9.5把NPU模型画面右半边作为禁入区域，顶点按顺序围成多边形。
     * 使用与越线相同的底边中点，但独立维护轨迹和区域状态。连续3个有效
     * 快照在内报告enter，连续3个在外报告leave；停留不重复报告。
     * 阶段9.6接入音频回调，仅ENTER报警；区域边界仍未绘制。
     */
    ctx->region_intrusion = region_intrusion_create(&ctx->config.region);
    if (ctx->region_intrusion == NULL) {
        aloge("[Main] Region-intrusion context allocation failed");
        return -1;
    }
    if (region_intrusion_start(ctx->region_intrusion) != 0) {
        aloge("[Main] Region-intrusion detector initialization failed");
        return -1;
    }
    ctx->region_intrusion_started = 1;
    return 0;
}


int ip_camera_application_start(IpCameraContext *ctx, IpCameraRunMode mode)
{
    if (ctx == NULL || ctx->start_attempted || ctx->stop_complete ||
        (mode != IPC_CAMERA_MONITOR && mode != IPC_CAMERA_AUDIO_ALARM_TEST)) {
        return -1;
    }
    ctx->start_attempted = 1;
    if (platform_init() != 0) {
        aloge("[App] Platform initialization failed");
        return -1;
    }
    ctx->platform_initialized = 1;

    /* 声音自检只创建MPP与AO，不依赖网络、摄像头或NPU。 */
    if (mode == IPC_CAMERA_AUDIO_ALARM_TEST) {
        AudioAlarmEvent event = {0};
        if (start_audio_alarm(ctx) != 0) {
            aloge("[App] Audio alarm self-test initialization failed");
            return -1;
        }
        event.kind = AUDIO_ALARM_LINE_CROSSING;
        return audio_alarm_push(ctx->audio_alarm, &event) == 0 ? 0 : -1;
    }

    /* 保留原有启动顺序；||短路确保某一步失败后不再启动后续服务。 */
    if (start_preview(ctx) != 0 || start_rtsp(ctx) != 0 ||
        start_video_encoder(ctx) != 0 || start_time_osd(ctx) != 0 ||
        start_recorder(ctx) != 0 || start_audio_encoder(ctx) != 0 ||
        start_detector(ctx) != 0) {
        return -1;
    }
    /* 纯参数与运行时依赖分离：这里才接入已经创建的NPU对象。 */
    ctx->config.overlay.detector = ctx->npu_detector;
    ctx->config.lcd_overlay.detector = ctx->npu_detector;
    if (start_overlays(ctx) != 0) {
        return -1;
    }
    if (start_audio_alarm(ctx) != 0) {
        alogw("[App] Local audio alarm unavailable; monitoring continues without sound");
    }
    ctx->config.line.detector = ctx->npu_detector;
    ctx->config.region.detector = ctx->npu_detector;
    ctx->config.line.event_callback = dispatch_line_alarm;
    ctx->config.line.event_callback_opaque = ctx->audio_alarm;
    ctx->config.region.event_callback = dispatch_region_alarm;
    ctx->config.region.event_callback_opaque = ctx->audio_alarm;
    return start_rules(ctx);
}

int ip_camera_application_stop(IpCameraContext *ctx)
{
    int result = 0;
    if (ctx == NULL) {
        return 0;
    }
    if (ctx->stop_complete) {
        return ctx->stop_result;
    }
    /*
     * 停止按资源依赖而非简单反转启动列表：
     * 规则 -> AO -> ORL -> NPU -> 关闭MP4入口 -> AENC -> OSD -> VENC
     * -> MUX -> RTSP -> VI -> VO -> MPP。编码回调结束前不能销毁消费者。
     */
    if (ctx->region_intrusion_started && region_intrusion_stop(ctx->region_intrusion) != 0) {
        result = -1;
    }
    region_intrusion_destroy(ctx->region_intrusion);
    ctx->region_intrusion = NULL;

    if (ctx->line_crossing_started &&
        line_crossing_stop(ctx->line_crossing) != 0) {
        result = -1;
    }
    line_crossing_destroy(ctx->line_crossing);
    ctx->line_crossing = NULL;

    /* 先join两个规则线程，保证销毁报警队列后不会再有回调入队。MPP此时仍有效。 */
    if (audio_alarm_stop(ctx->audio_alarm) != 0) {
        result = -1;
    }
    audio_alarm_destroy(ctx->audio_alarm);
    ctx->audio_alarm = NULL;

    if (ctx->lcd_detection_overlay_started &&
        detection_overlay_stop(ctx->lcd_detection_overlay) != 0) {
        result = -1;
    }
    detection_overlay_destroy(ctx->lcd_detection_overlay);
    ctx->lcd_detection_overlay = NULL;

    if (ctx->detection_overlay_started &&
        detection_overlay_stop(ctx->detection_overlay) != 0) {
        result = -1;
    }
    detection_overlay_destroy(ctx->detection_overlay);
    ctx->detection_overlay = NULL;

    /*
     * NPU线程停止后才释放模型和VIPP 8，确保推理线程不会访问
     * 已经销毁的VI帧或AWNN上下文。
     */
    if (ctx->npu_detector_started && npu_detector_stop(ctx->npu_detector) != 0) {
        result = -1;
    }
    npu_detector_destroy(ctx->npu_detector);
    ctx->npu_detector = NULL;

    /*
     * 先关闭MP4入口，使正在收尾的编码线程不再送入新录像帧。
     * 这里只关入口，仍保留MUX和它拥有的副本；生产者join后才统一收尾。
     */
    mp4_recorder_close_input(ctx->mp4_recorder);
    alogd("[Main] Stopping audio encoder");
    if (ctx->audio_encoder_started && audio_encoder_stop(ctx->audio_encoder) != 0) {
        result = -1;
    }
    audio_encoder_destroy(ctx->audio_encoder);
    ctx->audio_encoder = NULL;

    /* 必须先从VENC解绑并销毁RGN，之后才能销毁VENC通道本身。 */
    if (ctx->time_osd_started && time_osd_stop(ctx->time_osd) != 0) {
        result = -1;
    }
    time_osd_destroy(ctx->time_osd);
    ctx->time_osd = NULL;

    alogd("[Main] Stopping video encoder");
    if (ctx->video_encoder_started && video_encoder_stop(ctx->video_encoder) != 0) {
        result = -1;
    }
    video_encoder_destroy(ctx->video_encoder);
    ctx->video_encoder = NULL;

    /* 编码线程均停止后再让MUX写尾部索引，保证MP4能够被播放器定位。 */
    pthread_mutex_lock(&ctx->consumers.lock);
    ctx->consumers.recorder = NULL;
    pthread_mutex_unlock(&ctx->consumers.lock);
    if (ctx->mp4_recorder_started && mp4_recorder_stop(ctx->mp4_recorder) != 0) {
        result = -1;
    }
    mp4_recorder_destroy(ctx->mp4_recorder);
    ctx->mp4_recorder = NULL;

    if (ctx->rtsp_stream_started && rtsp_stream_stop(ctx->rtsp_stream) != 0) {
        result = -1;
    }
    rtsp_stream_destroy(ctx->rtsp_stream);
    ctx->rtsp_stream = NULL;
    ctx->consumers.rtsp = NULL; /* 编码线程均已停止。 */

    if (ctx->video_capture_started &&
        video_capture_stop(&ctx->video_capture) != 0) {
        result = -1;
    }

    ctx->video_capture.display = NULL;

    if (ctx->video_display_started && video_display_stop(ctx->video_display) != 0) {
        result = -1;
    }
    video_display_destroy(ctx->video_display);
    ctx->video_display = NULL;


    if (ctx->platform_initialized && platform_deinit() != 0) {
        result = -1;
    }
    ctx->platform_initialized = 0;
    ctx->stop_result = result;
    ctx->stop_complete = 1;
    return result;
}

void ip_camera_application_destroy(IpCameraContext *ctx)
{
    if (ctx == NULL) {
        return;
    }
    /* 遗漏stop时仍先join模块线程，再释放回调opaque和锁。 */
    (void)ip_camera_application_stop(ctx);
    pthread_mutex_destroy(&ctx->consumers.lock);
    free(ctx);
}
