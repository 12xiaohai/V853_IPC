/*
 * 主机白盒回归测试：只编译区域规则实现，不依赖AWNN、MPP或摄像头。
 * 直接包含.c以验证私有几何和状态机，产品Makefile不会扫描tests目录。
 * 本测试不能替代V853上实际人形、音视频和长时间运行验收。
 */
#include "../sample/ipc_camera/app/region_intrusion.c"
#include <assert.h>
#include <stdio.h>

struct NpuDetectorContext {
    NpuDetectionSnapshot snapshot;
    atomic_uint read_calls;
};

int npu_detector_get_latest(NpuDetectorContext *detector,
                            NpuDetectionSnapshot *snapshot)
{
    /* 生命周期测试使用恒定的空快照；主线程不在工作线程运行时修改它。 */
    *snapshot = detector->snapshot;
    atomic_fetch_add_explicit(&detector->read_calls, 1U, memory_order_relaxed);
    return 0;
}

typedef struct EventRecorder {
    unsigned int count;
    RegionIntrusionEvent events[64];
} EventRecorder;

static void record_event(void *opaque, const RegionIntrusionEvent *event)
{
    EventRecorder *recorder = opaque;
    assert(recorder->count < 64U);
    recorder->events[recorder->count++] = *event;
}

static RegionIntrusionConfig default_config(NpuDetectorContext *detector,
                                            EventRecorder *recorder)
{
    RegionIntrusionConfig config;
    memset(&config, 0, sizeof(config));
    config.detector = detector;
    config.model_width = config.model_height = 320;
    config.region_id = 1U;
    config.point_count = 4U;
    config.points[0] = (RegionPoint){160, 0};
    config.points[1] = (RegionPoint){320, 0};
    config.points[2] = (RegionPoint){320, 320};
    config.points[3] = (RegionPoint){160, 320};
    config.enter_confirm_snapshots = config.leave_confirm_snapshots = 3U;
    config.match_distance_pixels = 96U;
    config.max_missing_snapshots = 5U;
    config.max_tracks = 16U;
    config.poll_interval_ms = 5U;
    config.stale_timeout_ms = 50U;
    config.event_callback = record_event;
    config.event_callback_opaque = recorder;
    return config;
}

/* 由底边中点反向构造检测框，便于清楚描述测试中的人员移动。 */
static YoloDetection person_at(int x, int y)
{
    YoloDetection detection = {0, 0.9f, x - 5, y - 20, x + 5, y};
    return detection;
}

static void feed(RegionIntrusionContext *context, unsigned long long sequence,
                 int count, const RegionPoint *points)
{
    NpuDetectionSnapshot snapshot;
    int i;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.sequence = sequence;
    snapshot.frame_pts_us = sequence * 100000ULL;
    snapshot.detection_count = count;
    for (i = 0; i < count; ++i) {
        snapshot.detections[i] = person_at(points[i].x, points[i].y);
    }
    process_snapshot(context, &snapshot);
}

static void one_person(RegionIntrusionContext *context,
                       unsigned long long sequence, int x)
{
    RegionPoint point = {x, 300};
    feed(context, sequence, 1, &point);
}

static void test_geometry(void)
{
    NpuDetectorContext detector = {0};
    EventRecorder recorder = {0};
    RegionIntrusionConfig config = default_config(&detector, &recorder);
    RegionIntrusionContext *context;
    unsigned int i;

    assert(valid_polygon(&config));
    assert(point_in_polygon(&config, (RegionPoint){200, 300}));
    assert(!point_in_polygon(&config, (RegionPoint){120, 300}));
    assert(point_in_polygon(&config, (RegionPoint){160, 300})); /* 边界 */
    assert(point_in_polygon(&config, (RegionPoint){320, 320})); /* 顶点 */
    assert(!point_in_polygon(&config, (RegionPoint){200, -1}));
    for (i = 0U; i < 2U; ++i) {
        RegionPoint temporary = config.points[i];
        config.points[i] = config.points[3U - i];
        config.points[3U - i] = temporary;
    }
    assert(valid_polygon(&config));
    assert(point_in_polygon(&config, (RegionPoint){200, 300})); /* 逆序也可 */

    config.point_count = 6U; /* L形凹多边形 */
    config.points[0] = (RegionPoint){20, 20};
    config.points[1] = (RegionPoint){300, 20};
    config.points[2] = (RegionPoint){300, 120};
    config.points[3] = (RegionPoint){120, 120};
    config.points[4] = (RegionPoint){120, 300};
    config.points[5] = (RegionPoint){20, 300};
    assert(valid_polygon(&config));
    assert(point_in_polygon(&config, (RegionPoint){60, 250}));
    assert(point_in_polygon(&config, (RegionPoint){250, 60}));
    assert(!point_in_polygon(&config, (RegionPoint){250, 250}));
    config = default_config(&detector, &recorder);
    config.points[1] = (RegionPoint){320, 320};
    config.points[2] = (RegionPoint){320, 0}; /* 蝴蝶结自交 */
    assert(region_intrusion_create(&config) == NULL);
    config = default_config(&detector, &recorder);
    config.points[2] = config.points[1];
    assert(region_intrusion_create(&config) == NULL);
    config = default_config(&detector, &recorder);
    config.points[0].x = -1;
    assert(region_intrusion_create(&config) == NULL);
    config = default_config(&detector, &recorder);
    config.points[1].x = INT_MAX;
    config.points[2].y = INT_MIN;
    assert(region_intrusion_create(&config) == NULL);
    config = default_config(&detector, &recorder);
    config.point_count = 3U;
    config.points[0] = (RegionPoint){10, 10};
    config.points[1] = (RegionPoint){20, 20};
    config.points[2] = (RegionPoint){30, 30}; /* 零面积 */
    assert(region_intrusion_create(&config) == NULL);
    assert(region_intrusion_create(NULL) == NULL);
    config = default_config(&detector, &recorder);
    context = region_intrusion_create(&config);
    assert(context != NULL);
    region_intrusion_destroy(context);
}

static void test_enter_stay_leave_reenter(void)
{
    NpuDetectorContext detector = {0};
    EventRecorder recorder = {0};
    RegionIntrusionConfig config = default_config(&detector, &recorder);
    RegionIntrusionContext *context = region_intrusion_create(&config);
    unsigned long long seq;
    assert(context != NULL);

    one_person(context, 1U, 130);
    one_person(context, 2U, 200);
    one_person(context, 2U, 200); /* 同一快照不重复计数 */
    one_person(context, 3U, 200);
    assert(recorder.count == 0U);
    one_person(context, 4U, 200);
    assert(recorder.count == 1U);
    assert(recorder.events[0].type == REGION_INTRUSION_ENTER);
    assert(recorder.events[0].sequence == 4U);
    assert(recorder.events[0].frame_pts_us == 400000ULL);
    for (seq = 5U; seq <= 20U; ++seq) {
        one_person(context, seq, 200);
    }
    assert(recorder.count == 1U); /* 持续停留不重复enter */
    one_person(context, 21U, 130);
    one_person(context, 22U, 130);
    assert(recorder.count == 1U);
    one_person(context, 23U, 130);
    assert(recorder.count == 2U);
    assert(recorder.events[1].type == REGION_INTRUSION_LEAVE);
    for (seq = 24U; seq <= 26U; ++seq) {
        one_person(context, seq, 200);
    }
    assert(recorder.count == 3U);
    assert(recorder.events[2].type == REGION_INTRUSION_ENTER);
    assert(recorder.events[0].track_id == recorder.events[2].track_id);
    assert(context->enter_events == 2U && context->leave_events == 1U);
    region_intrusion_destroy(context);
}

static void test_pending_and_missing(void)
{
    NpuDetectorContext detector = {0};
    EventRecorder recorder = {0};
    RegionIntrusionConfig config = default_config(&detector, &recorder);
    RegionIntrusionContext *context = region_intrusion_create(&config);
    unsigned long long seq = 1U;
    unsigned int i;
    assert(context != NULL);
    /* 边界两侧交替抖动，不能凑出连续3帧。 */
    for (i = 0U; i < 10U; ++i) {
        one_person(context, seq++, i % 2U ? 159 : 161);
    }
    assert(recorder.count == 0U);
    one_person(context, seq++, 200);
    one_person(context, seq++, 200);
    feed(context, seq++, 0, NULL); /* 待进入时漏检，计数从头开始 */
    one_person(context, seq++, 200);
    one_person(context, seq++, 200);
    assert(recorder.count == 0U);
    one_person(context, seq++, 200);
    assert(recorder.count == 1U);
    feed(context, seq++, 0, NULL);
    feed(context, seq++, 0, NULL);
    one_person(context, seq++, 200); /* 已在内的短暂漏检不重复enter */
    assert(recorder.count == 1U);
    one_person(context, seq++, 130);
    feed(context, seq++, 0, NULL); /* 待离开被漏检中断 */
    one_person(context, seq++, 130);
    one_person(context, seq++, 130);
    assert(recorder.count == 1U);
    one_person(context, seq++, 200); /* 回到区域内撤销待离开 */
    for (i = 0U; i < 6U; ++i) {
        feed(context, seq++, 0, NULL);
    }
    assert(recorder.count == 1U && context->expired_tracks == 1U);
    for (i = 0U; i < 3U; ++i) {
        one_person(context, seq++, 200);
    }
    assert(recorder.count == 2U);
    assert(recorder.events[0].track_id != recorder.events[1].track_id);
    /* 长漏检后ID重建会再次报enter：明确测试并记录这一轻量跟踪局限。 */
    region_intrusion_destroy(context);
}

static void test_sequence_gap_and_capacity(void)
{
    NpuDetectorContext detector = {0};
    EventRecorder recorder = {0};
    RegionIntrusionConfig config = default_config(&detector, &recorder);
    RegionIntrusionContext *context = region_intrusion_create(&config);
    NpuDetectionSnapshot snapshot;
    RegionPoint points[YOLOV8_MAX_DETECTIONS];
    unsigned int i, active = 0U;
    assert(context != NULL);
    one_person(context, 1U, 200);
    one_person(context, 2U, 200);
    one_person(context, 4U, 200); /* 跳过3，不能沿用连续计数 */
    assert(recorder.count == 0U);
    one_person(context, 5U, 200);
    one_person(context, 6U, 200);
    assert(recorder.count == 1U);
    region_intrusion_destroy(context);
    context = region_intrusion_create(&config);
    assert(context != NULL);
    for (i = 0U; i < 3U; ++i) {
        points[i] = (RegionPoint){100, 300};
    }
    config.max_tracks = 2U;
    context->config.max_tracks = 2U;
    feed(context, 1U, 3, points);
    for (i = 0U; i < config.max_tracks; ++i) {
        active += context->tracks[i].active != 0;
    }
    assert(active == config.max_tracks && recorder.count == 1U);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.sequence = 2U;
    snapshot.detection_count = YOLOV8_MAX_DETECTIONS + 100;
    process_snapshot(context, &snapshot); /* 无效框和异常数量不能越界 */
    snapshot.sequence = 3U;
    snapshot.detection_count = -1;
    process_snapshot(context, &snapshot);
    region_intrusion_destroy(context);
}

static void test_multiple_persons(void)
{
    NpuDetectorContext detector = {0};
    EventRecorder recorder = {0};
    RegionIntrusionConfig config = default_config(&detector, &recorder);
    RegionIntrusionContext *context = region_intrusion_create(&config);
    RegionPoint points[2] = {{200, 80}, {200, 300}};
    unsigned long long seq;
    assert(context != NULL);
    for (seq = 1U; seq <= 3U; ++seq) {
        feed(context, seq, 2, points);
    }
    assert(recorder.count == 2U);
    assert(recorder.events[0].track_id != recorder.events[1].track_id);
    /* 框列表顺序交换，一人离开，另一人停留，状态不能相互覆盖。 */
    points[0] = (RegionPoint){200, 300};
    points[1] = (RegionPoint){130, 80};
    for (seq = 4U; seq <= 6U; ++seq) {
        feed(context, seq, 2, points);
    }
    assert(recorder.count == 3U);
    assert(recorder.events[2].type == REGION_INTRUSION_LEAVE);
    assert(recorder.events[2].point_y == 80);
    region_intrusion_destroy(context);
}

static void test_thread_lifecycle(void)
{
    NpuDetectorContext detector = {0};
    EventRecorder recorder = {0};
    RegionIntrusionConfig config = default_config(&detector, &recorder);
    RegionIntrusionContext *context = region_intrusion_create(&config);
    assert(context != NULL);
    assert(region_intrusion_start(NULL) == -1);
    assert(region_intrusion_start(context) == 0);
    assert(region_intrusion_start(context) == -1);
    assert(region_intrusion_stop(context) == 0);
    assert(region_intrusion_stop(context) == 0);
    assert(region_intrusion_start(context) == 0);
    region_intrusion_destroy(context); /* 自动stop并join */
    region_intrusion_destroy(NULL);
}

static void test_stale_snapshot(void)
{
    NpuDetectorContext detector = {0};
    EventRecorder recorder = {0};
    RegionIntrusionConfig config = default_config(&detector, &recorder);
    RegionIntrusionContext *context;
    unsigned long long deadline;
    atomic_init(&detector.read_calls, 0U);
    detector.snapshot.sequence = 1U;
    detector.snapshot.detection_count = 1;
    detector.snapshot.detections[0] = person_at(200, 320);
    config.enter_confirm_snapshots = 1U;
    context = region_intrusion_create(&config);
    assert(context != NULL);
    assert(region_intrusion_start(context) == 0);
    deadline = monotonic_time_ms() + 2000U;
    /* 等待至少20轮空更新，超过50ms陈旧阈值；两秒超时避免测试挂住。 */
    while (atomic_load_explicit(&detector.read_calls, memory_order_relaxed) < 20U &&
           monotonic_time_ms() < deadline) {
        sleep_ms(5U);
    }
    assert(region_intrusion_stop(context) == 0);
    /* join后才读取线程写过的记录和上下文，不制造测试自身的数据竞争。 */
    assert(atomic_load_explicit(&detector.read_calls, memory_order_relaxed) >= 20U);
    assert(recorder.count == 1U && recorder.events[0].type == REGION_INTRUSION_ENTER);
    assert(context->processed_snapshots == 1U);
    assert(context->expired_tracks == 1U && context->leave_events == 0U);
    region_intrusion_destroy(context);
}

int main(void)
{
    test_geometry();
    test_enter_stay_leave_reenter();
    test_pending_and_missing();
    test_sequence_gap_and_capacity();
    test_multiple_persons();
    test_thread_lifecycle();
    test_stale_snapshot();
    puts("PASS: region geometry, state machine, tracking, capacity, lifecycle and stale snapshot");
    return 0;
}
