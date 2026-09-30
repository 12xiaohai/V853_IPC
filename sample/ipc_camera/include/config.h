#ifndef IPC_CAMERA_CONFIG_H
#define IPC_CAMERA_CONFIG_H

#include "context.h"

/*
 * 创建/销毁整个应用的全局上下文。
 * construct 会同时填入一套可在 V853 开发板上运行的默认参数。
 */
IpCameraContext *constructIpCameraContext(void);
void destructIpCameraContext(IpCameraContext *context);

#endif
