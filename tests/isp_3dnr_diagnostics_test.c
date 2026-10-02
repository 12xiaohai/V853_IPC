#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 包含真实控制器实现；替身不接触板端ISP，不验证硬件实际开关状态。 */
#include "../sample/ipc_camera/app/isp_3dnr_diagnostics.c"

static struct isp_test_enable_cfg configured;
static int get_count, set_count, fail_get_at, fail_set_at;
static int ignore_set_at, partial_set_on_failure;
static int update_count, fail_update_at, short_get_at, short_set_at;

void vipp4_test_log(const char *format, ...)
{
    (void)format;
}

int isp_get_cfg(int device, unsigned char group, unsigned int id, void *data)
{
    struct isp_test_enable_cfg *value = data;
    assert(device == 0);
    assert(group == HW_ISP_CFG_TEST && id == HW_ISP_CFG_TEST_ENABLE);
    assert((uintptr_t)data % 4 == 0);
    if (++get_count == fail_get_at) {
        return -1;
    }
    *value = configured;
    return get_count == short_get_at ? 32 : (int)sizeof(*value);
}

int isp_set_cfg(int device, unsigned char group, unsigned int id, void *data)
{
    struct isp_test_enable_cfg *value = data;
    assert(device == 0);
    assert(group == HW_ISP_CFG_TEST && id == HW_ISP_CFG_TEST_ENABLE);
    assert((uintptr_t)data % 4 == 0);
    ++set_count;
    if (set_count == fail_set_at) {
        if (partial_set_on_failure) {
            configured = *value; /* 模拟返回失败但已经修改了配置。 */
        }
        return -1;
    }
    if (set_count != ignore_set_at) {
        configured = *value;
    }
    return set_count == short_set_at ? 32 : (int)sizeof(*value);
}

int isp_update(int device)
{
    assert(device == 0);
    return ++update_count == fail_update_at ? -1 : 0;
}

static Isp3dnrDiagnostics fresh_state(int manual, int tdf)
{
    Isp3dnrDiagnostics state = {0};
    configured = (struct isp_test_enable_cfg){
        .manual = (signed char)manual, .tdf = (signed char)tdf,
        .ae = 11, .awb = 22, .gamma = 33, .denoise = 44, .wdr_split = 55,
        .enc_3dnr_en = 66, .enc_2dnr_en = 77
    };
    get_count = set_count = fail_get_at = fail_set_at = 0;
    ignore_set_at = partial_set_on_failure = 0;
    update_count = fail_update_at = short_get_at = short_set_at = 0;
    return state;
}

static void assert_other_switches_preserved(void)
{
    assert(configured.ae == 11 && configured.awb == 22);
    assert(configured.gamma == 33 && configured.denoise == 44 &&
           configured.wdr_split == 55 && configured.enc_3dnr_en == 66 &&
           configured.enc_2dnr_en == 77);
}

int main(void)
{
    Isp3dnrDiagnostics state = fresh_state(0, 1);
    /* 默认运行不读写ISP；重复cleanup同样不读写。 */
    assert(isp_3dnr_diagnostics_restore(&state) == 0);
    assert(isp_3dnr_diagnostics_check(&state) == -1);
    assert(get_count == 0 && set_count == 0);
    assert(isp_3dnr_diagnostics_disable(NULL, 0) == -1);
    assert(isp_3dnr_diagnostics_check(NULL) == -1);
    assert(isp_3dnr_diagnostics_restore(NULL) == -1);

    /* 自动/手动、原tdf开/关组合都必须完整保留并恢复。 */
    for (int manual = 0; manual <= 1; ++manual) {
        for (int tdf = 0; tdf <= 1; ++tdf) {
            state = fresh_state(manual, tdf);
            assert(isp_3dnr_diagnostics_disable(&state, 0) == 0);
            assert(state.restore_pending && state.verified);
            assert(update_count == 1);
            assert(state.original.manual == manual && state.original.tdf == tdf);
            assert(configured.manual == 1 && configured.tdf == 0);
            struct isp_test_enable_cfg expected = state.original;
            expected.manual = 1;
            expected.tdf = 0;
            assert(memcmp(&configured, &expected, sizeof(expected)) == 0);
            assert_other_switches_preserved();
            assert(isp_3dnr_diagnostics_check(&state) == 0);
            /* 不可用第二次disable覆盖原配置。 */
            int before = set_count;
            assert(isp_3dnr_diagnostics_disable(&state, 0) == -1);
            assert(set_count == before);
            assert(isp_3dnr_diagnostics_restore(&state) == 0);
            assert(!state.restore_pending && !state.verified);
            assert(update_count == 2);
            assert(configured.manual == manual && configured.tdf == tdf);
            assert_other_switches_preserved();
            before = get_count;
            assert(isp_3dnr_diagnostics_restore(&state) == 0);
            assert(get_count == before);
        }
    }

    state = fresh_state(0, 1);
    fail_get_at = 1;
    assert(isp_3dnr_diagnostics_disable(&state, 0) == -1);
    assert(!state.restore_pending && set_count == 0);
    assert(isp_3dnr_diagnostics_restore(&state) == 0);
    assert(set_count == 0);

    state = fresh_state(0, 1);
    fail_set_at = 1;
    partial_set_on_failure = 1;
    assert(isp_3dnr_diagnostics_disable(&state, 0) == -1);
    assert(state.restore_pending && !state.verified);
    assert(configured.manual == 1 && configured.tdf == 0);
    assert(isp_3dnr_diagnostics_restore(&state) == 0);
    assert(configured.manual == 0 && configured.tdf == 1);

    state = fresh_state(0, 1);
    ignore_set_at = 1; /* Set报成功但没有生效，不能把C组标为有效。 */
    assert(isp_3dnr_diagnostics_disable(&state, 0) == -1);
    assert(!state.verified && state.restore_pending);
    assert(isp_3dnr_diagnostics_restore(&state) == 0);

    state = fresh_state(0, 1);
    fail_get_at = 2; /* 写入后读回失败，仍需恢复。 */
    assert(isp_3dnr_diagnostics_disable(&state, 0) == -1);
    assert(isp_3dnr_diagnostics_restore(&state) == 0);

    state = fresh_state(0, 1);
    assert(isp_3dnr_diagnostics_disable(&state, 0) == 0);
    configured.tdf = 1; /* 模拟运行中参数被自动调节/其他调用覆盖。 */
    assert(isp_3dnr_diagnostics_check(&state) == -1);
    configured.tdf = 0;
    configured.manual = 0;
    assert(isp_3dnr_diagnostics_check(&state) == -1);
    fail_get_at = get_count + 1;
    assert(isp_3dnr_diagnostics_check(&state) == -1);
    assert(isp_3dnr_diagnostics_restore(&state) == 0);

    state = fresh_state(0, 1);
    assert(isp_3dnr_diagnostics_disable(&state, 0) == 0);
    configured.awb = 99; /* 恢复不能覆盖其他模块在运行中做的修改。 */
    assert(isp_3dnr_diagnostics_restore(&state) == 0);
    assert(configured.awb == 99 && configured.manual == 0 && configured.tdf == 1);

    state = fresh_state(0, 1);
    assert(isp_3dnr_diagnostics_disable(&state, 0) == 0);
    fail_get_at = get_count + 1; /* 恢复Get失败，使用原始全配置快照。 */
    assert(isp_3dnr_diagnostics_restore(&state) == 0);
    assert_other_switches_preserved();

    state = fresh_state(0, 1);
    assert(isp_3dnr_diagnostics_disable(&state, 0) == 0);
    fail_set_at = set_count + 1;
    assert(isp_3dnr_diagnostics_restore(&state) == -1);
    assert(state.restore_pending && !state.verified);
    assert(isp_3dnr_diagnostics_restore(&state) == 0); /* 控制器允许显式重试。 */

    state = fresh_state(0, 1);
    assert(isp_3dnr_diagnostics_disable(&state, 0) == 0);
    ignore_set_at = set_count + 1;
    assert(isp_3dnr_diagnostics_restore(&state) == -1);
    assert(state.restore_pending);
    assert(isp_3dnr_diagnostics_restore(&state) == 0);

    state = fresh_state(0, 1);
    assert(isp_3dnr_diagnostics_disable(&state, 0) == 0);
    fail_get_at = get_count + 2; /* 恢复Set成功，但恢复读回失败。 */
    assert(isp_3dnr_diagnostics_restore(&state) == -1);
    assert(state.restore_pending);
    assert(isp_3dnr_diagnostics_restore(&state) == 0);
    state = fresh_state(0, 1);
    short_get_at = 1;
    assert(isp_3dnr_diagnostics_disable(&state, 0) == -1);
    assert(set_count == 0 && !state.restore_pending);

    state = fresh_state(0, 1);
    short_set_at = 1;
    assert(isp_3dnr_diagnostics_disable(&state, 0) == -1);
    assert(update_count == 0 && state.restore_pending);
    assert(isp_3dnr_diagnostics_restore(&state) == 0);

    state = fresh_state(0, 1);
    fail_update_at = 1;
    assert(isp_3dnr_diagnostics_disable(&state, 0) == -1);
    assert(!state.verified && state.restore_pending);
    assert(isp_3dnr_diagnostics_restore(&state) == 0);
    assert(configured.manual == 0 && configured.tdf == 1);

    state = fresh_state(0, 1);
    assert(isp_3dnr_diagnostics_disable(&state, 0) == 0);
    fail_update_at = 2;
    assert(isp_3dnr_diagnostics_restore(&state) == -1);
    assert(state.restore_pending);
    assert(isp_3dnr_diagnostics_restore(&state) == 0);
    puts("ISP 3DNR diagnostics tests passed (native ABI, update, readback, rollback)");
    return 0;
}
