#ifndef IPC_CAMERA_AUDIO_ALARM_H
#define IPC_CAMERA_AUDIO_ALARM_H

typedef enum AudioAlarmKind {
    AUDIO_ALARM_LINE_CROSSING = 1,
    AUDIO_ALARM_REGION_ENTER = 2
} AudioAlarmKind;

/* 事件以值复制入队，不保存规则线程栈上的event指针。两个模块的ID独立。 */
typedef struct AudioAlarmEvent {
    AudioAlarmKind kind;
    unsigned int track_id;
    unsigned int region_id;
    unsigned long long sequence;
    unsigned long long frame_pts_us;
} AudioAlarmEvent;

typedef struct AudioAlarmConfig {
    const char *wav_path;      /* create深拷贝路径，start校验并加载音频。 */
    int ao_device;
    int ao_channel;
    int volume;               /* 0..100，使用设备音量，不额外放大软音量。 */
    unsigned int cooldown_ms; /* 全局冷却：两个事件来源共享，使用单调时钟。 */
} AudioAlarmConfig;

typedef struct AudioAlarmContext AudioAlarmContext;

AudioAlarmContext *audio_alarm_create(const AudioAlarmConfig *config);
int audio_alarm_start(AudioAlarmContext *context);
/* 0=入队，1=忙/冷却/队列满而抑制，-1=未启动/已停止/播放服务故障。 */
int audio_alarm_push(AudioAlarmContext *context, const AudioAlarmEvent *event);
/* 先停事件生产者，再stop；唤醒播放线程、join，然后回收AO及音频内存。 */
int audio_alarm_stop(AudioAlarmContext *context);
void audio_alarm_destroy(AudioAlarmContext *context);

#endif
