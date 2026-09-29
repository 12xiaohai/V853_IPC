#include <mm_comm_sys.h>
#include <mm_common.h>
#include <mpi_sys.h>
#include <sys/time.h>
#include <tmessage.h>
#include <utils/plat_log.h>

/**
 * @brief 初始化多媒体处理平台
 * @return 0成功，-1失败
 */

int platform_init() {
  MPP_SYS_CONF_S stSysConf;
  memset(&stSysConf, 0, sizeof(MPP_SYS_CONF_S));
  stSysConf.nAlignWidth = 32;
  AW_MPI_SYS_SetConf(&stSysConf);
  int ret = AW_MPI_SYS_Init();
  if (ret < 0) {
    aloge("fatal error! sys Init failed! ret=%d", ret);
    return -1;
  }
  uint64_t pu64CurPts;
  AW_MPI_SYS_GetCurPts(pu64CurPts);
  AW_MPI_SYS_InitPtsBase(pu64CurPts);
  alogd("MPP platform Time:%d", pu64CurPts);
  return 0;
}
