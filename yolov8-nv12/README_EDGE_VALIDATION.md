# YOLOv8 V853端侧部署验证说明

## 1. 目录用途

本目录保存YOLOv8n从ONNX到V853 NBG模型的转换产物、量化数据、测试图片以及
Acuity生成的OVXLIB参考工程。当前在独立分支
`feature/yolov8-edge-validation`中验证，验证通过后再合并到`main`。

## 2. 已确认的模型一致性

转换产物：

```text
ovxilb/yolov8n-sim_nbg_unify/network_binary.nb
```

项目实际部署模型：

```text
../models/yolov8n.nb
```

两者MD5完全一致：

```text
d9cca461b73f577ddade48edd207eccd
```

开发板自检日志中的模型MD5也相同，因此当前`/lib/yolov8n.nb`就是本目录生成的
网络，不存在“转换模型和实际运行模型不是同一个”的问题。

在Linux虚拟机中可重复检查：

```sh
md5sum \
  yolov8-nv12/ovxilb/yolov8n-sim_nbg_unify/network_binary.nb \
  models/yolov8n.nb
```

## 3. 模型输入格式为NV12

转换配置`yolov8n-sim_inputmeta.yml`明确指定：

```text
preproc_type: IMAGE_NV12
```

`nbg_meta.json`也显示网络有两个UINT8输入平面：

```text
Y：  [1, 1, 320, 320]
UV： [1, 1, 160, 320]
```

因此真实彩色测试帧必须使用NV12（Y + UV），不能直接把NV21（Y + VU）交给
模型。黑帧中U和V都为128，所以此前黑帧测试无法暴露顺序差异。

后续实时VI通路应配置：

```c
MM_PIXEL_FORMAT_YUV_SEMIPLANAR_420
```

而不是预览通路使用的：

```c
MM_PIXEL_FORMAT_YVU_SEMIPLANAR_420
```

## 4. 模型输入输出

输入：

```text
320x320 NV12，共153600字节
```

AWNN提供给应用层的输出：

```text
[1, 84, 2100] float32指针
```

NBG内部输出带有UINT8量化信息，AWNN接口通过`float **`向当前应用提供可供后处理
读取的输出缓冲。其中前4个属性是框坐标，后80个属性是COCO类别概率，类别0为person。项目中的
`yolov8_postprocess.c`直接解析该布局，并以0.25置信度和0.45 IoU执行NMS。

## 5. 为什么不直接编译生成的OVXLIB工程

`ovxilb/*/makefile.linux`依赖Acuity/OVXLIB开发环境，包括：

```text
vsi_nn_pub.h
libovxlib
libOpenVX
libOpenVXU
libCLC
libVSC
libGAL
```

当前`sample_demo/sdk`和板端rootfs没有提供这套完整开发依赖，所以生成的
OVXLIB示例不能在本项目的独立编译环境中直接构建。它仍然有价值：用于核对
模型输入输出、量化信息和转换版本。

当前端侧部署采用已经在V853上验证成功的AWNN封装：

```text
awnn_get_info -> awnn_init -> awnn_create -> awnn_run
              -> awnn_get_output_buffers -> 自定义YOLOv8后处理
```

这条路线加载的是同一个NBG文件，不会改变模型计算结果。

## 6. 真实图片验证

使用目录内的测试图片生成320x320 NV12：

```sh
ffmpeg -i yolov8-nv12/data/test01.jpg \
  -vf scale=320:320 -frames:v 1 -pix_fmt nv12 -f rawvideo \
  npu_test_320x320.nv12
```

确认大小：

```sh
stat -c '%n %s bytes' npu_test_320x320.nv12
```

应为：

```text
153600 bytes
```

复制到开发板后运行：

```sh
./sample_strip --npu-self-test \
  /lib/yolov8n.nb \
  /mnt/UDISK/npu_test_320x320.nv12
```

含人图片应输出：

```text
[NPU] Result[0]: label=0, score=..., box=(...)-(...)
```

## 7. 验证结果与合并结论

以下合并条件均已通过开发板验证：

1. 黑帧推理成功并以状态码0退出；
2. `test01.jpg`和`test02.jpg`生成的NV12均能正常推理；
3. 含人图片至少输出一个合理的person框；
4. 空场景不产生大量误报；
5. 两个模型文件MD5仍保持一致；
6. 普通模式的显示、RTSP和录像功能没有回归问题。

阶段9.1的模型完整性、真实图片检测、重复运行和普通监控模式回归验证已完成。
推理转储和IDE元数据已经清理，最终干净快照已通过squash方式合并到`main`。

## 8. 仓库保留策略

仓库保留ONNX、Acuity转换配置、校准图片、转换数据、生成的OVXLIB参考源码、
`nbg_meta.json`、`network_binary.nb`以及最终部署的`models/yolov8n.nb`。

以下文件属于可重复生成的中间产物，不进入主分支：

```text
iter_*.tensor
.project
.cproject
*.vcxproj
```

其中`iter_*.tensor`是Acuity验证时由`dump_results`产生的输入/输出张量转储；
其余文件是Eclipse或Visual Studio工程元数据，不参与当前项目构建和板端运行。
