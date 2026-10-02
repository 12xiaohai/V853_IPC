#include "config.h"

#include <string.h>

int ip_camera_options_parse(int argc, char *argv[], IpCameraOptions *options)
{
    if (options == NULL || argc < 1 || argv == NULL || argv[0] == NULL) {
        return -1;
    }
    memset(options, 0, sizeof(*options));
    options->mode = IPC_CAMERA_MONITOR;
    options->model_path = IPC_CAMERA_DEFAULT_MODEL;
    options->alarm_path = IPC_CAMERA_DEFAULT_ALARM;

    /* 先验证参数完整性，防止测试调用者传入NULL或空路径。 */
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == NULL || argv[index][0] == '\0') {
            return -1;
        }
    }
    if (argc == 1) {
        return 0;
    }
    if (strcmp(argv[1], "--npu-self-test") == 0 && argc <= 4) {
        options->mode = IPC_CAMERA_NPU_SELF_TEST;
        options->model_path = argc >= 3 ? argv[2] : IPC_CAMERA_DEFAULT_MODEL;
        options->input_path = argc == 4 ? argv[3] : NULL;
        return 0;
    }
    if (strcmp(argv[1], "--npu-model") == 0 && argc == 3) {
        options->model_path = argv[2];
        return 0;
    }
    if (strcmp(argv[1], "--audio-alarm-test") == 0 && argc <= 3) {
        options->mode = IPC_CAMERA_AUDIO_ALARM_TEST;
        options->alarm_path = argc == 3 ? argv[2] : IPC_CAMERA_DEFAULT_ALARM;
        return 0;
    }
    return -1;
}

void ip_camera_config_defaults(IpCameraConfig *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));

    /* 三条VIPP共享ISP0：4预览、0编码、8推理，通道与参数沿用板端验证值。 */
    config->preview.device = 4;
    config->preview.isp_device = 0;
    config->preview.channel = 0;
    config->preview.width = 1920;
    config->preview.height = 1080;
    config->preview.frame_rate = 20;
    config->preview.timeout_ms = 200;
    config->preview.pixel_format = MM_PIXEL_FORMAT_YVU_SEMIPLANAR_420;

    /* 横屏摄像头先旋转270度，再缩放到480x800 LCD。 */
    config->display.source_width = config->preview.width;
    config->display.source_height = config->preview.height;
    config->display.pixel_format = config->preview.pixel_format;
    config->display.rotation = 270;
    config->display.display_x = 0;
    config->display.display_y = 0;
    config->display.display_width = 480;
    config->display.display_height = 800;

    config->rtsp.session_id = 0;
    config->rtsp.net_type = RTSP_NET_TYPE_WLAN0;
    config->rtsp.frame_rate = config->preview.frame_rate;
    config->rtsp.queue_capacity = 16;

    config->video_encoder.channel = 0;
    config->video_encoder.vi_device = 0;
    config->video_encoder.isp_device = 0;
    config->video_encoder.vi_channel = 0;
    config->video_encoder.width = config->preview.width;
    config->video_encoder.height = config->preview.height;
    config->video_encoder.frame_rate = config->preview.frame_rate;
    config->video_encoder.bit_rate = 5242880;
    config->video_encoder.gop_size = 75;
    config->video_encoder.pixel_format = config->preview.pixel_format;
    config->video_encoder.output_path = "/mnt/UDISK/sample_demo.h264";

    /* 时间水印附着VENC；录像仍共享H.264/AAC，不增加编码通路。 */
    config->time_osd.venc_channel = config->video_encoder.channel;
    config->time_osd.handle = 0;
    config->time_osd.x = 32;
    config->time_osd.y = 32;
    config->time_osd.update_seconds = 1;

    config->recorder.mux_channel = 0;
    config->recorder.venc_channel = config->video_encoder.channel;
    config->recorder.width = config->video_encoder.width;
    config->recorder.height = config->video_encoder.height;
    config->recorder.frame_rate = config->video_encoder.frame_rate;
    config->recorder.gop_size = config->video_encoder.gop_size;
    config->recorder.sample_rate = 16000;
    config->recorder.audio_channels = 1;
    config->recorder.audio_bit_width = 16;
    config->recorder.samples_per_frame = 1024;
    config->recorder.output_path = NULL; /* 分段模式下由录像器动态生成完整路径。 */
    config->recorder.output_directory = "/mnt/UDISK";
    config->recorder.file_prefix = "record";
    config->recorder.segment_duration_seconds = 60;
    /* 阶段8.3最多保留10段录像，并尽量保证UDISK至少剩余512 MiB。 */
    config->recorder.max_segment_files = 10;
    config->recorder.min_free_space_mb = 512;

    config->audio_encoder.ai_device = 0;
    config->audio_encoder.ai_channel = 0;
    config->audio_encoder.aenc_channel = 0;
    config->audio_encoder.sample_rate = 16000;
    config->audio_encoder.bit_width = 16;
    config->audio_encoder.channels = 1;
    config->audio_encoder.samples_per_frame = 1024;
    config->audio_encoder.volume = 100;
    config->audio_encoder.bit_rate = 0;
    config->audio_encoder.timeout_ms = 200;
    config->audio_encoder.output_path = "/mnt/UDISK/sample_demo.aac";

    /* NV12模型输入和方向修正保持不变；共享sensor不能重复映射翻转。 */
    config->npu.vi_device = 8;
    config->npu.isp_device = 0;
    config->npu.vi_channel = 0;
    config->npu.width = 320;
    config->npu.height = 320;
    config->npu.frame_rate = 10;
    config->npu.buffer_count = 3;
    config->npu.timeout_ms = 200;
    /*
     * VIPP 8抓帧证明图像为正确NV12，但物理安装方向导致人物倒置。
     * 水平镜像+垂直翻转等效于旋转180度，让YOLOv8始终接收正立画面。
     */
    config->npu.mirror = 1;
    config->npu.flip = 1;
    config->npu.model_path = IPC_CAMERA_DEFAULT_MODEL;
    /* 阶段9.2诊断期间保存一张NPU真实输入；确认检测正常后可改为NULL。 */
    config->npu.debug_dump_path = "/mnt/UDISK/npu_realtime_320x320.nv12";
    /* 延迟到约第5秒抓图，给单人测试留出走进摄像头画面的时间。 */
    config->npu.debug_dump_after_frames = 50U;
    config->npu.confidence_threshold = 0.25f;
    config->npu.nms_threshold = 0.45f;
    config->npu.log_interval_frames = 50U;

    config->overlay.target_vi_device = config->video_encoder.vi_device;
    config->overlay.target_vi_channel = config->video_encoder.vi_channel;
    config->overlay.target_width = config->video_encoder.width;
    config->overlay.target_height = config->video_encoder.height;
    config->overlay.model_width = config->npu.width;
    config->overlay.model_height = config->npu.height;
    /*
     * 板端日志显示 SetVippMirror/Flip 最终配置的是共享 sensor：
     *   [ISP] sensor set hflip:1, vflip:1
     * 因此 VIPP 0 和 VIPP 8 会同时看到校正后的方向，画框坐标不能再做
     * 一次 mirror/flip，否则会被重复旋转180度并跑到目标的对角。
     */
    config->overlay.map_mirror = 0;
    config->overlay.map_flip = 0;
    config->overlay.region_handle_base = 100U;
    config->overlay.max_regions = 16U;
    config->overlay.color = 0xffd01bU; /* 黄色，在深浅背景上都较醒目。 */
    config->overlay.thickness = 4U;
    config->overlay.poll_interval_ms = 50U;
    config->overlay.stale_timeout_ms = 500U;

    /* LCD与编码使用独立ORL句柄，坐标在G2D旋转前附着到VIPP4。 */
    config->lcd_overlay = config->overlay;
    config->lcd_overlay.target_vi_device = config->preview.device;
    config->lcd_overlay.target_vi_channel = config->preview.channel;
    config->lcd_overlay.target_width = config->preview.width;
    config->lcd_overlay.target_height = config->preview.height;
    config->lcd_overlay.region_handle_base = 200U;

    /* 消费者先于规则线程启动，两个报警来源共用15秒冷却时间。 */
    config->alarm.wav_path = IPC_CAMERA_DEFAULT_ALARM;
    config->alarm.ao_device = 0;
    config->alarm.ao_channel = 0;
    config->alarm.volume = 35;
    config->alarm.cooldown_ms = 15000U;

    /* 越线使用中央竖线，区域为右半画面，均采用320x320模型坐标。 */
    config->line.model_width = config->npu.width;
    config->line.model_height = config->npu.height;
    config->line.line_ax = config->npu.width / 2;
    config->line.line_ay = 0;
    config->line.line_bx = config->npu.width / 2;
    config->line.line_by = config->npu.height;
    config->line.hysteresis_pixels = 12U;
    config->line.match_distance_pixels = 96U;
    config->line.max_missing_snapshots = 5U;
    config->line.cooldown_ms = 3000U;
    config->line.poll_interval_ms = 50U;
    config->line.max_tracks = 16U;

    config->region.model_width = config->npu.width;
    config->region.model_height = config->npu.height;
    config->region.region_id = 1U;
    config->region.point_count = 4U;
    config->region.points[0] = (RegionPoint){config->npu.width / 2, 0};
    config->region.points[1] = (RegionPoint){config->npu.width, 0};
    config->region.points[2] = (RegionPoint){config->npu.width, config->npu.height};
    config->region.points[3] = (RegionPoint){config->npu.width / 2, config->npu.height};
    config->region.enter_confirm_snapshots = 3U;
    config->region.leave_confirm_snapshots = 3U;
    config->region.match_distance_pixels = 96U;
    config->region.max_missing_snapshots = 5U;
    config->region.max_tracks = 16U;
    config->region.poll_interval_ms = 50U;
    config->region.stale_timeout_ms = 500U;
}
