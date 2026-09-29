#include "config.h"

#include <stdlib.h>

#include <utils/plat_log.h>

IpCameraContext *constructIpCameraContext(void)
{
    IpCameraContext *context = calloc(1, sizeof(*context));

    if (context == NULL) {
        aloge("fatal error! allocate IP camera context failed");
        return NULL;
    }

    return context;
}

void destructIpCameraContext(IpCameraContext *context)
{
    free(context);
}
