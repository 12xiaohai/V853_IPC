
#include <pthread.h>
#include <log.h>

#include <stdlib.h>
#include <context.h>
#include <utils/plat_log.h>
#include <config.h>
#include <platform.h>
static pthread_mutex_t g_mutex_mpp;
static IpCameraContext *g_pContext = NULL;

static int initialize_context(IpCameraContext *ctx) { return 0; }

int main(int argc, char *argv[]) {
  int ret = 0;
  // 初始化互斥锁
  pthread_mutex_init(&g_mutex_mpp, NULL);
  // 初始化日志系统
  init_glog(argv);
  alogd("======================================================");
  alogd("[Main] Starting IP Camera Application");
  alogd("[Main] Build date: %s %s", __DATE__, __TIME__);
  alogd("======================================================");
  // 创建上下文
  g_pContext = constructIpCameraContext();
  if (!g_pContext) {
    aloge("[Main] Context allocation failed!");
    ret = -1;
    goto cleanup;
  }

  // 初始化上下文
  if ((ret = initialize_context(g_pContext)) != 0) {
    aloge("[Main] Context initialization failed");
    goto cleanup;
  }

  // 平台初始化
  if ((ret = platform_init()) != 0) {
    aloge("[Main] Platform initialization failed: %d", ret);
    goto cleanup;
  }

cleanup:
  // 停止服务

  // 释放资源

  // 释放上下文
  if (g_pContext) {
    free(g_pContext);
    g_pContext = NULL;
  }

  // 平台反初始化

  // 销毁互斥锁
  pthread_mutex_destroy(&g_mutex_mpp);

  alogd("======================================================");
  alogd("[Main] Application exited with code: %d", ret);
  alogd("======================================================");

  return ret;
}
