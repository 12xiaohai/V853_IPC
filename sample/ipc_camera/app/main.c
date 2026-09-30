#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "context.h"
#include "log.h"
#include "platform.h"
#include "video_capture.h"
#include "video_display.h"
#include "video_encoder.h"

#include <utils/plat_log.h>

static pthread_mutex_t g_mutex_mpp;
static IpCameraContext *g_pContext;
static volatile sig_atomic_t g_exit_signal;

static void handle_exit_signal(int signal_number)
{
    g_exit_signal = signal_number;
}

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

    (void)argc;

    if (pthread_mutex_init(&g_mutex_mpp, NULL) != 0) {
        return EXIT_FAILURE;
    }
    mutex_initialized = 1;

    if (install_signal_handlers() != 0) {
        goto cleanup;
    }

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

    if (platform_init() != 0) {
        aloge("[Main] Platform initialization failed");
        goto cleanup;
    }
    platform_initialized = 1;

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

    if (video_capture_start(&g_pContext->video_capture) != 0) {
        aloge("[Main] Video capture initialization failed");
        goto cleanup;
    }
    video_capture_started = 1;

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

    alogd("[Main] Application is running; press Ctrl+C to exit");
    while (g_exit_signal == 0) {
        sleep(1);
    }
    alogd("[Main] Exit signal received: %d", (int)g_exit_signal);

    ret = EXIT_SUCCESS;

cleanup:
    if (video_encoder_started && video_encoder_stop(video_encoder) != 0) {
        ret = EXIT_FAILURE;
    }
    video_encoder_destroy(video_encoder);
    video_encoder = NULL;

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
