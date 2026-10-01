#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_encoder.h"
#include "config.h"
#include "context.h"
#include "log.h"
#include "mp4_recorder.h"
#include "npu_self_test.h"
#include "platform.h"
#include "rtsp_stream.h"
#include "time_osd.h"
#include "video_capture.h"
#include "video_display.h"
#include "video_encoder.h"

#include <utils/plat_log.h>

static pthread_mutex_t g_mutex_mpp;
static IpCameraContext *g_pContext;
/* sig_atomic_t 保证信号处理函数对该变量的读写不会被打断。 */
static volatile sig_atomic_t g_exit_signal;

/*
 * 一帧编码数据可能同时被多个消费者使用。编码线程只调用一次适配回调，
 * 这里再把同一块只读缓冲分别交给RTSP和MP4；两个消费者都必须在返回前
 * 完成复制或同步消费，因为回调返回后编码缓冲就会归还MPP。
 */
typedef struct MediaConsumers {
    RtspStreamContext *rtsp;
    Mp4RecorderContext *recorder;
} MediaConsumers;

/*
 * 这是VENC与各码流消费者之间的适配函数。RTSP会深拷贝帧，MP4录像器
 * 则通过同步MUX接口在编码缓冲归还前完成消费。
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
    pthread_mutex_lock(&g_mutex_mpp);
    rtsp = consumers->rtsp;
    recorder = consumers->recorder;
    pthread_mutex_unlock(&g_mutex_mpp);

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
    pthread_mutex_lock(&g_mutex_mpp);
    rtsp = consumers->rtsp;
    recorder = consumers->recorder;
    pthread_mutex_unlock(&g_mutex_mpp);

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

static void handle_exit_signal(int signal_number)
{
    /*
     * 信号处理函数中只设置标志，不能调用 printf、free 或 MPP API。
     * 这些函数不是异步信号安全的，真正的清理由主线程完成。
     */
    g_exit_signal = signal_number;
}

/* 安装 Ctrl+C(SIGINT) 和系统结束(SIGTERM) 处理函数。 */
static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_exit_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) != 0) {
        perror("sigaction(SIGINT)");
        return -1;
    }

    if (sigaction(SIGTERM, &action, NULL) != 0) {
        perror("sigaction(SIGTERM)");
        return -1;
    }

    return 0;
}

static int initialize_context(IpCameraContext *context)
{
    if (context == NULL) {
        return -1;
    }

    context->initialized = 1;
    return 0;
}

int main(int argc, char *argv[])
{
    /*
     * ret 默认为失败。只有程序进入主循环并收到正常退出信号后，
     * 才改为 EXIT_SUCCESS。每个 started/initialized 标志决定 cleanup 能否操作对应资源。
     */
    int ret = EXIT_FAILURE;
    int mutex_initialized = 0;
    int log_initialized = 0;
    int platform_initialized = 0;
    int video_capture_started = 0;
    int video_display_started = 0;
    VideoDisplayContext *video_display = NULL;
    VideoDisplayConfig display_config;
    int video_encoder_started = 0;
    VideoEncoderContext *video_encoder = NULL;
    VideoEncoderConfig encoder_config;
    int time_osd_started = 0;
    TimeOsdContext *time_osd = NULL;
    TimeOsdConfig time_osd_config;
    int rtsp_stream_started = 0;
    RtspStreamContext *rtsp_stream = NULL;
    RtspStreamConfig rtsp_config;
    int audio_encoder_started = 0;
    AudioEncoderContext *audio_encoder = NULL;
    AudioEncoderConfig audio_config;
    int mp4_recorder_started = 0;
    Mp4RecorderContext *mp4_recorder = NULL;
    Mp4RecorderConfig mp4_config;
    MediaConsumers media_consumers;
    const unsigned char *h264_header = NULL;
    size_t h264_header_size = 0U;

    memset(&media_consumers, 0, sizeof(media_consumers));

    /* 保留原项目的全局 MPP 互斥量，供后续多模块协作扩展。 */
    if (pthread_mutex_init(&g_mutex_mpp, NULL) != 0) {
        return EXIT_FAILURE;
    }
    mutex_initialized = 1;

    if (install_signal_handlers() != 0) {
        goto cleanup;
    }

    /* 先启动日志，以便后续每一个初始化错误都能被记录。 */
    if (init_glog(argv) != 0) {
        goto cleanup;
    }
    log_initialized = 1;

    /*
     * 阶段9.1提供独立NPU自检模式，不启动摄像头、编码、RTSP和录像链路。
     * 用法：sample_strip --npu-self-test [model.nb] [input.nv12]
     */
    if (argc > 1 && strcmp(argv[1], "--npu-self-test") == 0) {
        const char *model_path = argc > 2 ? argv[2] : "/lib/yolov8n.nb";
        const char *input_path = argc > 3 ? argv[3] : NULL;

        alogd("[Main] Running Stage 9.1 NPU single-frame self-test");
        ret = npu_self_test_run(model_path, input_path, 0.25f) == 0
                  ? EXIT_SUCCESS
                  : EXIT_FAILURE;
        goto cleanup;
    }

    alogd("======================================================");
    alogd("[Main] Starting IP Camera Application");
    alogd("[Main] Build date: %s %s", __DATE__, __TIME__);
    alogd("======================================================");

    g_pContext = constructIpCameraContext();
    if (g_pContext == NULL) {
        aloge("[Main] Context allocation failed");
        goto cleanup;
    }

    if (initialize_context(g_pContext) != 0) {
        aloge("[Main] Context initialization failed");
        goto cleanup;
    }

    /* MPP 是所有媒体通路的公共基础，必须第一个启动。 */
    if (platform_init() != 0) {
        aloge("[Main] Platform initialization failed");
        goto cleanup;
    }
    platform_initialized = 1;

    /*
     * 摄像头输出为 1920x1080 横屏，LCD 为 480x800 竖屏。
     * 先用 G2D 旋转 270 度得到 1080x1920，再由 VO 缩放到 LCD 矩形。
     */
    memset(&display_config, 0, sizeof(display_config));
    display_config.source_width = g_pContext->video_capture.width;
    display_config.source_height = g_pContext->video_capture.height;
    display_config.pixel_format = g_pContext->video_capture.pixel_format;
    display_config.rotation = 270;
    display_config.display_x = 0;
    display_config.display_y = 0;
    display_config.display_width = 480;
    display_config.display_height = 800;

    video_display = video_display_create(&display_config);
    if (video_display == NULL) {
        aloge("[Main] Video display context allocation failed");
        goto cleanup;
    }

    if (video_display_start(video_display) != 0) {
        aloge("[Main] Video display initialization failed");
        goto cleanup;
    }
    video_display_started = 1;
    g_pContext->video_capture.display = video_display;

    /* 显示模块先于 VI 启动，避免采集到帧时还没有可用的显示目标。 */
    if (video_capture_start(&g_pContext->video_capture) != 0) {
        aloge("[Main] Video capture initialization failed");
        goto cleanup;
    }
    video_capture_started = 1;

    /*
     * RTSP 在 VENC 前启动，这样第一个编码关键帧就能立即入队。
     * session 0 对应 /ch0；16 帧约为 0.8 秒的 20 fps 视频。
     */
    memset(&rtsp_config, 0, sizeof(rtsp_config));
    rtsp_config.session_id = 0;
    rtsp_config.net_type = RTSP_NET_TYPE_WLAN0;
    rtsp_config.frame_rate = g_pContext->video_capture.frame_rate;
    rtsp_config.queue_capacity = 16;
    rtsp_stream = rtsp_stream_create(&rtsp_config);
    if (rtsp_stream == NULL) {
        aloge("[Main] RTSP context allocation failed");
        goto cleanup;
    }
    if (rtsp_stream_start(rtsp_stream) != 0) {
        aloge("[Main] RTSP video service initialization failed");
        goto cleanup;
    }
    rtsp_stream_started = 1;
    media_consumers.rtsp = rtsp_stream; /* VENC线程尚未创建，无并发访问。 */

    /*
     * 编码使用独立 VIPP 0，与预览 VIPP 4 并行。VI -> VENC 由 MPP Bind
     * 在内部传帧，应用只从 VENC 取压缩后的 H.264 数据。
     */
    memset(&encoder_config, 0, sizeof(encoder_config));
    encoder_config.channel = 0;
    encoder_config.vi_device = 0;
    encoder_config.isp_device = 0;
    encoder_config.vi_channel = 0;
    encoder_config.width = g_pContext->video_capture.width;
    encoder_config.height = g_pContext->video_capture.height;
    encoder_config.frame_rate = g_pContext->video_capture.frame_rate;
    encoder_config.bit_rate = 5242880;
    encoder_config.gop_size = 75;
    encoder_config.pixel_format = g_pContext->video_capture.pixel_format;
    encoder_config.output_path = "/mnt/UDISK/sample_demo.h264";
    encoder_config.frame_callback = dispatch_encoded_video;
    encoder_config.frame_callback_opaque = &media_consumers;

    video_encoder = video_encoder_create(&encoder_config);
    if (video_encoder == NULL) {
        aloge("[Main] Video encoder context allocation failed");
        goto cleanup;
    }

    if (video_encoder_start(video_encoder) != 0) {
        aloge("[Main] Video encoder initialization failed");
        goto cleanup;
    }
    video_encoder_started = 1;

    /*
     * RGN附着在VENC通道0，所以本地H.264文件和RTSP视频都会包含时间。
     * LCD预览来自另一条VI/G2D/VO通路，本阶段不会在LCD上重复叠加。
     */
    memset(&time_osd_config, 0, sizeof(time_osd_config));
    time_osd_config.venc_channel = encoder_config.channel;
    time_osd_config.handle = 0;
    time_osd_config.x = 32;
    time_osd_config.y = 32;
    time_osd_config.update_seconds = 1;
    time_osd = time_osd_create(&time_osd_config);
    if (time_osd == NULL) {
        aloge("[Main] Time OSD context allocation failed");
        goto cleanup;
    }
    if (time_osd_start(time_osd) != 0) {
        aloge("[Main] Time OSD initialization failed");
        goto cleanup;
    }
    time_osd_started = 1;

    /*
     * 阶段8.2沿用阶段8.1的MPP MUX链路，并启用按时间命名的60秒分段录像。
     * 录像、RTSP和裸流文件仍共享同一批带原始PTS的编码结果，不重复编码。
     */
    if (video_encoder_get_h264_header(video_encoder,
                                      &h264_header,
                                      &h264_header_size) != 0) {
        aloge("[Main] Get H.264 SPS/PPS for MP4 failed");
        goto cleanup;
    }
    memset(&mp4_config, 0, sizeof(mp4_config));
    mp4_config.mux_channel = 0;
    mp4_config.venc_channel = encoder_config.channel;
    mp4_config.width = encoder_config.width;
    mp4_config.height = encoder_config.height;
    mp4_config.frame_rate = encoder_config.frame_rate;
    mp4_config.gop_size = encoder_config.gop_size;
    mp4_config.sample_rate = 16000;
    mp4_config.audio_channels = 1;
    mp4_config.audio_bit_width = 16;
    mp4_config.samples_per_frame = 1024;
    mp4_config.output_path = NULL; /* 分段模式下由录像器动态生成完整路径。 */
    mp4_config.output_directory = "/mnt/UDISK";
    mp4_config.file_prefix = "record";
    mp4_config.segment_duration_seconds = 60;
    /* 阶段8.3最多保留10段录像，并尽量保证UDISK至少剩余512 MiB。 */
    mp4_config.max_segment_files = 10;
    mp4_config.min_free_space_mb = 512;
    mp4_config.h264_header = h264_header;
    mp4_config.h264_header_size = h264_header_size;

    mp4_recorder = mp4_recorder_create(&mp4_config);
    if (mp4_recorder == NULL) {
        aloge("[Main] MP4 recorder context allocation failed");
        goto cleanup;
    }
    if (mp4_recorder_start(mp4_recorder) != 0) {
        aloge("[Main] MP4 recorder initialization failed");
        goto cleanup;
    }
    mp4_recorder_started = 1;
    pthread_mutex_lock(&g_mutex_mpp);
    media_consumers.recorder = mp4_recorder;
    pthread_mutex_unlock(&g_mutex_mpp);
    /* 避免录像器等待完整GOP，接入后立即请求一个新的MP4起始关键帧。 */
    if (video_encoder_request_key_frame(video_encoder) != 0) {
        alogw("[Main] Request key frame for MP4 failed; waiting for next GOP");
    }

    /*
     * AI 与 AENC 由 MPP 绑定，应用取出带 ADTS 头的 AAC。阶段 6.3 在
     * 保留本地 AAC 文件的同时，通过回调把同一帧送入 RTSP 音频队列。
     */
    memset(&audio_config, 0, sizeof(audio_config));
    audio_config.ai_device = 0;
    audio_config.ai_channel = 0;
    audio_config.aenc_channel = 0;
    audio_config.sample_rate = 16000;
    audio_config.bit_width = 16;
    audio_config.channels = 1;
    audio_config.samples_per_frame = 1024;
    audio_config.volume = 100;
    audio_config.bit_rate = 0;
    audio_config.timeout_ms = 200;
    audio_config.output_path = "/mnt/UDISK/sample_demo.aac";
    /* RTSP 未启动时保持回调为空，本地 AAC 文件仍可独立生成。 */
    if (rtsp_stream_started) {
        audio_config.frame_callback = dispatch_encoded_audio;
        audio_config.frame_callback_opaque = &media_consumers;
    }

    audio_encoder = audio_encoder_create(&audio_config);
    if (audio_encoder == NULL) {
        aloge("[Main] Audio encoder context allocation failed");
        goto cleanup;
    }
    if (audio_encoder_start(audio_encoder) != 0) {
        aloge("[Main] AAC audio encoder initialization failed");
        goto cleanup;
    }
    audio_encoder_started = 1;

    /* 主线程不做媒体处理，只等待信号；实际工作由各子线程完成。 */
    alogd("[Main] Application is running; press Ctrl+C to exit");
    while (g_exit_signal == 0) {
        sleep(1);
    }
    alogd("[Main] Exit signal received: %d", (int)g_exit_signal);

    ret = EXIT_SUCCESS;

cleanup:
    /*
     * 统一失败回滚和正常退出入口。按启动顺序的反向销毁：
     * AI -> OSD -> VENC -> RTSP -> VI -> VO/G2D -> MPP -> 上下文 -> 日志。
     * 先停 VENC 再停 RTSP，可保证销毁 RTSP 后不会再有新编码帧入队。
     */

    /* 音频最后启动，因此在逆序清理时最先停止。 */
    if (audio_encoder_started && audio_encoder_stop(audio_encoder) != 0) {
        ret = EXIT_FAILURE;
    }
    audio_encoder_destroy(audio_encoder);
    audio_encoder = NULL;

    /* 必须先从VENC解绑并销毁RGN，之后才能销毁VENC通道本身。 */
    if (time_osd_started && time_osd_stop(time_osd) != 0) {
        ret = EXIT_FAILURE;
    }
    time_osd_destroy(time_osd);
    time_osd = NULL;

    if (video_encoder_started && video_encoder_stop(video_encoder) != 0) {
        ret = EXIT_FAILURE;
    }
    video_encoder_destroy(video_encoder);
    video_encoder = NULL;

    /* 编码线程均停止后再让MUX写尾部索引，保证MP4能够被播放器定位。 */
    pthread_mutex_lock(&g_mutex_mpp);
    media_consumers.recorder = NULL;
    pthread_mutex_unlock(&g_mutex_mpp);
    if (mp4_recorder_started && mp4_recorder_stop(mp4_recorder) != 0) {
        ret = EXIT_FAILURE;
    }
    mp4_recorder_destroy(mp4_recorder);
    mp4_recorder = NULL;

    if (rtsp_stream_started && rtsp_stream_stop(rtsp_stream) != 0) {
        ret = EXIT_FAILURE;
    }
    rtsp_stream_destroy(rtsp_stream);
    rtsp_stream = NULL;
    media_consumers.rtsp = NULL; /* 编码线程均已停止。 */

    if (video_capture_started &&
        video_capture_stop(&g_pContext->video_capture) != 0) {
        ret = EXIT_FAILURE;
    }

    if (g_pContext != NULL) {
        g_pContext->video_capture.display = NULL;
    }

    if (video_display_started && video_display_stop(video_display) != 0) {
        ret = EXIT_FAILURE;
    }
    video_display_destroy(video_display);
    video_display = NULL;

    if (platform_initialized && platform_deinit() != 0) {
        ret = EXIT_FAILURE;
    }

    destructIpCameraContext(g_pContext);
    g_pContext = NULL;

    if (log_initialized) {
        alogd("======================================================");
        alogd("[Main] Application exited with code: %d", ret);
        alogd("======================================================");
        deinit_glog();
    }

    if (mutex_initialized) {
        pthread_mutex_destroy(&g_mutex_mpp);
    }

    return ret;
}
