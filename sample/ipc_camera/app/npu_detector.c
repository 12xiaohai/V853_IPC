#define _POSIX_C_SOURCE 200809L

#include "npu_detector.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <awnn.h>
#include <media/mpi_isp.h>
#include <media/mpi_vi.h>
#include <media/mpi_videoformat_conversion.h>
#include <utils/plat_log.h>

#define YOLOV8_INPUT_WIDTH 320
#define YOLOV8_INPUT_HEIGHT 320
#define NPU_MAX_MODEL_DIMENSION 4096

struct NpuDetectorContext {
    NpuDetectorConfig config;
    char model_path[256];

    pthread_t thread_id;
    pthread_mutex_t result_mutex;
    volatile int stop_requested;
    int mutex_initialized;
    int thread_started;

    int vipp_created;
    int isp_running;
    int vipp_enabled;
    int channel_created;
    int channel_enabled;

    int awnn_initialized;
    Awnn_Context_t *network;
    unsigned char *input_buffer;
    size_t y_plane_size;
    size_t input_buffer_size;

    unsigned long long captured_frames;
    unsigned long long inferred_frames;
    unsigned long long skipped_frames;
    unsigned long long last_inference_pts_us;
    unsigned long long inference_failures;
    NpuDetectionSnapshot latest;
};

/* CLOCK_MONOTONIC 不受 NTP 校时影响，适合统计一次 NPU 推理的真实耗时。 */
static unsigned long long monotonic_time_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return (unsigned long long)now.tv_sec * 1000000ULL +
           (unsigned long long)now.tv_nsec / 1000ULL;
}

/*
 * 释放 VI/ISP 硬件通路。状态标志使它既能处理正常停止，也能处理启动到一半失败。
 */
static int npu_detector_destroy_vi_pipeline(NpuDetectorContext *detector)
{
    int result = 0;
    ERRORTYPE ret;

    if (detector->channel_enabled) {
        ret = AW_MPI_VI_DisableVirChn(detector->config.vi_device,
                                      detector->config.vi_channel);
        if (ret != SUCCESS) {
            aloge("[NPU] Disable VI channel failed: ret=%d", ret);
            result = -1;
        }
        detector->channel_enabled = 0;
    }

    if (detector->channel_created) {
        ret = AW_MPI_VI_DestroyVirChn(detector->config.vi_device,
                                      detector->config.vi_channel);
        if (ret != SUCCESS) {
            aloge("[NPU] Destroy VI channel failed: ret=%d", ret);
            result = -1;
        }
        detector->channel_created = 0;
    }

    if (detector->vipp_enabled) {
        ret = AW_MPI_VI_DisableVipp(detector->config.vi_device);
        if (ret != SUCCESS) {
            aloge("[NPU] Disable VIPP failed: ret=%d", ret);
            result = -1;
        }
        detector->vipp_enabled = 0;
    }

    /*
     * 与原项目一致，每次成功 AW_MPI_ISP_Run 都有一次对应 Stop。
     * MPP 内部维护同一 ISP 被多条 VIPP 通路使用时的运行状态。
     */
    if (detector->isp_running) {
        ret = AW_MPI_ISP_Stop(detector->config.isp_device);
        if (ret != SUCCESS) {
            aloge("[NPU] Stop ISP failed: ret=%d", ret);
            result = -1;
        }
        detector->isp_running = 0;
    }

    if (detector->vipp_created) {
        ret = AW_MPI_VI_DestroyVipp(detector->config.vi_device);
        if (ret != SUCCESS) {
            aloge("[NPU] Destroy VIPP failed: ret=%d", ret);
            result = -1;
        }
        detector->vipp_created = 0;
    }
    return result;
}

/* 把新结果写入共享快照。复制时间很短，不会阻塞下一帧推理。 */
static void publish_detection_result(NpuDetectorContext *detector,
                                     const YoloDetection *detections,
                                     int detection_count,
                                     unsigned long long frame_pts_us,
                                     unsigned long long inference_time_us)
{
    pthread_mutex_lock(&detector->result_mutex);
    ++detector->latest.sequence;
    detector->latest.frame_pts_us = frame_pts_us;
    detector->latest.inference_time_us = inference_time_us;
    detector->latest.detection_count = detection_count;
    if (detection_count > 0) {
        memcpy(detector->latest.detections,
               detections,
               (size_t)detection_count * sizeof(detections[0]));
    }
    pthread_mutex_unlock(&detector->result_mutex);
}

static void log_detection_result(const NpuDetectorContext *detector,
                                 const YoloDetection *detections,
                                 int detection_count,
                                 unsigned long long inference_time_us)
{
    int index;

    alogd("[NPU] Realtime inference: frame=%llu, cost=%llu us, persons=%d",
          detector->inferred_frames,
          inference_time_us,
          detection_count);
    for (index = 0; index < detection_count; ++index) {
        alogd("[NPU] Person[%d]: score=%.3f, box=(%d,%d)-(%d,%d)",
              index,
              detections[index].score,
              detections[index].xmin,
              detections[index].ymin,
              detections[index].xmax,
              detections[index].ymax);
    }
}

/*
 * 实时推理线程：
 *   1. 从专用 VIPP 8 取一帧 320x320 NV12；
 *   2. 将 Y、UV 两个平面复制到连续输入缓存；
 *   3. 立刻归还 VI 帧，避免 NPU 运行期间占住摄像头缓冲；
 *   4. 执行 AWNN 推理和 person 后处理；
 *   5. 发布线程安全结果快照。
 */
static void *npu_detector_thread(void *argument)
{
    NpuDetectorContext *detector = argument;
    unsigned int consecutive_failures = 0U;

    alogd("[NPU] Realtime detection thread started");
    while (!detector->stop_requested) {
        VIDEO_FRAME_INFO_S frame;
        unsigned char *input_planes[2];
        float **output_buffers;
        YoloDetection detections[YOLOV8_MAX_DETECTIONS];
        unsigned long long frame_pts_us;
        unsigned long long frame_interval_us;
        unsigned long long start_us;
        unsigned long long end_us;
        int detection_count;
        ERRORTYPE ret;

        memset(&frame, 0, sizeof(frame));
        ret = AW_MPI_VI_GetFrame(detector->config.vi_device,
                                 detector->config.vi_channel,
                                 &frame,
                                 detector->config.timeout_ms);
        if (ret != SUCCESS) {
            if (detector->stop_requested) {
                break;
            }
            ++consecutive_failures;
            if (consecutive_failures == 1U ||
                (consecutive_failures % 25U) == 0U) {
                alogw("[NPU] GetFrame failed: ret=%d, consecutive=%u",
                      ret,
                      consecutive_failures);
            }
            continue;
        }

        consecutive_failures = 0U;
        ++detector->captured_frames;
        frame_pts_us = (unsigned long long)frame.VFrame.mpts;

        /*
         * V853的次级VIPP在use_current_win=1时可能忽略attributes.fps，仍按传感器
         * 20 FPS出帧。这里利用原始PTS做第二层限频：未到下一个推理周期的帧立即
         * 归还，不复制、不送NPU，从而让真实推理频率符合config.frame_rate。
         */
        frame_interval_us = 1000000ULL /
                            (unsigned long long)detector->config.frame_rate;
        if (detector->last_inference_pts_us != 0U &&
            frame_pts_us > detector->last_inference_pts_us &&
            frame_pts_us - detector->last_inference_pts_us <
                frame_interval_us) {
            ret = AW_MPI_VI_ReleaseFrame(detector->config.vi_device,
                                         detector->config.vi_channel,
                                         &frame);
            if (ret != SUCCESS) {
                aloge("[NPU] Release skipped frame failed: ret=%d, frame_id=%u",
                      ret,
                      frame.mId);
            }
            ++detector->skipped_frames;
            continue;
        }
        detector->last_inference_pts_us = frame_pts_us;
        ++detector->inferred_frames;

        /* 320 已满足 V853 的 32 字节对齐，因此两平面的有效大小可直接计算。 */
        if (frame.VFrame.mpVirAddr[0] == NULL ||
            frame.VFrame.mpVirAddr[1] == NULL) {
            aloge("[NPU] VI frame has invalid NV12 planes: Y=%p, UV=%p",
                  frame.VFrame.mpVirAddr[0],
                  frame.VFrame.mpVirAddr[1]);
            AW_MPI_VI_ReleaseFrame(detector->config.vi_device,
                                   detector->config.vi_channel,
                                   &frame);
            ++detector->inference_failures;
            continue;
        }
        memcpy(detector->input_buffer,
               frame.VFrame.mpVirAddr[0],
               detector->y_plane_size);
        memcpy(detector->input_buffer + detector->y_plane_size,
               frame.VFrame.mpVirAddr[1],
               detector->y_plane_size / 2U);

        ret = AW_MPI_VI_ReleaseFrame(detector->config.vi_device,
                                     detector->config.vi_channel,
                                     &frame);
        if (ret != SUCCESS) {
            aloge("[NPU] ReleaseFrame failed: ret=%d, frame_id=%u",
                  ret,
                  frame.mId);
        }

        input_planes[0] = detector->input_buffer;
        input_planes[1] = detector->input_buffer + detector->y_plane_size;
        awnn_set_input_buffers(detector->network, input_planes);

        start_us = monotonic_time_us();
        awnn_run(detector->network);
        end_us = monotonic_time_us();
        output_buffers = awnn_get_output_buffers(detector->network);
        if (output_buffers == NULL || output_buffers[0] == NULL) {
            ++detector->inference_failures;
            aloge("[NPU] Get output tensor failed: failures=%llu",
                  detector->inference_failures);
            continue;
        }

        memset(detections, 0, sizeof(detections));
        detection_count = yolov8_decode_person(
            output_buffers[0],
            detector->config.width,
            detector->config.height,
            detector->config.confidence_threshold,
            detector->config.nms_threshold,
            detections,
            YOLOV8_MAX_DETECTIONS);
        if (detection_count < 0) {
            ++detector->inference_failures;
            aloge("[NPU] YOLOv8 postprocess failed: failures=%llu",
                  detector->inference_failures);
            continue;
        }

        publish_detection_result(detector,
                                 detections,
                                 detection_count,
                                 frame_pts_us,
                                 end_us >= start_us ? end_us - start_us : 0U);

        /* 第一帧必打日志，之后按配置间隔输出，避免 10 fps 持续刷屏。 */
        if (detector->inferred_frames == 1U ||
            (detector->config.log_interval_frames > 0U &&
             detector->inferred_frames %
                     detector->config.log_interval_frames ==
                 0U)) {
            log_detection_result(detector,
                                 detections,
                                 detection_count,
                                 end_us >= start_us ? end_us - start_us : 0U);
        }
    }

    alogd("[NPU] Realtime detection thread stopped: received=%llu, "
          "inferred=%llu, skipped=%llu, failures=%llu",
          detector->captured_frames,
          detector->inferred_frames,
          detector->skipped_frames,
          detector->inference_failures);
    return NULL;
}

NpuDetectorContext *npu_detector_create(const NpuDetectorConfig *config)
{
    NpuDetectorContext *detector;

    if (config == NULL || config->model_path == NULL ||
        config->model_path[0] == '\0' || config->width <= 0 ||
        config->height <= 0 || config->frame_rate <= 0 ||
        config->buffer_count < 2 || config->timeout_ms < 0 ||
        config->confidence_threshold <= 0.0f ||
        config->confidence_threshold >= 1.0f ||
        config->nms_threshold <= 0.0f || config->nms_threshold >= 1.0f) {
        return NULL;
    }

    detector = calloc(1, sizeof(*detector));
    if (detector == NULL) {
        return NULL;
    }
    detector->config = *config;
    snprintf(detector->model_path,
             sizeof(detector->model_path),
             "%s",
             config->model_path);
    detector->config.model_path = detector->model_path;
    if (pthread_mutex_init(&detector->result_mutex, NULL) != 0) {
        free(detector);
        return NULL;
    }
    detector->mutex_initialized = 1;
    return detector;
}

int npu_detector_start(NpuDetectorContext *detector)
{
    awnn_info_t *model_info;
    VI_ATTR_S attributes;
    ERRORTYPE ret;
    int thread_ret;

    if (detector == NULL || detector->thread_started ||
        detector->network != NULL) {
        return -1;
    }
    if (detector->config.width != YOLOV8_INPUT_WIDTH ||
        detector->config.height != YOLOV8_INPUT_HEIGHT) {
        aloge("[NPU] Realtime detector requires %dx%d NV12, got=%dx%d",
              YOLOV8_INPUT_WIDTH,
              YOLOV8_INPUT_HEIGHT,
              detector->config.width,
              detector->config.height);
        return -1;
    }

    /* 先校验模型输入和 NPU 内存需求，模型不匹配时不创建任何 VI 资源。 */
    model_info = awnn_get_info(detector->model_path);
    if (model_info == NULL || model_info->width != detector->config.width ||
        model_info->height != detector->config.height ||
        model_info->width > NPU_MAX_MODEL_DIMENSION ||
        model_info->height > NPU_MAX_MODEL_DIMENSION ||
        model_info->mem_size == 0U) {
        aloge("[NPU] Invalid or incompatible model: %s", detector->model_path);
        return -1;
    }

    detector->y_plane_size = (size_t)detector->config.width *
                             (size_t)detector->config.height;
    detector->input_buffer_size = detector->y_plane_size * 3U / 2U;
    detector->input_buffer = malloc(detector->input_buffer_size);
    if (detector->input_buffer == NULL) {
        aloge("[NPU] Allocate realtime input buffer failed: %zu bytes",
              detector->input_buffer_size);
        return -1;
    }

    awnn_init(model_info->mem_size);
    detector->awnn_initialized = 1;
    detector->network = awnn_create(detector->model_path);
    if (detector->network == NULL) {
        aloge("[NPU] Create realtime network failed: %s", detector->model_path);
        goto error;
    }

    /* 使用原项目参数：VIPP 8、NV12、多平面 MMAP、3 个采集缓冲。 */
    memset(&attributes, 0, sizeof(attributes));
    attributes.mOnlineEnable = 0;
    attributes.mOnlineShareBufNum = 0;
    attributes.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    attributes.memtype = V4L2_MEMORY_MMAP;
    attributes.format.pixelformat = map_PIXEL_FORMAT_E_to_V4L2_PIX_FMT(
        MM_PIXEL_FORMAT_YUV_SEMIPLANAR_420);
    attributes.format.field = V4L2_FIELD_NONE;
    attributes.format.colorspace = V4L2_COLORSPACE_JPEG;
    attributes.format.width = (unsigned int)detector->config.width;
    attributes.format.height = (unsigned int)detector->config.height;
    attributes.fps = (unsigned int)detector->config.frame_rate;
    attributes.capturemode = V4L2_MODE_VIDEO;
    attributes.use_current_win = detector->config.vi_device > 0 ? 1 : 0;
    attributes.nbufs = (unsigned int)detector->config.buffer_count;
    attributes.nplanes = 2;
    attributes.drop_frame_num = 0;

    ret = AW_MPI_VI_CreateVipp(detector->config.vi_device);
    if (ret != SUCCESS) {
        aloge("[NPU] Create VIPP failed: ret=%d", ret);
        goto error;
    }
    detector->vipp_created = 1;

    ret = AW_MPI_VI_SetVippAttr(detector->config.vi_device, &attributes);
    if (ret != SUCCESS) {
        aloge("[NPU] Set VIPP attributes failed: ret=%d", ret);
        goto error;
    }
    ret = AW_MPI_ISP_Run(detector->config.isp_device);
    if (ret != SUCCESS) {
        aloge("[NPU] Start ISP failed: ret=%d", ret);
        goto error;
    }
    detector->isp_running = 1;

    ret = AW_MPI_VI_EnableVipp(detector->config.vi_device);
    if (ret != SUCCESS) {
        aloge("[NPU] Enable VIPP failed: ret=%d", ret);
        goto error;
    }
    detector->vipp_enabled = 1;

    ret = AW_MPI_VI_CreateVirChn(detector->config.vi_device,
                                 detector->config.vi_channel,
                                 NULL);
    if (ret != SUCCESS) {
        aloge("[NPU] Create VI channel failed: ret=%d", ret);
        goto error;
    }
    detector->channel_created = 1;

    ret = AW_MPI_VI_EnableVirChn(detector->config.vi_device,
                                 detector->config.vi_channel);
    if (ret != SUCCESS) {
        aloge("[NPU] Enable VI channel failed: ret=%d", ret);
        goto error;
    }
    detector->channel_enabled = 1;

    detector->stop_requested = 0;
    detector->captured_frames = 0U;
    detector->inferred_frames = 0U;
    detector->skipped_frames = 0U;
    detector->last_inference_pts_us = 0U;
    detector->inference_failures = 0U;
    memset(&detector->latest, 0, sizeof(detector->latest));
    thread_ret = pthread_create(&detector->thread_id,
                                NULL,
                                npu_detector_thread,
                                detector);
    if (thread_ret != 0) {
        aloge("[NPU] Create realtime thread failed: ret=%d", thread_ret);
        goto error;
    }
    detector->thread_started = 1;

    alogd("[NPU] Realtime detector started: model=%s, vipp=%d, chn=%d, "
          "%dx%d@%dfps, threshold=%.2f, nms=%.2f",
          detector->model_path,
          detector->config.vi_device,
          detector->config.vi_channel,
          detector->config.width,
          detector->config.height,
          detector->config.frame_rate,
          detector->config.confidence_threshold,
          detector->config.nms_threshold);
    return 0;

error:
    npu_detector_stop(detector);
    return -1;
}

int npu_detector_get_latest(NpuDetectorContext *detector,
                            NpuDetectionSnapshot *snapshot)
{
    if (detector == NULL || snapshot == NULL ||
        !detector->mutex_initialized) {
        return -1;
    }
    pthread_mutex_lock(&detector->result_mutex);
    *snapshot = detector->latest;
    pthread_mutex_unlock(&detector->result_mutex);
    return 0;
}

int npu_detector_stop(NpuDetectorContext *detector)
{
    int result = 0;
    int thread_ret;

    if (detector == NULL) {
        return -1;
    }

    detector->stop_requested = 1;
    if (detector->thread_started) {
        thread_ret = pthread_join(detector->thread_id, NULL);
        if (thread_ret != 0) {
            aloge("[NPU] Join realtime thread failed: ret=%d", thread_ret);
            result = -1;
        }
        detector->thread_started = 0;
    }
    if (npu_detector_destroy_vi_pipeline(detector) != 0) {
        result = -1;
    }

    if (detector->network != NULL) {
        awnn_destroy(detector->network);
        detector->network = NULL;
    }
    if (detector->awnn_initialized) {
        awnn_uninit();
        detector->awnn_initialized = 0;
    }
    free(detector->input_buffer);
    detector->input_buffer = NULL;
    detector->input_buffer_size = 0U;
    detector->y_plane_size = 0U;

    alogd("[NPU] Realtime detector stopped");
    return result;
}

void npu_detector_destroy(NpuDetectorContext *detector)
{
    if (detector == NULL) {
        return;
    }
    /* 正常路径通常已显式 stop；只有仍持有资源时才再次执行兜底停止。 */
    if (detector->thread_started || detector->channel_enabled ||
        detector->channel_created || detector->vipp_enabled ||
        detector->isp_running || detector->vipp_created ||
        detector->network != NULL || detector->awnn_initialized ||
        detector->input_buffer != NULL) {
        npu_detector_stop(detector);
    }
    if (detector->mutex_initialized) {
        pthread_mutex_destroy(&detector->result_mutex);
    }
    free(detector);
}
