#include <context.h>
#include <utils/plat_log.h>

IpCameraContext *constructIpCameraContext() {
  int ret;
  IpCameraContext *pContext =
      (IpCameraContext *)malloc(sizeof(IpCameraContext));
  if (NULL == pContext) {
    aloge("fatal error! malloc fail!");
    return NULL;
  }
  return pContext;
}