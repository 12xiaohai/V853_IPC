#ifndef IPC_CAMERA_AUDIO_CAPTURE_H
#define IPC_CAMERA_AUDIO_CAPTURE_H

/* 音频采集模块对外只暴露不透明指针，调用者不需要了解内部线程和 MPP 状态。 */
typedef struct AudioCaptureContext AudioCaptureContext;

typedef struct AudioCaptureConfig {
    int device;             /* AI 设备号，板载 AudioCodec 使用 0。 */
    int channel;            /* AI 通道号。 */
    int sample_rate;        /* 每秒采样点数，当前为 16000 Hz。 */
    int bit_width;          /* 每个采样点的位数，当前为 16 bit。 */
    int channels;           /* 声道数，当前为单声道 1。 */
    int samples_per_frame;  /* 一帧 PCM 包含的采样点数。 */
    int volume;             /* AI 设备录音音量。 */
    int timeout_ms;         /* GetFrame 超时，使线程能及时检查退出标志。 */
    const char *output_path;/* 阶段 6.1 保存的裸 PCM 文件。 */
} AudioCaptureConfig;

/* 生命周期：create -> start -> stop -> destroy。 */
AudioCaptureContext *audio_capture_create(const AudioCaptureConfig *config);
int audio_capture_start(AudioCaptureContext *capture);
int audio_capture_stop(AudioCaptureContext *capture);
void audio_capture_destroy(AudioCaptureContext *capture);

#endif
