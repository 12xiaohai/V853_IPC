#include "platform.h"

#include <stdint.h>
#include <string.h>

#include <media/mpi_sys.h>
#include <utils/plat_log.h>

int platform_init(void)
{
    ERRORTYPE result;
    MPP_SYS_CONF_S sys_conf;
    uint64_t current_pts = 0;

    memset(&sys_conf, 0, sizeof(sys_conf));
    sys_conf.nAlignWidth = 32;

    result = AW_MPI_SYS_SetConf(&sys_conf);
    if (result != SUCCESS) {
        aloge("AW_MPI_SYS_SetConf failed: %#x", result);
        return -1;
    }

    result = AW_MPI_SYS_Init();
    if (result != SUCCESS) {
        aloge("AW_MPI_SYS_Init failed: %#x", result);
        return -1;
    }

    result = AW_MPI_SYS_GetCurPts(&current_pts);
    if (result != SUCCESS) {
        aloge("AW_MPI_SYS_GetCurPts failed: %#x", result);
        AW_MPI_SYS_Exit();
        return -1;
    }

    result = AW_MPI_SYS_InitPtsBase(current_pts);
    if (result != SUCCESS) {
        aloge("AW_MPI_SYS_InitPtsBase failed: %#x", result);
        AW_MPI_SYS_Exit();
        return -1;
    }

    alogd("MPP platform time: %llu us",
          (unsigned long long)current_pts);
    return 0;
}

int platform_deinit(void)
{
    ERRORTYPE result = AW_MPI_SYS_Exit();

    if (result != SUCCESS) {
        aloge("AW_MPI_SYS_Exit failed: %#x", result);
        return -1;
    }

    return 0;
}
