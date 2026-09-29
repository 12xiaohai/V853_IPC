#ifndef IPC_CAMERA_CONFIG_H
#define IPC_CAMERA_CONFIG_H

#include "context.h"

IpCameraContext *constructIpCameraContext(void);
void destructIpCameraContext(IpCameraContext *context);

#endif
