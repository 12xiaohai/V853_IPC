#ifndef IPC_CAMERA_PLATFORM_H
#define IPC_CAMERA_PLATFORM_H

int platform_init(void);   /* 初始化 MPP 系统和全局 PTS 时钟。 */
int platform_deinit(void); /* 所有媒体模块停止后退出 MPP。 */

#endif
