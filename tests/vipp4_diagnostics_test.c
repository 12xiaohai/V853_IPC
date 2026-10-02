#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* 用确定的时间代替真实时钟，让慢处理统计可以稳定回归。 */
static uint64_t fake_now;
static int clock_reads;
static int test_clock_gettime(int clock_id, struct timespec *value)
{
    (void)clock_id;
    ++clock_reads;
    value->tv_sec = (time_t)(fake_now / 1000000ULL);
    value->tv_nsec = (long)((fake_now % 1000000ULL) * 1000ULL);
    return 0;
}
#define clock_gettime test_clock_gettime
/* 直接测试真实采集循环；MPP/G2D/VO是主机替身，不操作开发板。 */
#include "../sample/ipc_camera/app/video_capture.c"
#undef clock_gettime

static VideoCaptureContext *active_capture;
static unsigned int acquired_frames, released_frames, target_frames;
static int get_calls, display_calls, timed_calls;
static int fail_first_get, fail_release, display_return, slow_log_count;
static char final_summary[1024], final_durations[1024], final_display[1024];
static VI_ATTR_S configured_attributes;

void vipp4_test_log(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (strstr(line, "[VIPP4-DIAG] final") != NULL) {
        strcpy(final_summary, line);
    }
    if (strstr(line, "us avg/max: get=") != NULL) {
        strcpy(final_durations, line);
    }
    if (strstr(line, "us avg/max: g2d=") != NULL) {
        strcpy(final_display, line);
    }
    if (strstr(line, "Slow hold:") != NULL) {
        ++slow_log_count;
    }
}

ERRORTYPE AW_MPI_VI_GetFrame(VI_DEV dev, VI_CHN ch,
                            VIDEO_FRAME_INFO_S *frame, int timeout)
{
    assert(dev == 4 && ch == 0 && timeout == 200);
    ++get_calls;
    fake_now += 1000;
    if (fail_first_get && get_calls == 1) {
        return -1;
    }
    frame->mId = ++acquired_frames;
    frame->VFrame.mWidth = 1920;
    frame->VFrame.mHeight = 1080;
    frame->VFrame.mpts = acquired_frames * 50000ULL;
    return SUCCESS;
}

ERRORTYPE AW_MPI_VI_ReleaseFrame(VI_DEV dev, VI_CHN ch,
                                VIDEO_FRAME_INFO_S *frame)
{
    assert(dev == 4 && ch == 0);
    /* 每次成功取帧恰好归还一次，GetFrame失败不归还。 */
    assert(frame->mId == released_frames + 1);
    ++released_frames;
    fake_now += 2000;
    if (released_frames == target_frames) {
        active_capture->stop_requested = 1;
    }
    return fail_release ? -1 : SUCCESS;
}

int video_display_submit(VideoDisplayContext *display,
                         const VIDEO_FRAME_INFO_S *frame)
{
    assert(display != NULL && frame->mId == acquired_frames);
    ++display_calls;
    return display_return;
}

int video_display_submit_timed(VideoDisplayContext *display,
                               const VIDEO_FRAME_INFO_S *frame,
                               VideoDisplayTiming *timing)
{
    assert(display != NULL && frame->mId == acquired_frames);
    ++timed_calls;
    timing->g2d_begin_us = fake_now;
    fake_now += 50000; /* 模拟一次50ms旋转，再加10ms送显。 */
    timing->g2d_end_us = fake_now;
    timing->vo_begin_us = fake_now;
    fake_now += 10000;
    timing->vo_end_us = fake_now;
    return display_return;
}

/* 初始化/回收替身：真实线程入口仍使用产品代码。 */
ERRORTYPE AW_MPI_VI_SetVippAttr(VI_DEV dev, VI_ATTR_S *attr)
{
    assert(dev == 4);
    configured_attributes = *attr;
    return SUCCESS;
}
#define VIPP_STUB(name) ERRORTYPE name(VI_DEV dev) { assert(dev == 4); return 0; }
VIPP_STUB(AW_MPI_VI_CreateVipp)
VIPP_STUB(AW_MPI_VI_DestroyVipp)
VIPP_STUB(AW_MPI_VI_EnableVipp)
VIPP_STUB(AW_MPI_VI_DisableVipp)
#define CHANNEL_STUB(name) ERRORTYPE name(VI_DEV dev, VI_CHN ch) \
    { assert(dev == 4 && ch == 0); return 0; }
CHANNEL_STUB(AW_MPI_VI_DestroyVirChn)
CHANNEL_STUB(AW_MPI_VI_EnableVirChn)
CHANNEL_STUB(AW_MPI_VI_DisableVirChn)
ERRORTYPE AW_MPI_VI_CreateVirChn(VI_DEV dev, VI_CHN ch, void *attr)
{ assert(dev == 4 && ch == 0 && attr == NULL); return 0; }
ERRORTYPE AW_MPI_ISP_Run(ISP_DEV dev) { assert(dev == 0); return 0; }
ERRORTYPE AW_MPI_ISP_Stop(ISP_DEV dev) { assert(dev == 0); return 0; }
int map_PIXEL_FORMAT_E_to_V4L2_PIX_FMT(PIXEL_FORMAT_E format) { return format; }

static void run_capture(int timing, int capture_only, int render_result,
                        int get_failure, int release_failure)
{
    VideoCaptureContext capture;
    memset(&capture, 0, sizeof(capture));
    capture.device = 4;
    capture.width = 1920;
    capture.height = 1080;
    capture.frame_rate = 20;
    capture.timeout_ms = 200;
    capture.display = (VideoDisplayContext *)(uintptr_t)1;
    capture.diagnostic_timing = timing;
    capture.diagnostic_capture_only = capture_only;
    active_capture = &capture;
    fake_now = 1000000;
    clock_reads = get_calls = display_calls = timed_calls = slow_log_count = 0;
    acquired_frames = released_frames = 0;
    target_frames = 200;
    fail_first_get = get_failure;
    fail_release = release_failure;
    display_return = render_result;
    final_summary[0] = final_durations[0] = final_display[0] = 0;

    assert(video_capture_start(&capture) == 0);
    assert(pthread_join(capture.thread_id, NULL) == 0);
    capture.thread_started = 0; /* 已join，不再让stop重复join。 */
    assert(video_capture_stop(&capture) == 0);
    assert(acquired_frames == 200 && released_frames == 200);
    assert(get_calls == 200 + get_failure);
    assert(configured_attributes.nbufs == 5);
    assert(configured_attributes.format.width == 1920);
    assert(configured_attributes.format.height == 1080);
    assert(configured_attributes.fps == 20);
    assert(capture.vipp_created == 0 && capture.channel_created == 0);
    if (!timing) {
        assert(clock_reads == 0 && final_summary[0] == 0);
        assert(display_calls == (capture_only ? 0 : 200) && timed_calls == 0);
    } else if (capture_only) {
        assert(display_calls == 0 && timed_calls == 0);
        assert(strstr(final_summary, "mode=capture-only") != NULL);
        assert(strstr(final_summary, "slow_hold=0") != NULL);
        assert(strstr(final_durations, "hold=2000/2000") != NULL);
        assert(strstr(final_display, "g2d=0/0 samples=0") != NULL);
        assert(strstr(final_display, "vo_send=0/0 samples=0") != NULL);
    } else {
        assert(timed_calls == 200 && display_calls == 0);
        assert(strstr(final_summary, "slow_hold=200") != NULL);
        assert(strstr(final_durations, "get=1000/1000 hold=62000/62000") != NULL);
        assert(strstr(final_display, "g2d=50000/50000 samples=200") != NULL);
        assert(strstr(final_display, "vo_send=10000/10000 samples=200") != NULL);
        assert(slow_log_count == 5); /* 前3次、100次、200次。 */
    }
    if (timing) {
        assert(strstr(final_summary, get_failure ? "get_fail=1" : "get_fail=0"));
        assert(strstr(final_summary, release_failure ? "release_fail=200" :
                                                      "release_fail=0"));
        assert(strstr(final_summary, render_result < 0 && !capture_only
                                      ? "display_fail=200" : "display_fail=0"));
        assert(strstr(final_summary, render_result > 0 && !capture_only
                                      ? "display_drop=200" : "display_drop=0"));
    }
}

int main(void)
{
    Vipp4DurationStat stat = {0};
    assert(vipp4_duration_avg(&stat) == 0);
    vipp4_duration_add(&stat, 100, 120);
    vipp4_duration_add(&stat, 100, 140);
    vipp4_duration_add(&stat, 0, 999); /* 无效时钟不计入。 */
    vipp4_duration_add(&stat, 200, 100);
    assert(stat.count == 2 && stat.max_us == 40);
    assert(vipp4_duration_avg(&stat) == 30);
    assert(vipp4_elapsed_us(0, 999) == 0);
    assert(vipp4_elapsed_us(200, 100) == 0);
    run_capture(0, 0, 0, 0, 0);
    run_capture(1, 0, 0, 0, 0);
    run_capture(1, 1, 0, 0, 0);
    run_capture(1, 0, -1, 1, 0); /* 显示失败仍归还源帧。 */
    run_capture(1, 0, 1, 0, 1);  /* 丢预览帧、归还失败均进入统计。 */
    run_capture(1, 1, -1, 1, 0); /* bypass不调用显示，也不归还失败取帧。 */
    puts("VIPP4 diagnostics tests passed (6 capture runs, 1200 released frames)");
    return 0;
}
