#include "../include/platform.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <media/mm_comm_sys.h>
#include <media/mpi_sys.h>

int platform_init(AppContext *context)
{
    MPP_SYS_CONF_S sys_conf;
    uint64_t current_pts = 0;
    ERRORTYPE result;

    if (context == NULL || context->mpp_initialized) {
        return -1;
    }

    memset(&sys_conf, 0, sizeof(sys_conf));
    sys_conf.nAlignWidth = 32;

    result = AW_MPI_SYS_SetConf(&sys_conf);
    if (result != SUCCESS) {
        fprintf(stderr, "AW_MPI_SYS_SetConf failed: %#x\n", result);
        return -1;
    }

    result = AW_MPI_SYS_Init();
    if (result != SUCCESS) {
        fprintf(stderr, "AW_MPI_SYS_Init failed: %#x\n", result);
        return -1;
    }
    context->mpp_initialized = 1;

    result = AW_MPI_SYS_GetCurPts(&current_pts);
    if (result != SUCCESS) {
        fprintf(stderr, "AW_MPI_SYS_GetCurPts failed: %#x\n", result);
        goto error;
    }

    result = AW_MPI_SYS_InitPtsBase(current_pts);
    if (result != SUCCESS) {
        fprintf(stderr, "AW_MPI_SYS_InitPtsBase failed: %#x\n", result);
        goto error;
    }

    printf("MPP initialized, PTS base: %llu us\n",
           (unsigned long long)current_pts);
    return 0;

error:
    platform_deinit(context);
    return -1;
}

int platform_deinit(AppContext *context)
{
    ERRORTYPE result;

    if (context == NULL || !context->mpp_initialized) {
        return 0;
    }

    result = AW_MPI_SYS_Exit();
    if (result != SUCCESS) {
        fprintf(stderr, "AW_MPI_SYS_Exit failed: %#x\n", result);
        return -1;
    }

    context->mpp_initialized = 0;
    puts("MPP exited");
    return 0;
}

