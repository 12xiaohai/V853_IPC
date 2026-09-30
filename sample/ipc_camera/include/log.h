#ifndef IPC_CAMERA_LOG_H
#define IPC_CAMERA_LOG_H

int init_glog(char *argv[]); /* 创建日志目录并启动 glog。 */
void deinit_glog(void);      /* 刷新日志并关闭 glog。 */

#endif
