#ifndef IPC_CAMERA_MP4_RECORDER_H
#define IPC_CAMERA_MP4_RECORDER_H

#include <stddef.h>

#include <media/mm_comm_aio.h>
#include <media/mm_comm_venc.h>

typedef struct Mp4RecorderContext Mp4RecorderContext;

typedef struct Mp4RecorderConfig {
    int mux_channel;             /* MPP MUX 通道号，阶段8.1固定使用0。 */
    int venc_channel;            /* H.264来源VENC通道号。 */
    int width;
    int height;
    int frame_rate;
    int gop_size;
    int sample_rate;
    int audio_channels;
    int audio_bit_width;
    int samples_per_frame;
    const char *output_path;     /* 阶段8.1固定输出MP4路径。 */
    const unsigned char *h264_header; /* VENC产生的SPS/PPS。 */
    size_t h264_header_size;
} Mp4RecorderConfig;

/* 生命周期：create -> start -> push... -> stop -> destroy。 */
Mp4RecorderContext *mp4_recorder_create(const Mp4RecorderConfig *config);
int mp4_recorder_start(Mp4RecorderContext *recorder);

/*
 * Send*StreamSync() 在函数返回前消费当前码流，因此调用者随后即可把编码
 * 缓冲归还VENC/AENC，无需在录像模块中再复制一份大码流。
 */
int mp4_recorder_push_video(Mp4RecorderContext *recorder,
                            const VENC_STREAM_S *stream,
                            int key_frame);
int mp4_recorder_push_audio(Mp4RecorderContext *recorder,
                            const AUDIO_STREAM_S *stream);

int mp4_recorder_stop(Mp4RecorderContext *recorder);
void mp4_recorder_destroy(Mp4RecorderContext *recorder);

#endif
