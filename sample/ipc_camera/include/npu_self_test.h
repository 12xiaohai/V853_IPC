#ifndef IPC_CAMERA_NPU_SELF_TEST_H
#define IPC_CAMERA_NPU_SELF_TEST_H

/*
 * 阶段9.1的NPU独立自检入口。
 *
 * input_path可以为NULL：此时使用一张黑色NV12图像，只验证模型能否加载和
 * 完成一次推理。传入文件时要求提供模型对应的320x320 NV12原始帧。
 */
int npu_self_test_run(const char *model_path,
                      const char *input_path,
                      float confidence_threshold);

#endif
