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

    /* MPP 系统必须早于 VI/VO/VENC 等任何子模块初始化。 */
    memset(&sys_conf, 0, sizeof(sys_conf));
    /* 媒体缓冲行按 32 字节对齐，满足硬件 DMA 的访存要求。 */
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

    /*
     * PTS（Presentation Timestamp）是媒体帧的时间轴。用当前 MPP 时间
     * 初始化全局 PTS 基准，后续音视频才能使用同一时钟同步。
     */
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
    /* 调用前应确保 VI/VO/VENC/RTSP 已全部停止。 */
    ERRORTYPE result = AW_MPI_SYS_Exit();

    if (result != SUCCESS) {
        aloge("AW_MPI_SYS_Exit failed: %#x", result);
        return -1;
    }

    return 0;
}
