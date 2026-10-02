#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "application.h"
#include "log.h"
#include "npu_self_test.h"

#include <utils/plat_log.h>

/* 信号处理只写标志；资源清理由主线程执行，不在信号中调用日志或MPP。 */
static volatile sig_atomic_t g_exit_signal;

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


static void main_loop(IpCameraRunMode mode)
{
    if (mode == IPC_CAMERA_AUDIO_ALARM_TEST) {
        alogd("[Main] Audio alarm self-test queued; check sound/logs, Ctrl+C to exit");
    } else {
        alogd("[Main] Application is running; press Ctrl+C to exit");
    }
    /* 子模块各自运行工作线程，入口线程只负责等待退出。 */
    while (g_exit_signal == 0) {
        sleep(1);
    }
    alogd("[Main] Exit signal received: %d", (int)g_exit_signal);
}

int main(int argc, char *argv[])
{
    int result = EXIT_FAILURE;
    int log_initialized = 0;
    IpCameraOptions options;
    IpCameraConfig config;
    IpCameraContext *context = NULL;

    /* 1. 准备信号和日志，随后任何失败都走同一个cleanup入口。 */
    g_exit_signal = 0;
    if (install_signal_handlers() != 0 || init_glog(argv) != 0) {
        goto cleanup;
    }
    log_initialized = 1;
    if (ip_camera_options_parse(argc, argv, &options) != 0) {
        aloge("[Main] Invalid arguments");
        aloge("Usage: %s [--npu-model MODEL.nb]", argv[0]);
        aloge("       %s --npu-self-test [MODEL.nb] [INPUT.nv12]", argv[0]);
        aloge("       %s --audio-alarm-test [ALARM.wav]", argv[0]);
        goto cleanup;
    }

    /* NPU单帧自检不创建应用服务，也不初始化MPP平台。 */
    if (options.mode == IPC_CAMERA_NPU_SELF_TEST) {
        alogd("[Main] Running Stage 9.1 NPU single-frame self-test");
        result = npu_self_test_run(options.model_path, options.input_path, 0.25f)
                     == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        goto cleanup;
    }

    alogd("======================================================");
    alogd("[Main] Starting IP Camera Application");
    alogd("[Main] Build date: %s %s", __DATE__, __TIME__);
    alogd("======================================================");

    /* 2. 默认参数集中生成；命令行只覆盖明确指定的路径。 */
    ip_camera_config_defaults(&config);
    config.npu.model_path = options.model_path;
    config.alarm.wav_path = options.alarm_path;
    context = ip_camera_application_create(&config);
    if (context == NULL) {
        aloge("[Main] Context allocation failed");
        goto cleanup;
    }

    /* 3. 应用层负责启动依赖和部分初始化状态，main不展开模块细节。 */
    if (ip_camera_application_start(context, options.mode) != 0) {
        aloge("[Main] Application initialization failed");
        goto cleanup;
    }

    /* 4. 正常退出才改为成功，清理失败仍会覆盖为失败。 */
    main_loop(options.mode);
    result = EXIT_SUCCESS;

cleanup:
    /* 5. 正常退出和中途失败，都先停服务再释放上下文和日志。 */
    if (ip_camera_application_stop(context) != 0) {
        result = EXIT_FAILURE;
    }
    ip_camera_application_destroy(context);
    if (log_initialized) {
        alogd("======================================================");
        alogd("[Main] Application exited with code: %d", result);
        alogd("======================================================");
        deinit_glog();
    }
    return result;
}
