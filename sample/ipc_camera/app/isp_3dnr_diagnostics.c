#include "isp_3dnr_diagnostics.h"

#include <stddef.h>
#include <string.h>
#include <utils/plat_log.h>

/*
 * 当前libaw_mpp的ModuleOnOff包装复制33字节，但公共结构体是32位字段。
 * 不强转指针、不改SDK头文件：使用libISP声明的原生字节结构及配置接口。
 * 库反汇编确认TEST/ENABLE组传输33字节；ABI变化应在编译/读回时拒绝。
 */
_Static_assert(sizeof(struct isp_test_enable_cfg) == 33, "ISP enable ABI size");
_Static_assert(offsetof(struct isp_test_enable_cfg, manual) == 0, "ISP manual ABI");
_Static_assert(offsetof(struct isp_test_enable_cfg, tdf) == 13, "ISP tdf ABI");

static int read_module_switches(ISP_DEV device, struct isp_test_enable_cfg *value)
{
    int ret = isp_get_cfg(device, HW_ISP_CFG_TEST, HW_ISP_CFG_TEST_ENABLE, value);
    /* 原生Get/Set返回传输字节数，不是MPI的SUCCESS=0。 */
    if (ret != (int)sizeof(*value)) {
        aloge("[ISP3DNR-DIAG] isp_get_cfg failed/ABI mismatch: "
              "isp=%d ret=%d expected_bytes=%u", device, ret,
              (unsigned int)sizeof(*value));
        return -1;
    }
    return 0;
}

static int write_module_switches(ISP_DEV device, struct isp_test_enable_cfg *value)
{
    int ret = isp_set_cfg(device, HW_ISP_CFG_TEST, HW_ISP_CFG_TEST_ENABLE, value);
    if (ret != (int)sizeof(*value)) {
        aloge("[ISP3DNR-DIAG] isp_set_cfg failed/ABI mismatch: "
              "isp=%d ret=%d expected_bytes=%u", device, ret,
              (unsigned int)sizeof(*value));
        return -1;
    }
    /* 写入参数后还要通知ISP更新；不能只改内存配置便宣称生效。 */
    ret = isp_update(device);
    if (ret != 0) {
        aloge("[ISP3DNR-DIAG] isp_update failed: isp=%d ret=%d", device, ret);
        return -1;
    }
    return 0;
}

int isp_3dnr_diagnostics_check(const Isp3dnrDiagnostics *state)
{
    _Alignas(4) struct isp_test_enable_cfg current;
    int ret;
    if (state == NULL || !state->restore_pending) {
        return -1;
    }
    memset(&current, 0, sizeof(current));
    ret = read_module_switches(state->device, &current);
    if (ret != SUCCESS) {
        aloge("[ISP3DNR-DIAG] Readback failed: isp=%d ret=%d",
              state->device, ret);
        return -1;
    }
    if (current.manual != 1 || current.tdf != 0) {
        aloge("[ISP3DNR-DIAG] Configuration mismatch: isp=%d manual=%d "
              "tdf=%d; expected manual=1 tdf=0, test INVALID",
              state->device, current.manual, current.tdf);
        return -1;
    }
    return 0;
}

int isp_3dnr_diagnostics_disable(Isp3dnrDiagnostics *state, ISP_DEV device)
{
    _Alignas(4) struct isp_test_enable_cfg requested;
    int ret;
    if (state == NULL || state->restore_pending) {
        return -1; /* 不允许覆盖尚未恢复的原始快照。 */
    }
    state->device = device;
    state->verified = 0;
    memset(&state->original, 0, sizeof(state->original));
    ret = read_module_switches(device, &state->original);
    if (ret != SUCCESS) {
        aloge("[ISP3DNR-DIAG] Read original configuration failed: "
              "isp=%d ret=%d; no configuration written", device, ret);
        return -1;
    }
    alogd("[ISP3DNR-DIAG] Original: isp=%d manual=%d tdf=%d",
          device, state->original.manual, state->original.tdf);
    if (state->original.tdf == 0) {
        alogw("[ISP3DNR-DIAG] Original tdf already 0; do not assume "
              "this run compares enabled hardware 3DNR against disabled 3DNR");
    }
    /*
     * SDK的manual是模块开关控制使能。完整复制Get结果，避免把AE/AWB/Gamma
     * 等开关清零。manual=1会固定这些读取到的开关，须在测试记录中注明。
     * 这是ISP时域降噪tdf，不是VENC参考帧LBC或Set3NRAttr降噪强度。
     */
    requested = state->original;
    requested.manual = 1;
    requested.tdf = 0;
    state->restore_pending = 1;
    ret = write_module_switches(device, &requested);
    if (ret != SUCCESS) {
        aloge("[ISP3DNR-DIAG] Set failed: isp=%d ret=%d; "
              "test INVALID, restoration required", device, ret);
        return -1;
    }
    if (isp_3dnr_diagnostics_check(state) != 0) {
        return -1;
    }
    state->verified = 1;
    alogd("[ISP3DNR-DIAG] Configuration verified: isp=%d manual=1 tdf=0; "
          "shared ISP, hardware activity not independently verified");
    return 0;
}

int isp_3dnr_diagnostics_restore(Isp3dnrDiagnostics *state)
{
    _Alignas(4) struct isp_test_enable_cfg restored, readback;
    int ret;
    if (state == NULL) {
        return -1;
    }
    if (!state->restore_pending) {
        return 0;
    }
    /*
     * 只恢复我们修改的manual/tdf，保留运行中其他模块开关的当前值。
     * 若当前配置无法读取，退回保存的完整原配置并明确记录该回滚策略。
     */
    memset(&restored, 0, sizeof(restored));
    ret = read_module_switches(state->device, &restored);
    if (ret != SUCCESS) {
        alogw("[ISP3DNR-DIAG] Restore read failed: ret=%d; "
              "using complete original snapshot", ret);
        restored = state->original;
    }
    restored.manual = state->original.manual;
    restored.tdf = state->original.tdf;
    state->verified = 0;
    ret = write_module_switches(state->device, &restored);
    if (ret != SUCCESS) {
        aloge("[ISP3DNR-DIAG] RESTORE FAILED: isp=%d ret=%d; "
              "original state not confirmed", state->device, ret);
        return -1;
    }
    memset(&readback, 0, sizeof(readback));
    ret = read_module_switches(state->device, &readback);
    if (ret != SUCCESS || readback.manual != restored.manual ||
        readback.tdf != restored.tdf) {
        aloge("[ISP3DNR-DIAG] RESTORE readback failed: isp=%d ret=%d "
              "manual=%d tdf=%d expected=%d/%d",
              state->device, ret, readback.manual, readback.tdf,
              restored.manual, restored.tdf);
        return -1;
    }
    state->restore_pending = 0;
    alogd("[ISP3DNR-DIAG] Restored: isp=%d manual=%d tdf=%d",
          state->device, restored.manual, restored.tdf);
    return 0;
}
