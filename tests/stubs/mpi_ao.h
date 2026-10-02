#ifndef ALARM_TEST_MPI_AO_H
#define ALARM_TEST_MPI_AO_H

/* 主机测试替身；ARM语法检查必须使用真实SDK头文件，不能使用本文件。 */
typedef int ERRORTYPE;
typedef int AUDIO_SAMPLE_RATE_E;
enum { SUCCESS = 0, FALSE = 0, TRUE = 1, MOD_ID_AO = 5,
       AUDIO_BIT_WIDTH_16 = 1, AUDIO_SOUND_MODE_MONO = 0,
       PCM_CARD_TYPE_AUDIOCODEC = 0 };
typedef enum MPP_EVENT_TYPE {
    MPP_EVENT_RELEASE_AUDIO_BUFFER = 1, MPP_EVENT_NOTIFY_EOF = 0x100
} MPP_EVENT_TYPE;
typedef struct MPP_CHN_S { int mModId, mDevId, mChnId; } MPP_CHN_S;
typedef struct AUDIO_FRAME_S {
    AUDIO_SAMPLE_RATE_E mSamplerate;
    int mBitwidth, mSoundmode;
    void *mpAddr;
    unsigned long long mTimeStamp;
    unsigned int mSeq, mLen, mId;
} AUDIO_FRAME_S;
typedef struct MPPCallbackInfo {
    void *cookie;
    ERRORTYPE (*callback)(void *, MPP_CHN_S *, MPP_EVENT_TYPE, void *);
} MPPCallbackInfo;
int AW_MPI_AO_CreateChn(int, int);
int AW_MPI_AO_DestroyChn(int, int);
int AW_MPI_AO_RegisterCallback(int, int, MPPCallbackInfo *);
int AW_MPI_AO_SetPcmCardType(int, int, int);
int AW_MPI_AO_StartChn(int, int);
int AW_MPI_AO_StopChn(int, int);
int AW_MPI_AO_SetDevVolume(int, int);
int AW_MPI_AO_SetSoftVolume(int, int);
int AW_MPI_AO_SetChnMute(int, int, int);
int AW_MPI_AO_SendFrame(int, int, AUDIO_FRAME_S *, int);
int AW_MPI_AO_SetStreamEof(int, int, int, int);

#endif
