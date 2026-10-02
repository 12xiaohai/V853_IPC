#ifndef IPC_CAMERA_WAV_READER_H
#define IPC_CAMERA_WAV_READER_H

#include <stddef.h>
#include <stdio.h>

/* 初期只支持PCM16单声道；data保存真正音频样本，不包含RIFF文件头。 */
typedef struct WavPcm {
    unsigned char *data;
    size_t size;
    unsigned int sample_rate;
} WavPcm;

/* stream必须可seek。失败时输出保持空，调用者负责关闭FILE。 */
int wav_pcm_read(FILE *stream, WavPcm *pcm);
void wav_pcm_free(WavPcm *pcm);

#endif
