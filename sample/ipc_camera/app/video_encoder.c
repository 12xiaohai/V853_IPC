#include "video_encoder.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <media/mpi_isp.h>
#include <media/mpi_sys.h>
#include <media/mpi_vi.h>
#include <media/mpi_venc.h>
#include <media/mpi_videoformat_conversion.h>
#include <utils/plat_log.h>

struct VideoEncoderContext {
    VideoEncoderConfig config;
    char output_path[256];           /* 内部副本，避免外部字符串提前失效。 */
    FILE *output_file;               /* 阶段测试使用的本地 Annex-B 文件。 */
    unsigned char *h264_header;      /* SPS/PPS 的应用层副本。 */
    size_t h264_header_size;
    pthread_t stream_thread;
    volatile int stop_requested;

    /* 记录初始化进度，使 stop 可做失败回滚。 */
    int channel_created;
    int receiving;
    int thread_started;
    int vipp_created;
    int isp_running;
    int vipp_enabled;
    int vi_channel_created;
    int vi_channel_enabled;
    int channels_bound;

    unsigned long long encoded_frames;
    unsigned long long key_frames;
    unsigned long long encoded_bytes;
};

/* VENC 的一帧可被拆成最多三段，写文件时必须按 0->1->2 顺序拼接。 */
static int write_stream_pack(FILE *file, const VENC_PACK_S *pack)
{
    size_t written;

    if (pack->mLen0 > 0U) {
        written = fwrite(pack->mpAddr0, 1, pack->mLen0, file);
        if (written != pack->mLen0) {
            return -1;
        }
    }
    if (pack->mLen1 > 0U) {
        written = fwrite(pack->mpAddr1, 1, pack->mLen1, file);
        if (written != pack->mLen1) {
            return -1;
        }
    }
    if (pack->mLen2 > 0U) {
        written = fwrite(pack->mpAddr2, 1, pack->mLen2, file);
        if (written != pack->mLen2) {
            return -1;
        }
    }
    return 0;
}

/*
 * 编码码流消费线程。原始 NV21 帧由 MPP Bind 直接从 VI 传到 VENC，
 * 该线程只获取已压缩的 H.264，同时写本地文件并推入 RTSP 队列。
 */
static void *video_encoder_stream_thread(void *argument)
{
    VideoEncoderContext *encoder = argument;

    alogd("[VENC] Stream thread started");

    for (;;) {
        VENC_STREAM_S stream;
        VENC_PACK_S pack;
        unsigned int stream_length;
        int is_key_frame;
        ERRORTYPE ret;

        memset(&stream, 0, sizeof(stream));
        memset(&pack, 0, sizeof(pack));
        /* 当前编码器按帧输出，一次 GetStream 接收一个 pack 描述符。 */
        stream.mPackCount = 1;
        stream.mpPack = &pack;

        /* 200 ms 超时避免退出时永久阻塞在 GetStream。 */
        ret = AW_MPI_VENC_GetStream(encoder->config.channel, &stream, 200);
        if (ret != SUCCESS) {
            if (encoder->stop_requested) {
                break;
            }
            continue;
        }

        stream_length = pack.mLen0 + pack.mLen1 + pack.mLen2;
        /* IDR(I) 帧可作为新客户端开始解码的随机接入点。 */
        is_key_frame = pack.mDataType.enH264EType == H264E_NALU_ISLICE;

        if (stream_length == 0U) {
            alogw("[VENC] Empty encoded stream: seq=%u", stream.mSeq);
        } else if (write_stream_pack(encoder->output_file, &pack) != 0) {
            aloge("[VENC] Write output file failed: %s",
                  encoder->output_path);
            encoder->stop_requested = 1;
        } else {
            ++encoder->encoded_frames;
            encoder->encoded_bytes += stream_length;
            if (is_key_frame) {
                ++encoder->key_frames;
            }

            if (encoder->encoded_frames == 1ULL ||
                (encoder->encoded_frames % 100ULL) == 0ULL) {
                alogd("[VENC] Encoded frame=%llu, seq=%u, bytes=%u, "
                      "key=%d, pts=%llu us",
                      encoder->encoded_frames,
                      stream.mSeq,
                      stream_length,
                      is_key_frame,
                      (unsigned long long)pack.mPTS);
                fflush(encoder->output_file);
            }

            /*
             * 回调在 ReleaseStream 之前执行。RTSP 模块会在回调中完成深拷贝，
             * 因此下面归还 VENC 缓冲后，发送线程仍可安全访问数据。
             */
            if (encoder->config.frame_callback != NULL &&
                encoder->config.frame_callback(
                    encoder->config.frame_callback_opaque,
                    encoder->h264_header,
                    encoder->h264_header_size,
                    pack.mpAddr0,
                    pack.mLen0,
                    pack.mpAddr1,
                    pack.mLen1,
                    pack.mpAddr2,
                    pack.mLen2,
                    (unsigned long long)pack.mPTS,
                    is_key_frame) != 0) {
                alogw("[VENC] RTSP queue rejected frame: seq=%u, key=%d",
                      stream.mSeq,
                      is_key_frame);
            }
        }

        /* 每次 GetStream 成功都必须 ReleaseStream，否则 VENC 输出缓冲最终会耗尽。 */
        ret = AW_MPI_VENC_ReleaseStream(encoder->config.channel, &stream);
        if (ret != SUCCESS) {
            aloge("[VENC] ReleaseStream failed: ret=%d", ret);
        }
    }

    fflush(encoder->output_file);
    alogd("[VENC] Stream thread stopped, frames=%llu, bytes=%llu",
          encoder->encoded_frames,
          encoder->encoded_bytes);
    return NULL;
}

/* 填充 H.264 编码器、CBR 码控和 GOP 参数。 */
static void configure_channel_attributes(const VideoEncoderContext *encoder,
                                         VENC_CHN_ATTR_S *attributes)
{
    unsigned int threshold_size;
    unsigned int buffer_size;

    /* 先把 bit/s 换算成 byte/s，再按帧率估算单帧阈值和 VBV 容量。 */
    threshold_size = (unsigned int)(encoder->config.bit_rate / 8 /
                                    encoder->config.frame_rate * 15);
    buffer_size = (unsigned int)(encoder->config.bit_rate / 8 * 4) +
                  threshold_size;

    memset(attributes, 0, sizeof(*attributes));
    attributes->VeAttr.Type = PT_H264;
    attributes->VeAttr.AttrH264e.MaxPicWidth =
        (unsigned int)encoder->config.width;
    attributes->VeAttr.AttrH264e.MaxPicHeight =
        (unsigned int)encoder->config.height;
    attributes->VeAttr.AttrH264e.BufSize = buffer_size;
    attributes->VeAttr.AttrH264e.Profile = 1; /* 1 表示 H.264 Main Profile。 */
    attributes->VeAttr.AttrH264e.bByFrame = TRUE;
    attributes->VeAttr.AttrH264e.PicWidth =
        (unsigned int)encoder->config.width;
    attributes->VeAttr.AttrH264e.PicHeight =
        (unsigned int)encoder->config.height;
    attributes->VeAttr.AttrH264e.mLevel = H264_LEVEL_Default;
    attributes->VeAttr.AttrH264e.mbPIntraEnable = TRUE;
    attributes->VeAttr.AttrH264e.mThreshSize = threshold_size;

    attributes->VeAttr.MaxKeyInterval = encoder->config.gop_size;
    attributes->VeAttr.SrcPicWidth = (unsigned int)encoder->config.width;
    attributes->VeAttr.SrcPicHeight = (unsigned int)encoder->config.height;
    attributes->VeAttr.Field = VIDEO_FIELD_FRAME;
    attributes->VeAttr.PixelFormat = encoder->config.pixel_format;
    attributes->VeAttr.mColorSpace = V4L2_COLORSPACE_REC709;
    attributes->VeAttr.mOnlineEnable = 0;
    attributes->VeAttr.mVeRefFrameLbcMode = 0;
    attributes->VeAttr.mVeRecRefBufReduceEnable = 0;

    /* CBR（恒定码率）便于控制 RTSP 网络带宽。 */
    attributes->RcAttr.mRcMode = VENC_RC_MODE_H264CBR;
    attributes->RcAttr.mProductMode = 1;
    attributes->RcAttr.mAttrH264Cbr.mGop =
        (unsigned int)encoder->config.gop_size;
    attributes->RcAttr.mAttrH264Cbr.mStatTime = 1;
    attributes->RcAttr.mAttrH264Cbr.mSrcFrmRate =
        (unsigned int)encoder->config.frame_rate;
    attributes->RcAttr.mAttrH264Cbr.mDstFrmRate =
        (unsigned int)encoder->config.frame_rate;
    attributes->RcAttr.mAttrH264Cbr.mBitRate =
        (unsigned int)encoder->config.bit_rate;
    attributes->RcAttr.mAttrH264Cbr.mFluctuateLevel = 0;

    attributes->GopAttr.enGopMode = VENC_GOPMODE_NORMALP;
    attributes->GopAttr.mGopSize = encoder->config.gop_size;
}

/* 限制量化参数 QP：QP 越小画质越好，但码流通常越大。 */
static int configure_rate_control(VideoEncoderContext *encoder)
{
    VENC_RC_PARAM_S parameters;
    ERRORTYPE ret;

    memset(&parameters, 0, sizeof(parameters));
    ret = AW_MPI_VENC_GetRcParam(encoder->config.channel, &parameters);
    if (ret != SUCCESS) {
        aloge("[VENC] GetRcParam failed: ret=%d", ret);
        return -1;
    }

    parameters.ParamH264Cbr.mMaxQp = 36;
    parameters.ParamH264Cbr.mMinQp = 22;
    parameters.ParamH264Cbr.mMaxPqp = 36;
    parameters.ParamH264Cbr.mMinPqp = 22;
    parameters.ParamH264Cbr.mQpInit = 25;
    parameters.ParamH264Cbr.mbEnMbQpLimit = 1;

    ret = AW_MPI_VENC_SetRcParam(encoder->config.channel, &parameters);
    if (ret != SUCCESS) {
        aloge("[VENC] SetRcParam failed: ret=%d", ret);
        return -1;
    }
    return 0;
}

/* 编码专用 VIPP 0 的 VI 输入参数，与预览 VIPP 4 互相独立。 */
static void configure_vi_attributes(const VideoEncoderContext *encoder,
                                    VI_ATTR_S *attributes)
{
    memset(attributes, 0, sizeof(*attributes));
    attributes->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    attributes->memtype = V4L2_MEMORY_MMAP;
    attributes->format.pixelformat =
        map_PIXEL_FORMAT_E_to_V4L2_PIX_FMT(encoder->config.pixel_format);
    attributes->format.field = V4L2_FIELD_NONE;
    attributes->format.colorspace = V4L2_COLORSPACE_REC709;
    attributes->format.width = (unsigned int)encoder->config.width;
    attributes->format.height = (unsigned int)encoder->config.height;
    attributes->nbufs = 5;
    attributes->nplanes = 2;
    attributes->fps = (unsigned int)encoder->config.frame_rate;
    attributes->capturemode = V4L2_MODE_VIDEO;
    attributes->use_current_win = 0;
    attributes->wdr_mode = 0;
    attributes->drop_frame_num = 0;
}

VideoEncoderContext *video_encoder_create(const VideoEncoderConfig *config)
{
    VideoEncoderContext *encoder;

    if (config == NULL || config->width <= 0 || config->height <= 0 ||
        config->frame_rate <= 0 || config->bit_rate <= 0 ||
        config->gop_size <= 0 || config->output_path == NULL) {
        return NULL;
    }

    /* calloc 让所有资源状态标志默认为 0。 */
    encoder = calloc(1, sizeof(*encoder));
    if (encoder == NULL) {
        return NULL;
    }

    encoder->config = *config;
    snprintf(encoder->output_path,
             sizeof(encoder->output_path),
             "%s",
             config->output_path);
    encoder->config.output_path = encoder->output_path;
    return encoder;
}

int video_encoder_start(VideoEncoderContext *encoder)
{
    VI_ATTR_S vi_attributes;
    VENC_CHN_ATTR_S attributes;
    VencHeaderData header;
    MPP_CHN_S vi_mpp_channel;
    MPP_CHN_S venc_mpp_channel;
    ERRORTYPE ret;
    int thread_ret;

    if (encoder == NULL) {
        return -1;
    }

    /* 启动编码专用 VI 通路。 */
    configure_vi_attributes(encoder, &vi_attributes);
    ret = AW_MPI_VI_CreateVipp(encoder->config.vi_device);
    if (ret != SUCCESS) {
        aloge("[VENC] Create encoding VIPP failed: ret=%d", ret);
        return -1;
    }
    encoder->vipp_created = 1;

    ret = AW_MPI_VI_SetVippAttr(encoder->config.vi_device, &vi_attributes);
    if (ret != SUCCESS) {
        aloge("[VENC] Set encoding VIPP attributes failed: ret=%d", ret);
        goto error;
    }

    ret = AW_MPI_ISP_Run(encoder->config.isp_device);
    if (ret != SUCCESS) {
        aloge("[VENC] Start encoding ISP failed: ret=%d", ret);
        goto error;
    }
    encoder->isp_running = 1;

    ret = AW_MPI_VI_EnableVipp(encoder->config.vi_device);
    if (ret != SUCCESS) {
        aloge("[VENC] Enable encoding VIPP failed: ret=%d", ret);
        goto error;
    }
    encoder->vipp_enabled = 1;

    ret = AW_MPI_VI_CreateVirChn(encoder->config.vi_device,
                                 encoder->config.vi_channel,
                                 NULL);
    if (ret != SUCCESS) {
        aloge("[VENC] Create encoding VI channel failed: ret=%d", ret);
        goto error;
    }
    encoder->vi_channel_created = 1;

    /* VI 就绪后创建 VENC 通道并配置码控。 */
    configure_channel_attributes(encoder, &attributes);
    ret = AW_MPI_VENC_CreateChn(encoder->config.channel, &attributes);
    if (ret != SUCCESS) {
        aloge("[VENC] Create channel failed: ret=%d", ret);
        goto error;
    }
    encoder->channel_created = 1;

    if (configure_rate_control(encoder) != 0) {
        goto error;
    }

    /* wb 会清空旧文件，并以二进制方式写入本次 H.264 码流。 */
    encoder->output_file = fopen(encoder->output_path, "wb");
    if (encoder->output_file == NULL) {
        aloge("[VENC] Open output file failed: %s", encoder->output_path);
        goto error;
    }

    /*
     * SPS/PPS 描述分辨率、Profile、Level 等解码参数。文件开头写一次，
     * RTSP 则在每个关键帧前重复附加，便于客户端中途连入。
     */
    memset(&header, 0, sizeof(header));
    ret = AW_MPI_VENC_GetH264SpsPpsInfo(encoder->config.channel, &header);
    if (ret != SUCCESS || header.pBuffer == NULL || header.nLength == 0U) {
        aloge("[VENC] Get SPS/PPS failed: ret=%d", ret);
        goto error;
    }
    if (fwrite(header.pBuffer, 1, header.nLength, encoder->output_file) !=
        header.nLength) {
        aloge("[VENC] Write SPS/PPS failed");
        goto error;
    }
    /* SDK 返回的 header 缓冲生命周期不由应用保证，因此保存自己的副本。 */
    encoder->h264_header = malloc(header.nLength);
    if (encoder->h264_header == NULL) {
        aloge("[VENC] Allocate SPS/PPS copy failed");
        goto error;
    }
    memcpy(encoder->h264_header, header.pBuffer, header.nLength);
    encoder->h264_header_size = header.nLength;

    vi_mpp_channel.mModId = MOD_ID_VIU;
    vi_mpp_channel.mDevId = encoder->config.vi_device;
    vi_mpp_channel.mChnId = encoder->config.vi_channel;
    venc_mpp_channel.mModId = MOD_ID_VENC;
    venc_mpp_channel.mDevId = 0;
    venc_mpp_channel.mChnId = encoder->config.channel;
    /*
     * Bind 后由 MPP 在内核/媒体组件之间传递帧描述符和缓冲所有权，
     * 应用无需自己 GetFrame + SendFrame，也无需 CPU 拷贝 1920x1080 NV21。
     */
    ret = AW_MPI_SYS_Bind(&vi_mpp_channel, &venc_mpp_channel);
    if (ret != SUCCESS) {
        aloge("[VENC] Bind VI to VENC failed: ret=%d", ret);
        goto error;
    }
    encoder->channels_bound = 1;

    /* 先 Bind，再让 VENC 进入接收状态，避免在 Executing 状态临时建立隧道。 */
    ret = AW_MPI_VENC_StartRecvPic(encoder->config.channel);
    if (ret != SUCCESS) {
        aloge("[VENC] StartRecvPic failed: ret=%d", ret);
        goto error;
    }
    encoder->receiving = 1;

    ret = AW_MPI_VI_EnableVirChn(encoder->config.vi_device,
                                 encoder->config.vi_channel);
    if (ret != SUCCESS) {
        aloge("[VENC] Enable encoding VI channel failed: ret=%d", ret);
        goto error;
    }
    encoder->vi_channel_enabled = 1;

    encoder->stop_requested = 0;
    thread_ret = pthread_create(&encoder->stream_thread,
                                NULL,
                                video_encoder_stream_thread,
                                encoder);
    if (thread_ret != 0) {
        aloge("[VENC] Create stream thread failed: ret=%d", thread_ret);
        goto error;
    }
    encoder->thread_started = 1;

    alogd("[VENC] H.264 encoder started: vipp=%d, vi_chn=%d, "
          "venc_chn=%d, %dx%d@%dfps, "
          "bitrate=%d, gop=%d, file=%s",
          encoder->config.vi_device,
          encoder->config.vi_channel,
          encoder->config.channel,
          encoder->config.width,
          encoder->config.height,
          encoder->config.frame_rate,
          encoder->config.bit_rate,
          encoder->config.gop_size,
          encoder->output_path);
    return 0;

error:
    /* stop 依据资源标志回滚已经完成的每一步。 */
    video_encoder_stop(encoder);
    return -1;
}

int video_encoder_stop(VideoEncoderContext *encoder)
{
    MPP_CHN_S vi_mpp_channel;
    MPP_CHN_S venc_mpp_channel;
    int result = 0;
    int thread_ret;
    ERRORTYPE ret;

    if (encoder == NULL) {
        return -1;
    }

    /*
     * 先禁用 VI 虚拟通道阻止新帧进入 VENC，再 join 取流线程；
     * 线程结束后才能 StopRecvPic、UnBind 和销毁通道。
     */
    encoder->stop_requested = 1;
    if (encoder->vi_channel_enabled) {
        ret = AW_MPI_VI_DisableVirChn(encoder->config.vi_device,
                                      encoder->config.vi_channel);
        if (ret != SUCCESS) {
            aloge("[VENC] Disable encoding VI channel failed: ret=%d", ret);
            result = -1;
        }
        encoder->vi_channel_enabled = 0;
    }

    if (encoder->thread_started) {
        thread_ret = pthread_join(encoder->stream_thread, NULL);
        if (thread_ret != 0) {
            aloge("[VENC] Join stream thread failed: ret=%d", thread_ret);
            result = -1;
        }
        encoder->thread_started = 0;
    }

    if (encoder->receiving) {
        ret = AW_MPI_VENC_StopRecvPic(encoder->config.channel);
        if (ret != SUCCESS) {
            aloge("[VENC] StopRecvPic failed: ret=%d", ret);
            result = -1;
        }
        encoder->receiving = 0;
    }

    if (encoder->channels_bound) {
        vi_mpp_channel.mModId = MOD_ID_VIU;
        vi_mpp_channel.mDevId = encoder->config.vi_device;
        vi_mpp_channel.mChnId = encoder->config.vi_channel;
        venc_mpp_channel.mModId = MOD_ID_VENC;
        venc_mpp_channel.mDevId = 0;
        venc_mpp_channel.mChnId = encoder->config.channel;
        ret = AW_MPI_SYS_UnBind(&vi_mpp_channel, &venc_mpp_channel);
        if (ret != SUCCESS) {
            aloge("[VENC] Unbind VI from VENC failed: ret=%d", ret);
            result = -1;
        }
        encoder->channels_bound = 0;
    }

    if (encoder->channel_created) {
        ret = AW_MPI_VENC_ResetChn(encoder->config.channel);
        if (ret != SUCCESS) {
            aloge("[VENC] Reset channel failed: ret=%d", ret);
            result = -1;
        }
        ret = AW_MPI_VENC_DestroyChn(encoder->config.channel);
        if (ret != SUCCESS) {
            aloge("[VENC] Destroy channel failed: ret=%d", ret);
            result = -1;
        }
        encoder->channel_created = 0;
    }

    if (encoder->vi_channel_created) {
        ret = AW_MPI_VI_DestroyVirChn(encoder->config.vi_device,
                                      encoder->config.vi_channel);
        if (ret != SUCCESS) {
            aloge("[VENC] Destroy encoding VI channel failed: ret=%d", ret);
            result = -1;
        }
        encoder->vi_channel_created = 0;
    }

    if (encoder->vipp_enabled) {
        ret = AW_MPI_VI_DisableVipp(encoder->config.vi_device);
        if (ret != SUCCESS) {
            aloge("[VENC] Disable encoding VIPP failed: ret=%d", ret);
            result = -1;
        }
        encoder->vipp_enabled = 0;
    }

    if (encoder->isp_running) {
        ret = AW_MPI_ISP_Stop(encoder->config.isp_device);
        if (ret != SUCCESS) {
            aloge("[VENC] Stop encoding ISP failed: ret=%d", ret);
            result = -1;
        }
        encoder->isp_running = 0;
    }

    if (encoder->vipp_created) {
        ret = AW_MPI_VI_DestroyVipp(encoder->config.vi_device);
        if (ret != SUCCESS) {
            aloge("[VENC] Destroy encoding VIPP failed: ret=%d", ret);
            result = -1;
        }
        encoder->vipp_created = 0;
    }

    if (encoder->output_file != NULL) {
        fflush(encoder->output_file);
        fclose(encoder->output_file);
        encoder->output_file = NULL;
    }
    free(encoder->h264_header);
    encoder->h264_header = NULL;
    encoder->h264_header_size = 0U;

    alogd("[VENC] Encoder stopped: encoded=%llu, "
          "key=%llu, bytes=%llu",
          encoder->encoded_frames,
          encoder->key_frames,
          encoder->encoded_bytes);
    return result;
}

void video_encoder_destroy(VideoEncoderContext *encoder)
{
    if (encoder == NULL) {
        return;
    }

    /* 如调用者忘记 stop，destroy 会先执行一次安全停止。 */
    if (encoder->thread_started || encoder->receiving ||
        encoder->channel_created || encoder->output_file != NULL ||
        encoder->h264_header != NULL ||
        encoder->vipp_created || encoder->isp_running ||
        encoder->vipp_enabled || encoder->vi_channel_created ||
        encoder->vi_channel_enabled || encoder->channels_bound) {
        video_encoder_stop(encoder);
    }
    free(encoder);
}
