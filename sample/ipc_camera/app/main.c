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
#include "platform.h"
#include "rtsp_stream.h"
#include "video_capture.h"
#include "video_display.h"
#include "video_encoder.h"

#include <utils/plat_log.h>

static pthread_mutex_t g_mutex_mpp;
static IpCameraContext *g_pContext;
/* sig_atomic_t 保证信号处理函数对该变量的读写不会被打断。 */
static volatile sig_atomic_t g_exit_signal;

/*
 * 这是 VENC 与 RTSP 之间的适配函数。opaque 实际指向 RtspStreamContext，
 * 将编码器的三段帧数据原样交给 RTSP 模块深拷贝。
 */
static int push_encoded_frame_to_rtsp(
    void *opaque,
    const unsigned char *header,
    size_t header_size,
    const unsigned char *data0,
    size_t size0,
    const unsigned char *data1,
    size_t size1,
    const unsigned char *data2,
    size_t size2,
    unsigned long long pts,
    int key_frame)
{
    return rtsp_stream_push_h264((RtspStreamContext *)opaque,
                                 header,
                                 header_size,
                                 data0,
                                 size0,
                                 data1,
                                 size1,
                                 data2,
                                 size2,
                                 pts,
                                 key_frame);
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
    int rtsp_stream_started = 0;
    RtspStreamContext *rtsp_stream = NULL;
    RtspStreamConfig rtsp_config;
    int audio_encoder_started = 0;
    AudioEncoderContext *audio_encoder = NULL;
    AudioEncoderConfig audio_config;

    (void)argc;

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
    encoder_config.frame_callback = push_encoded_frame_to_rtsp;
    encoder_config.frame_callback_opaque = rtsp_stream;

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
     * 阶段 6.2 把 AI 与 AENC 绑定。MPP 在内部传递 PCM，应用线程只取出
     * 带 ADTS 头的 AAC 码流并保存，先独立验证音频编码链路。
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
     * AI -> VENC -> RTSP -> VI -> VO/G2D -> MPP -> 上下文 -> 日志。
     * 先停 VENC 再停 RTSP，可保证销毁 RTSP 后不会再有新编码帧入队。
     */

    /* 音频最后启动，因此在逆序清理时最先停止。 */
    if (audio_encoder_started && audio_encoder_stop(audio_encoder) != 0) {
        ret = EXIT_FAILURE;
    }
    audio_encoder_destroy(audio_encoder);
    audio_encoder = NULL;

    if (video_encoder_started && video_encoder_stop(video_encoder) != 0) {
        ret = EXIT_FAILURE;
    }
    video_encoder_destroy(video_encoder);
    video_encoder = NULL;

    if (rtsp_stream_started && rtsp_stream_stop(rtsp_stream) != 0) {
        ret = EXIT_FAILURE;
    }
    rtsp_stream_destroy(rtsp_stream);
    rtsp_stream = NULL;

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
