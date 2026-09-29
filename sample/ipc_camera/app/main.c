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

    if (video_capture_start(&g_pContext->video_capture) != 0) {
        aloge("[Main] Video capture initialization failed");
        goto cleanup;
    }
    video_capture_started = 1;

    alogd("[Main] Application is running; press Ctrl+C to exit");
    while (g_exit_signal == 0) {
        sleep(1);
    }
    alogd("[Main] Exit signal received: %d", (int)g_exit_signal);

    ret = EXIT_SUCCESS;

cleanup:
    if (video_capture_started &&
        video_capture_stop(&g_pContext->video_capture) != 0) {
        ret = EXIT_FAILURE;
    }

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
