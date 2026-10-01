#define _POSIX_C_SOURCE 200809L

#include "npu_self_test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <awnn.h>
#include <utils/plat_log.h>

#define NPU_ALIGNMENT 32U
#define NPU_MAX_DIMENSION 4096
#define NPU_MAX_RESULTS 100

/* 将模型要求的图像高度向上对齐到V853 NPU使用的32像素边界。 */
static size_t align_up_32(size_t value)
{
    return (value + NPU_ALIGNMENT - 1U) & ~(NPU_ALIGNMENT - 1U);
}

/* CLOCK_MONOTONIC不受NTP校时影响，适合测量一次推理的真实耗时。 */
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
 * 读取一张NV21测试图。文件可以包含紧凑可见高度，也可以已经带有32行对齐。
 * 紧凑文件会逐平面放入对齐缓冲，补齐区域保持黑色，不能直接整体memcpy，
 * 否则UV平面会落在错误偏移处。
 */
static int load_nv21_input(const char *path,
                           unsigned char *buffer,
                           size_t width,
                           size_t visible_height,
                           size_t aligned_height)
{
    FILE *file;
    long file_size;
    size_t tight_y_size = width * visible_height;
    size_t tight_uv_size = tight_y_size / 2U;
    size_t tight_size = tight_y_size + tight_uv_size;
    size_t aligned_y_size = width * aligned_height;
    size_t aligned_size = aligned_y_size * 3U / 2U;
    size_t bytes_read;
    size_t y_bytes_read;
    size_t uv_bytes_read;

    file = fopen(path, "rb");
    if (file == NULL) {
        aloge("[NPU] Open NV21 input failed: file=%s, errno=%d", path, errno);
        return -1;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        aloge("[NPU] Seek NV21 input failed: file=%s, errno=%d", path, errno);
        fclose(file);
        return -1;
    }
    file_size = ftell(file);
    if (file_size < 0L || fseek(file, 0L, SEEK_SET) != 0) {
        aloge("[NPU] Query NV21 input size failed: file=%s, errno=%d",
              path,
              errno);
        fclose(file);
        return -1;
    }

    if ((size_t)file_size == aligned_size) {
        bytes_read = fread(buffer, 1U, aligned_size, file);
        fclose(file);
        if (bytes_read != aligned_size) {
            aloge("[NPU] Read aligned NV21 input failed: expected=%zu, got=%zu",
                  aligned_size,
                  bytes_read);
            return -1;
        }
        alogd("[NPU] Loaded aligned NV21 input: %s (%zu bytes)",
              path,
              aligned_size);
        return 0;
    }

    if ((size_t)file_size == tight_size) {
        y_bytes_read = fread(buffer, 1U, tight_y_size, file);
        uv_bytes_read = y_bytes_read == tight_y_size
                            ? fread(buffer + aligned_y_size,
                                    1U,
                                    tight_uv_size,
                                    file)
                            : 0U;
        fclose(file);
        if (y_bytes_read != tight_y_size || uv_bytes_read != tight_uv_size) {
            aloge("[NPU] Read compact NV21 input failed: file=%s, "
                  "Y=%zu/%zu, VU=%zu/%zu",
                  path,
                  y_bytes_read,
                  tight_y_size,
                  uv_bytes_read,
                  tight_uv_size);
            return -1;
        }
        alogd("[NPU] Loaded compact NV21 input: %s (%zu bytes -> %zu bytes)",
              path,
              tight_size,
              aligned_size);
        return 0;
    }

    fclose(file);
    aloge("[NPU] Invalid NV21 file size: file=%s, got=%ld, expected=%zu or %zu",
          path,
          file_size,
          tight_size,
          aligned_size);
    return -1;
}

int npu_self_test_run(const char *model_path,
                      const char *input_path,
                      float confidence_threshold)
{
    awnn_info_t *model_info;
    Awnn_Context_t *network = NULL;
    Awnn_Post_t post;
    Awnn_Result_t result;
    unsigned char *input = NULL;
    unsigned char *input_planes[2] = {NULL, NULL};
    size_t width;
    size_t visible_height;
    size_t aligned_height;
    size_t y_size;
    size_t input_size;
    unsigned long long start_us;
    unsigned long long end_us;
    int awnn_initialized = 0;
    int index;
    int status = -1;

    if (model_path == NULL || model_path[0] == '\0') {
        aloge("[NPU] Model path is empty");
        return -1;
    }

    /* 先读取.nb附带的信息，得到输入尺寸、内存需求和推荐阈值。 */
    model_info = awnn_get_info((char *)model_path);
    if (model_info == NULL) {
        aloge("[NPU] Read model information failed: %s", model_path);
        return -1;
    }
    if (model_info->width <= 0 || model_info->height <= 0 ||
        (model_info->width & 1) != 0 || (model_info->height & 1) != 0 ||
        model_info->width > NPU_MAX_DIMENSION ||
        model_info->height > NPU_MAX_DIMENSION ||
        model_info->mem_size == 0U) {
        aloge("[NPU] Invalid model information: width=%d, height=%d, memory=%u",
              model_info->width,
              model_info->height,
              model_info->mem_size);
        return -1;
    }

    width = (size_t)model_info->width;
    visible_height = (size_t)model_info->height;
    aligned_height = align_up_32(visible_height);
    y_size = width * aligned_height;
    input_size = y_size * 3U / 2U;

    alogd("[NPU] Model: name=%s, md5=%s, input=%zux%zu, aligned=%zux%zu, "
          "memory=%u, recommended_threshold=%.3f",
          model_info->name != NULL ? model_info->name : "unknown",
          model_info->md5,
          width,
          visible_height,
          width,
          aligned_height,
          model_info->mem_size,
          model_info->thresh);

    input = malloc(input_size);
    if (input == NULL) {
        aloge("[NPU] Allocate input buffer failed: %zu bytes", input_size);
        return -1;
    }

    /* NV21黑色：Y=16，交错VU=128。对齐补边也使用相同的黑色值。 */
    memset(input, 16, y_size);
    memset(input + y_size, 128, input_size - y_size);
    if (input_path != NULL && input_path[0] != '\0') {
        if (load_nv21_input(input_path,
                            input,
                            width,
                            visible_height,
                            aligned_height) != 0) {
            goto cleanup;
        }
    } else {
        alogw("[NPU] No input file supplied; using a black NV21 frame");
    }

    /* AWNN在一个进程中只初始化一次；阶段9.1自检结束后立即反初始化。 */
    awnn_init(model_info->mem_size);
    awnn_initialized = 1;
    network = awnn_create((char *)model_path);
    if (network == NULL) {
        aloge("[NPU] Create network failed: %s", model_path);
        goto cleanup;
    }

    input_planes[0] = input;
    input_planes[1] = input + y_size;
    awnn_set_input_buffers(network, input_planes);

    start_us = monotonic_time_us();
    awnn_run(network);
    end_us = monotonic_time_us();

    memset(&post, 0, sizeof(post));
    post.type = AWNN_DET_POST_HUMANOID_1;
    post.width = (int)width;
    post.height = (int)visible_height;
    post.thresh = confidence_threshold > 0.0f
                      ? confidence_threshold
                      : model_info->thresh;
    memset(&result, 0, sizeof(result));
    awnn_det_post(network, &post, &result);
    if (result.valid_cnt < 0 || result.valid_cnt > NPU_MAX_RESULTS) {
        aloge("[NPU] Invalid result count returned by postprocess: %d",
              result.valid_cnt);
        goto cleanup;
    }

    alogd("[NPU] Single-frame inference succeeded: cost=%llu us, objects=%d, "
          "threshold=%.3f",
          end_us >= start_us ? end_us - start_us : 0U,
          result.valid_cnt,
          post.thresh);
    for (index = 0; index < result.valid_cnt; ++index) {
        alogd("[NPU] Result[%d]: label=%d, score=%.3f, box=(%d,%d)-(%d,%d)",
              index,
              result.boxes[index].label,
              result.boxes[index].score,
              result.boxes[index].xmin,
              result.boxes[index].ymin,
              result.boxes[index].xmax,
              result.boxes[index].ymax);
    }
    status = 0;

cleanup:
    if (network != NULL) {
        awnn_destroy(network);
    }
    if (awnn_initialized) {
        awnn_uninit();
    }
    free(input);
    return status;
}
