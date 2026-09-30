#ifndef IPC_CAMERA_AUDIO_ENCODER_H
#define IPC_CAMERA_AUDIO_ENCODER_H

/*
 * 对外只暴露不透明上下文。调用者只负责配置和生命周期管理，
 * 不直接操作内部的 AI、AENC、绑定关系和取流线程。
 */
typedef struct AudioEncoderContext AudioEncoderContext;

typedef struct AudioEncoderConfig {
    int ai_device;          /* 音频输入设备号，板载 AudioCodec 使用 0。 */
    int ai_channel;         /* AI 通道号。 */
    int aenc_channel;       /* AAC 编码通道号。 */
    int sample_rate;        /* 采样率，当前使用 16000 Hz。 */
    int bit_width;          /* PCM 位宽，当前使用 16 bit。 */
    int channels;           /* 声道数，当前使用单声道。 */
    int samples_per_frame;  /* 一帧 PCM 中的采样点数。 */
    int volume;             /* 麦克风采集音量。 */
    int bit_rate;           /* AAC 目标码率；0 表示采用 SDK 默认值。 */
    int timeout_ms;         /* AENC GetStream 的超时时间。 */
    const char *output_path;/* 带 ADTS 帧头的 AAC 文件保存路径。 */
} AudioEncoderConfig;

/* 生命周期必须遵循 create -> start -> stop -> destroy。 */
AudioEncoderContext *audio_encoder_create(const AudioEncoderConfig *config);
int audio_encoder_start(AudioEncoderContext *encoder);
int audio_encoder_stop(AudioEncoderContext *encoder);
void audio_encoder_destroy(AudioEncoderContext *encoder);

#endif
