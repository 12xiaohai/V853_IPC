#ifndef IPC_CAMERA_ISP_3DNR_DIAGNOSTICS_H
#define IPC_CAMERA_ISP_3DNR_DIAGNOSTICS_H

#include <media/mm_comm_vi.h>
#include <isp.h>

/* 仅由主线程访问；ISP0是多条VIPP共享的资源，不为每条通路重复修改。 */
typedef struct Isp3dnrDiagnostics {
    ISP_DEV device;
    /* 库复制配置时使用ARM字访问，字节结构的起始地址也须4字节对齐。 */
    _Alignas(4) struct isp_test_enable_cfg original;
    int restore_pending; /* Set失败也可能已部分修改，必须尝试恢复。 */
    int verified;
} Isp3dnrDiagnostics;

/* 状态必须先清零。调用时所有VIPP的ISP_Run已完成，ISP尚未Stop。 */
int isp_3dnr_diagnostics_disable(Isp3dnrDiagnostics *state, ISP_DEV device);
/* 只读核对配置，不能证明硬件寄存器/DDR访问已经关闭。 */
int isp_3dnr_diagnostics_check(const Isp3dnrDiagnostics *state);
/* 在任何依赖该ISP的模块Stop之前恢复；无修改时是安全的空操作。 */
int isp_3dnr_diagnostics_restore(Isp3dnrDiagnostics *state);

#endif
