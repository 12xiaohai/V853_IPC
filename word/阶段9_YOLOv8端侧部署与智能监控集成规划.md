# 阶段9：YOLOv8端侧部署与智能监控集成规划

## 1. 文档目的

本文档说明第九阶段的完整实现路线，包括：

1. `yolov8-nv12`目录中的模型转换工程是怎样工作的；
2. `models/yolov8n.nb`与转换工程之间的关系；
3. V853板端如何通过AWNN和NPU执行YOLOv8；
4. 为什么主视频链路使用NV21，而NPU模型输入使用NV12；
5. 如何通过独立Git分支验证端侧部署；
6. 验证达到什么程度后才允许合并到`main`；
7. 后续实时人形检测、越线检测、区域入侵和音频报警如何分阶段实现。

本阶段采用“先证明模型可运行，再接实时视频，最后增加业务规则”的顺序，避免
摄像头、图像格式、NPU、后处理和报警逻辑同时出现问题时难以定位。

## 2. 第九阶段总体拆分

第九阶段建议拆分为以下七个小阶段：

| 小阶段 | 功能 | 开发位置 |
| --- | --- | --- |
| 9.1 | YOLOv8模型工程归档、NPU环境和单帧推理 | `feature/yolov8-edge-validation` |
| 9.2 | NPU专用VI通路和实时人形检测线程 | `main` |
| 9.3 | 检测结果共享、坐标映射和检测框显示 | `main` |
| 9.4 | 越线检测、方向判断和重复报警抑制 | `main` |
| 9.5 | 多边形区域入侵检测和状态机 | `main` |
| 9.6 | 本地WAV音频报警、事件队列和冷却时间 | `main` |
| 9.7 | 全链路联调、压力测试和最终文档 | `main` |

本项目只为“YOLOv8模型转换产物和端侧部署验证”建立一个隔离分支。原因是模型
工程体积较大，而且模型格式、NPU运行环境和真实图片推理都需要先单独确认。
阶段9.1验收通过后，将`feature/yolov8-edge-validation`合并回`main`；后面的
9.2至9.7继续在`main`上按阶段实现、验证和提交，不再为每项功能建立新分支。

## 3. `yolov8-nv12`目录结构与作用

该目录不是摄像头业务代码，而是YOLOv8模型在V853上的离线转换产物和厂商生成
工程。主要文件如下：

```text
yolov8-nv12/
|-- yolov8n-sim.onnx
|-- yolov8n-sim.data
|-- yolov8n-sim.quantize
|-- yolov8n-sim_inputmeta.yml
|-- dataset.txt
|-- data/
|   |-- test01.jpg
|   `-- test02.jpg
`-- ovxilb/
    |-- yolov8n-sim/
    `-- yolov8n-sim_nbg_unify/
        |-- network_binary.nb
        |-- nbg_meta.json
        |-- main.c
        |-- vnn_pre_process.c
        |-- vnn_post_process.c
        `-- vnn_yolov8nsimprj.c
```

各文件的职责如下：

- `yolov8n-sim.onnx`：简化后的YOLOv8n ONNX网络；
- `dataset.txt`和`data/`：模型量化时使用的校准图片列表和样例图片；
- `yolov8n-sim.quantize`：量化参数和张量量化信息；
- `yolov8n-sim_inputmeta.yml`：输入尺寸、归一化和NV12硬件预处理配置；
- `yolov8n-sim.data`：Acuity转换过程产生的网络数据；
- `yolov8n-sim/`：包含完整网络结构的OVXLIB生成工程；
- `yolov8n-sim_nbg_unify/`：加载已编译NBG模型的精简生成工程；
- `network_binary.nb`：V853 NPU最终执行的模型二进制；
- `nbg_meta.json`：NBG输入、输出、数据类型和形状说明。

`ovxilb`目录下的代码主要用于说明模型图如何创建、如何绑定输入输出以及如何
调用OVXLIB。当前`sample_demo`没有直接编译这些生成代码，因为它们依赖完整的
OVXLIB、OpenVX和Vivante开发库。项目运行时采用Tina SDK已经封装好的AWNN接口。

## 4. 模型离线转换过程

模型转换发生在开发机上，不在V853运行时完成。整体过程为：

```text
YOLOv8n训练权重
    -> 导出ONNX
    -> 简化为yolov8n-sim.onnx
    -> 使用样例图片进行量化校准
    -> 加入320x320 NV12硬件预处理节点
    -> 编译为V853 NPU可执行的network_binary.nb
    -> 复制为models/yolov8n.nb
    -> 制作固件时安装为/lib/yolov8n.nb
```

输入配置中的关键内容是：

```yaml
shape:
- 1
- 3
- 320
- 320

scale: 0.0039

preproc_node_params:
  add_preproc_node: true
  preproc_type: IMAGE_NV12
  preproc_image_size:
  - 320
  - 320
```

原始YOLOv8网络接收`1x3x320x320`的RGB浮点张量。转换时在网络前加入NV12
预处理节点后，板端应用只需要提供两个`uint8`输入平面：

```text
输入0：Y平面，形状[1, 1, 320, 320]
输入1：UV平面，形状[1, 1, 160, 320]
```

模型内部预处理节点负责完成：

```text
NV12 -> RGB/通道调整 -> 乘0.0039归一化 -> YOLOv8网络
```

这样可以避免ARM CPU逐像素执行YUV转RGB、HWC/NCHW重排和浮点归一化。

## 5. 模型文件的一致性

以下两个文件应当完全一致：

```text
yolov8-nv12/ovxilb/yolov8n-sim_nbg_unify/network_binary.nb
models/yolov8n.nb
```

当前已验证MD5均为：

```text
d9cca461b73f577ddade48edd207eccd
```

Linux下可使用：

```sh
md5sum \
  yolov8-nv12/ovxilb/yolov8n-sim_nbg_unify/network_binary.nb \
  models/yolov8n.nb
```

两个MD5相同，才能证明项目使用的`.nb`确实来自当前保存的转换工程。固件部署后
还应检查：

```sh
md5sum /lib/yolov8n.nb
```

## 6. NV21主链路与NV12模型输入

项目中两种格式同时存在，但用途不同：

| 通路 | 分辨率 | 格式 | 用途 |
| --- | --- | --- | --- |
| 主视频通路 | 1920x1080 | NV21 | LCD、H.264、RTSP、MP4 |
| NPU通路 | 320x320 | NV12 | YOLOv8推理 |

数据排列分别为：

```text
NV21：Y平面 + VUVUVU交错平面
NV12：Y平面 + UVUVUV交错平面
```

黑帧中U和V都等于128，因此黑帧无法验证UV顺序。真实彩色NV21帧如果不转换就
直接交给NV12模型，会导致输入颜色错误，降低检测准确率。

实时检测阶段应优先沿用原项目方式：创建独立的AI用VI/VIPP通路，让硬件直接
输出`320x320 NV12`。主视频通路继续保持NV21，不需要为了NPU修改LCD、编码、
RTSP和录像链路。

## 7. 阶段9.1板端实现

阶段9.1通过以下命令进入独立自检模式：

```sh
./sample_strip --npu-self-test [model.nb] [input.nv12]
```

该模式不会启动摄像头、LCD、编码、RTSP、音频和录像，只执行以下流程：

```text
awnn_get_info
    -> 校验模型尺寸和内存需求
    -> 分配320x320 NV12输入缓冲
    -> awnn_init
    -> awnn_create
    -> awnn_set_input_buffers(Y, UV)
    -> awnn_run
    -> awnn_get_output_buffers
    -> YOLOv8 person后处理
    -> NMS
    -> 打印检测框和耗时
    -> awnn_destroy
    -> awnn_uninit
```

模型输出固定为：

```text
[1, 84, 2100]
```

- 4个通道为`center_x、center_y、width、height`；
- 80个通道为COCO类别分数；
- 2100个候选来自`40x40 + 20x20 + 10x10`；
- 当前业务只保留COCO类别0，即`person`；
- 置信度阈值为0.25；
- NMS IoU阈值为0.45；
- 最多保留100个检测框。

当前黑帧测试已经验证：

```text
模型加载：成功
NPU执行：成功
推理耗时：约27.9 ms
黑帧目标数：0
退出码：0
```

黑帧结果只能证明模型和NPU链路可运行，不能代替真实人形检测验证。

## 8. YOLOv8端侧部署技术说明

### 8.1 什么叫端侧部署

本项目中的“端侧部署”不是把ONNX文件直接复制到开发板，也不是在V853上训练
或转换模型，而是把以下四层组合成一条可以在设备本地独立运行的链路：

```text
模型层：    yolov8n.nb
应用层：    sample/sample_strip + AWNN调用代码 + YOLOv8后处理
运行时层：  AWNN、VIPLite用户态库和NPU内核驱动
输入层：    320x320 NV12图像或实时VI帧
```

V853不直接执行`yolov8n-sim.onnx`。ONNX必须先在开发机上通过Acuity转换成
适配V853 NPU的`.nb`文件，开发板只负责加载`.nb`并执行推理。

### 8.2 当前项目选择的部署接口

`yolov8-nv12/ovxilb`中的代码是Acuity自动生成的OVXLIB参考工程，但当前独立
SDK没有提供直接重新编译该工程所需的完整OpenVX/OVXLIB开发包。因此项目没有
把生成工程的`main.c`作为最终应用入口，而是采用Tina中已经封装好的AWNN接口：

```text
sample_demo业务代码
    -> libawnn
    -> libVIPlite/libVIPuser
    -> V853 NPU内核驱动
    -> NPU硬件
```

生成工程仍然保留在仓库中，用于追溯模型转换参数、输入输出布局和原始NBG产物。
真正参与产品程序编译的是：

```text
sample/ipc_camera/app/npu_self_test.c
sample/ipc_camera/app/yolov8_postprocess.c
models/yolov8n.nb
```

### 8.3 编译时如何接入NPU

NPU头文件来自：

```text
sdk/aw_pack_src/lib_aw/include/viplite-driver/
```

业务代码通过：

```c
#include <awnn.h>
```

取得AWNN接口。Makefile会链接随项目保存的NPU库，关键链接项为：

```text
-lVIPlite
-lawnn
-lVIPuser
```

V853交叉编译目标为ARMv7-A、Cortex-A7、NEON和hard-float ABI。执行：

```sh
cd sample_demo
./build.sh
```

将生成：

```text
output/sample
output/sample_strip
```

其中`sample_strip`已经去除调试符号，更适合复制到开发板。可以在虚拟机检查：

```sh
file output/sample_strip
readelf -h output/sample_strip | grep -E 'Class|Machine'
readelf -d output/sample_strip | grep NEEDED
```

预期架构应为32位ARM，而不是虚拟机的x86-64。

### 8.4 部署方式一：UDISK快速验证

该方式不需要重新烧写固件，适合验证feature分支。先在Linux虚拟机编译：

```sh
git switch feature/yolov8-edge-validation
git pull --ff-only
./build.sh
```

准备以下文件：

```text
output/sample_strip
models/yolov8n.nb
npu_test_320x320.nv12
```

如果尚未生成真实测试帧，可执行：

```sh
ffmpeg -i yolov8-nv12/data/test01.jpg \
    -vf scale=320:320 -frames:v 1 \
    -pix_fmt nv12 -f rawvideo \
    npu_test_320x320.nv12

stat -c '%n %s bytes' npu_test_320x320.nv12
```

文件大小必须为：

```text
153600 bytes
```

通过U盘、共享目录或`scp`把三个文件放到开发板`/mnt/UDISK`，然后执行：

```sh
cd /mnt/UDISK
chmod +x sample_strip

md5sum yolov8n.nb
./sample_strip --npu-self-test \
    /mnt/UDISK/yolov8n.nb \
    /mnt/UDISK/npu_test_320x320.nv12

echo $?
```

这种调用显式指定UDISK模型，不会使用固件中可能存在的旧模型。返回值应为0，
并出现`Single-frame inference succeeded`。含人图片还应至少输出一个合理的
`Result[...]`。

如果只验证驱动和模型加载，不传输入文件即可使用程序生成的黑帧：

```sh
./sample_strip --npu-self-test /mnt/UDISK/yolov8n.nb
```

### 8.5 部署方式二：集成到正式固件

真实产品部署应把应用和模型放入rootfs，避免每次开机从UDISK手工启动。执行：

```sh
./build.sh firmware
```

构建过程为：

```text
1. 交叉编译并生成output/sample_strip
2. 解压sdk/aw_pack_src/rootfs/rootfs.tar.gz
3. 把sample_strip复制为rootfs中的/usr/bin/sample
4. 把models/yolov8n.nb复制到rootfs中的/lib/yolov8n.nb
5. 重新生成rootfs.fex
6. 调用aw_pack.sh生成可烧录镜像
7. 输出output/tina_ipc_uart0.img
```

对应自动安装代码位于`mk_firmware/build_part.sh`：

```text
output/sample_strip -> /usr/bin/sample
models/yolov8n.nb  -> /lib/yolov8n.nb
```

烧写`output/tina_ipc_uart0.img`并重启后检查：

```sh
ls -lh /usr/bin/sample
ls -lh /lib/yolov8n.nb
md5sum /lib/yolov8n.nb
/usr/bin/sample --npu-self-test
echo $?
```

程序默认模型路径就是`/lib/yolov8n.nb`，因此正式固件中不需要额外传模型参数。

### 8.6 运行时模型加载过程

部署完成后，`sample --npu-self-test`内部依次执行：

```text
打开/lib/yolov8n.nb
    -> awnn_get_info读取尺寸、MD5和NPU内存需求
    -> 按mem_size初始化AWNN/VIP堆
    -> awnn_create创建NPU网络
    -> 绑定Y、UV两个输入平面
    -> awnn_run触发NPU执行
    -> 取得输出张量
    -> CPU执行person筛选和NMS
    -> 销毁网络并释放NPU资源
```

`.nb`模型是运行时读取的数据文件，不会被编译进`sample_strip`。因此只更新模型
时可以单独替换`.nb`进行验证，但必须确保输入输出布局仍与应用代码一致。如果
新模型不再是`320x320`或输出不再是`[1,84,2100]`，必须同步修改预处理和后处理，
不能只替换文件名。

### 8.7 板端必须具备的运行条件

端侧成功运行需要同时满足：

1. 内核包含V853 NPU/VIP驱动；
2. 固件与应用使用兼容的VIPLite/AWNN版本；
3. 应用为ARM hard-float版本；
4. `.nb`是面向对应V853 NPU生成的模型；
5. NPU可用内存不小于模型报告的`mem_size`；
6. 输入严格为320x320 NV12双平面；
7. 模型和应用后处理约定一致。

当前开发板成功日志中已经出现：

```text
AWNN_LIB_1.0.4
VIPLite driver version=0x00010d00
vipcore, device init
```

这说明驱动、AWNN和VIPLite运行链已经建立。

### 8.8 常见部署故障定位

| 现象 | 优先检查 |
| --- | --- |
| `yolov8n.nb`不存在 | 模型路径、固件打包脚本、UDISK挂载位置 |
| `awnn_get_info`失败 | 模型损坏、文件权限、模型格式不兼容 |
| `awnn_create`失败 | VIPLite版本、NPU驱动、模型目标平台、NPU内存 |
| 输入文件大小错误 | 必须为320x320x1.5，即153600字节 |
| 黑帧成功但真人检测为0 | NV12/NV21顺序、缩放方式、置信度和后处理布局 |
| 检测框数量异常多 | 输出张量布局、量化输出解释、阈值、NMS |
| 第一次成功后再次失败 | `awnn_destroy/awnn_uninit`和VIP内存是否正确释放 |
| 程序能运行但普通模式回归 | NPU代码是否错误修改了原NV21主视频通路 |

### 8.9 部署完成的判定标准

只有同时满足以下条件，才能称为完成阶段9.1端侧部署：

- 板端运行的是ARM版本`sample_strip`；
- 板端模型MD5与仓库模型一致；
- 黑帧推理成功且退出码为0；
- 真实NV12图片能够得到正确person框；
- 连续重复运行不会出现VIP内存或初始化错误；
- 正式固件启动后无需从UDISK提供模型；
- 普通监控模式的显示、推流、录像和音频没有回归。

本阶段暂时采用手动命令启动NPU自检。实时检测线程将在阶段9.2接入正常程序；
是否配置系统开机自启属于最终部署工作，不应在单帧验证尚未通过时提前启用。

## 9. 验证分支工作流

### 9.1 创建分支

在稳定的`main`上创建验证分支：

```sh
git switch main
git pull --ff-only origin main
git switch -c feature/yolov8-edge-validation
```

当前仓库已经创建该分支，因此后续只需要：

```sh
git switch feature/yolov8-edge-validation
```

### 9.2 提交模型工程和验证代码

```sh
git status
git add yolov8-nv12 models \
    sample/ipc_camera/app/npu_self_test.c \
    sample/ipc_camera/app/yolov8_postprocess.c \
    sample/ipc_camera/include/npu_self_test.h \
    sample/ipc_camera/include/yolov8_postprocess.h \
    word README.md
git commit -m "验证YOLOv8端侧模型部署"
```

推送远端分支：

```sh
git push -u origin feature/yolov8-edge-validation
```

虚拟机获取分支：

```sh
git fetch origin
git switch feature/yolov8-edge-validation
git pull --ff-only
```

### 9.3 分支验证失败时

如果真实图片检测、重复运行或完整应用回归测试失败，应继续在当前feature分支
修改和提交，不要切回`main`强行合并，也不要使用`git push --force`覆盖主分支。

## 10. 当前分支的合并门槛

`feature/yolov8-edge-validation`的以下条件已经全部通过开发板验证：

### 10.1 构建检查

- Linux虚拟机中`./build.sh`成功；
- 没有新增编译错误和未处理的关键警告；
- 生成的`sample_strip`能够在V853启动；
- Git工作区中没有误提交临时输出、日志或密钥。

### 10.2 模型完整性

- `network_binary.nb`与`models/yolov8n.nb`的MD5一致；
- 开发板实际加载模型的MD5一致；
- `awnn_get_info()`读到的输入尺寸为`320x320`；
- 模型输出确认是`[1,84,2100]`。

### 10.3 单帧推理

- 黑帧推理成功、`objects=0`且退出码为0；
- `test01.jpg`转换出的NV12能够完成推理；
- `test02.jpg`转换出的NV12能够完成推理；
- 至少一张含人的图片能够输出合理的`person`检测框；
- 不含人的图片不会产生大量高置信度误检；
- 检测框坐标位于`0..320`范围内；
- 单次推理耗时稳定，没有异常增长。

### 10.4 稳定性和回归

- 连续执行自检至少50次，没有崩溃或NPU初始化失败；
- 内核日志中没有VIP内存持续增长或驱动异常；
- 正常启动应用时，LCD、RTSP、MP4和音频功能不受影响；
- Ctrl+C退出后NPU和应用资源能够正确释放。

本次验证已经满足上述条件。Acuity张量转储和IDE工程元数据已经清理，功能分支
的最终干净快照也已经合并到`main`。由于本地功能分支的早期提交曾包含这些中间
产物，本次采用`squash`合并，只把最终文件状态写入主分支，避免已删除的大型
转储仍通过历史提交进入远端仓库。实际采用的操作为：

```sh
git switch main
git pull --ff-only origin main
git merge --squash feature/yolov8-edge-validation
git commit -m "完成YOLOv8端侧部署与验证"
git push origin main
```

合并完成后切回`main`，直接开始阶段9.2。不要在阶段9.1验证分支中继续堆叠
实时检测、越线、区域入侵和报警功能；该分支只负责端侧AI部署及其验证。

## 11. 阶段9.2：实时NPU人形检测

### 11.1 数据通路

```text
MIPI CSI传感器
    |-- 主通路：1920x1080 NV21 -> LCD/VENC/RTSP/MP4
    `-- AI通路：320x320 NV12 -> NPU线程 -> person检测结果
```

### 11.2 实际采用的线程模型

```text
NPU工作线程
    -> 从VIPP 8取得320x320 NV12帧
    -> 复制Y/UV到固定输入缓存
    -> 立即ReleaseFrame
    -> AWNN推理
    -> 后处理
    -> 发布DetectionSnapshot
```

阶段9.2沿用原项目的单工作线程方案。由于VIPP 8只输出10 FPS，而实测单帧推理
约28 ms，线程能在下一帧到来前完成推理。VI帧只在复制期间被占用，推理时使用
固定的153600字节输入缓存；不建立无限增长的帧队列，因此不会累积应用层延迟。
如果后续模型推理时间接近或超过100 ms，再拆分取帧与推理线程并采用“最新帧
覆盖旧帧”槽位，不能改成无界队列。

`DetectionSnapshot`建议包含：

```c
typedef struct DetectionSnapshot {
    unsigned long long pts_us;
    unsigned long long sequence;
    int object_count;
    YoloDetection objects[YOLOV8_MAX_DETECTIONS];
} DetectionSnapshot;
```

生产者在互斥锁内替换快照，显示、越线和区域入侵模块只读取快照副本，不能直接
持有MPP视频帧指针。

### 11.3 坐标映射

如果AI通路将1920x1080直接拉伸到320x320：

```text
x_full = x_model * 1920 / 320
y_full = y_model * 1080 / 320
```

如果后续改为保持比例并使用letterbox，则必须记录缩放比例和上下/左右填充，先
减去padding再恢复坐标。两种预处理方式不能混用。

### 11.4 合并门槛

- 能持续输出真实摄像头中的person检测框；
- 检测帧率达到项目设定值，建议先以5到10 FPS运行；
- 主视频画面、RTSP和录像没有明显卡顿；
- 运行30分钟无死锁、帧泄漏和NPU内存泄漏；
- NPU线程停止后再销毁网络，退出顺序正确。

阶段9.2代码现已在`main`实现并完成板端复测。NPU连续推理、安全退出以及约
9.9 FPS的PTS应用层限频已经通过；person实景检测、完整退出统计和30分钟稳定性
测试通过后再标记为完成。

## 12. 阶段9.3：检测结果显示

阶段9.3负责把`320x320`模型坐标映射回视频坐标，并显示检测框、类别和置信度。

可选择两种显示目标：

1. 通过VENC RGN显示在H.264、RTSP和MP4中；
2. 通过LCD预览通路显示在本地屏幕中。

建议先只实现一个目标，验证坐标后再复用结果。OSD更新模块只读取最新检测结果，
不能阻塞NPU线程。

合并门槛：

- 人移动到画面四角时框的位置仍正确；
- 横竖屏旋转后的LCD坐标不会颠倒；
- 没有目标时旧框能及时清除；
- 框刷新不会破坏现有时间水印。

## 13. 阶段9.4：越线检测

### 13.1 判定点

越线判断不建议使用框中心，因为监控业务通常关心人的落脚位置。推荐使用检测框
底边中心点：

```text
point_x = (xmin + xmax) / 2
point_y = ymax
```

### 13.2 线两侧判断

设警戒线端点为`A(x1,y1)`、`B(x2,y2)`，目标点为`P(x,y)`：

```text
side(P) = (x2-x1)*(y-y1) - (y2-y1)*(x-x1)
```

若同一目标前后两次有效位置的`side()`符号发生变化，并且移动轨迹与有限线段
有效相交，则判定越线。符号变化方向还可以区分A到B或B到A。

### 13.3 状态与防抖

YOLOv8只提供检测框，不提供稳定目标ID，因此需要简单跟踪：

- 使用框中心距离或IoU关联前后帧目标；
- 连续丢失若干帧后删除轨迹；
- 在线附近设置滞回带，避免目标抖动造成反复越线；
- 同一轨迹触发后设置冷却时间。

合并门槛：

- 两个方向都能正确识别；
- 在线附近站立不会连续报警；
- 检测框短暂丢失不会立即产生新报警；
- 一次真实穿越只产生一次业务事件。

## 14. 阶段9.5：区域入侵检测

监控区域建议使用多边形顶点表示，而不是限制为矩形：

```c
typedef struct AlarmRegion {
    int enabled;
    int point_count;
    Point points[MAX_REGION_POINTS];
} AlarmRegion;
```

仍使用人形框底边中心点作为目标位置，通过射线法或叉积法判断点是否位于多边形
内部。

区域入侵不能只看单帧，应实现状态机：

```text
OUTSIDE
  -> 连续N帧位于区域内
ENTER_PENDING
  -> 达到确认帧数
INSIDE并产生进入事件
  -> 连续M帧位于区域外
OUTSIDE并允许下一次进入事件
```

这样可以抑制检测框在边界附近抖动造成的重复报警。

合并门槛：

- 多边形内部、外部和边界测试正确；
- 进入区域能触发一次事件；
- 持续停留不会每帧重复触发；
- 离开后再次进入可以重新触发；
- 多个人同时出现时状态不会互相覆盖。

## 15. 阶段9.6：本地音频报警

检测线程和规则线程只负责产生报警事件，不能直接阻塞播放音频。推荐结构为：

```text
越线/入侵事件
    -> 有界报警事件队列
    -> AudioAlarm线程
    -> AO播放/lib/alarm.wav
```

报警模块需要：

- 支持`LINE_CROSSING`和`REGION_INTRUSION`事件类型；
- 队列满时合并重复事件，不能无限分配内存；
- 设置全局或分规则冷却时间；
- 同一音频正在播放时避免重复叠加；
- 应用退出时唤醒线程、停止AO并释放资源；
- 播放失败只记录错误，不能导致录像和RTSP退出。

合并门槛：

- 两种业务事件都能触发`alarm.wav`；
- 连续检测不会造成声音无限重叠；
- 报警期间RTSP、录像和AAC采集继续工作；
- Ctrl+C可以立即结束报警线程并正常退出。

## 16. 阶段9.7：最终集成与压力测试

最终需要同时运行：

```text
VI采集
LCD预览
G2D旋转
H.264编码
时间OSD
RTSP音视频推流
MP4循环录像
YOLOv8实时检测
越线检测
区域入侵
本地音频报警
```

最终验收建议包括：

1. 连续运行至少2小时，无崩溃、死锁和明显内存增长；
2. RTSP音画同步保持正常；
3. MP4分段、循环覆盖和最终文件关闭正常；
4. LCD和编码画面没有持续黑屏、花屏或严重掉帧；
5. 人形检测持续输出，误检率在现场环境中可接受；
6. 越线和区域入侵事件符合配置；
7. 报警声音可播放且具有防重复机制；
8. 断网不影响本地检测和录像；
9. 存储空间不足时仍按阶段8.3策略清理；
10. Ctrl+C按照反向依赖顺序释放所有资源。

全部通过后，才能认为简历中的以下描述已经由复刻项目完整支撑：

> 集成NPU硬件加速能力，实现基于YOLOv8的人形检测、区域入侵警报及越线检测；
> 检测到目标事件时可触发本地音频报警并记录触发状态。

## 17. 验证分支回归与后续开发顺序

```text
main（当前稳定代码）
  |
  `-> feature/yolov8-edge-validation
          完成模型工程归档、板端部署和真实图片验证
          |
          `-> 验收通过后合并回main
                    |
                    |-- main提交：阶段9.2 实时NPU人形检测
                    |-- main提交：阶段9.3 检测结果显示
                    |-- main提交：阶段9.4 越线检测
                    |-- main提交：阶段9.5 区域入侵
                    |-- main提交：阶段9.6 音频报警
                    `-- main提交：阶段9.7 集成与压力测试
```

阶段9.1合并后，后续每完成一个小阶段就在`main`上生成一个独立提交，同时保存
对应技术说明、开发板日志和测试结果。这样仍然可以通过Git提交准确定位问题，
但不会引入过多短期分支。
