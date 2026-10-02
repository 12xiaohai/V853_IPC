#ifndef IPC_CAMERA_APPLICATION_H
#define IPC_CAMERA_APPLICATION_H

#include "config.h"
#include "context.h"

/*
 * 不透明应用上下文：main不直接访问模块指针和started标志。
 * 生命周期由主线程调用，各功能模块仍自行管理工作线程。
 * create复制配置结构，但路径字符串为借用引用，必须存活到destroy。
 */
IpCameraContext *ip_camera_application_create(const IpCameraConfig *config);
/* 支持监控/声音自检；NPU单帧自检无需MPP，由main独立执行。 */
int ip_camera_application_start(IpCameraContext *context, IpCameraRunMode mode);
/* 可处理部分启动和重复调用；停止失败返回-1，并继续尝试后续清理。 */
int ip_camera_application_stop(IpCameraContext *context);
void ip_camera_application_destroy(IpCameraContext *context);

#endif
