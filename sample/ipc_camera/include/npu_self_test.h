#ifndef IPC_CAMERA_NPU_SELF_TEST_H
#define IPC_CAMERA_NPU_SELF_TEST_H

/*
 * 阶段9.1的NPU独立自检入口。
 *
 * input_path可以为NULL：此时使用一张黑色NV21图像，只验证模型能否加载和
 * 完成一次推理。传入文件时支持紧凑的320x180 NV21，以及高度按32对齐后的
 * 320x192 NV21；实际尺寸以模型信息为准。
 */
int npu_self_test_run(const char *model_path,
                      const char *input_path,
                      float confidence_threshold);

#endif
