# 阶段 2：VI 视频采集技术说明

> 阅读方式：正文第1～9节对照当前VI模块；第10节单独保留2026-09-30历史验证记录。
> 本篇只展开预览VIPP4，但当前无参数运行仍会启动完整监控，不是只取帧测试。

- 文档版本：V2.0，更新日期：2026-10-06。
- 源码基准：`main`的`eeeb66b`。
- 当前回归边界：2026-10-03整机测试持续取帧并正常退出，但仍有70条VIPP4 FIFO溢出，不能标记当前采集链路长期稳定性已通过。

入口与对象管理见[阶段1说明](阶段1_基础框架与安全退出技术说明.md)；显示处理见[阶段3说明](阶段3_G2D旋转与VO实时显示技术说明.md)。

## 1. 阶段目标

本篇学习当前`video_capture.c`如何创建预览采集通路、取得VI帧、交给显示模块，以及归还帧和退出线程。视频编码使用VIPP0，NPU使用VIPP8，不能把这三条通路混成同一个采集线程。

```text
MIPI CSI 摄像头
      ↓
ISP 0：RAW 图像处理
      ↓
VIPP 4：输出 1920×1080 NV21 图像
      ↓
VI 虚拟通道 0
      ↓
采集线程：GetFrame → 读取元数据
      ↓ display非NULL时
video_display_submit：G2D旋转到独立MMZ帧 → 提交VO
      ↓ 返回采集线程
ReleaseFrame：归还VI源帧
```

当前`application.c`会先创建并启动显示模块，再把显示指针赋给VI上下文，所以正常监控包含上图的显示处理。`capture->display == NULL`时VI模块可以只取帧、记录元数据再归还，但main没有提供独立的采集测试选项。不要把模块支持的空显示指针与已有命令行模式混淆。

## 2. 当前源码的职责与阅读顺序

| 文件 | 作用 |
| --- | --- |
| `sample/ipc_camera/include/config.h` | `VideoCaptureConfig`保存纯采集参数，`IpCameraConfig.preview`持有它 |
| `sample/ipc_camera/include/context.h` | `VideoCaptureContext`保存参数副本、显示指针、线程和资源状态 |
| `sample/ipc_camera/include/video_capture.h` | 声明视频采集模块对外接口 |
| `sample/ipc_camera/app/video_capture.c` | 实现 VI/ISP 创建、采集线程、帧获取与资源销毁 |
| `sample/ipc_camera/app/config.c` | 初始化视频采集默认参数 |
| `sample/ipc_camera/app/application.c` | 在MPP/显示就绪后启动VI，在显示/MPP退出前停止VI |
| `sample/ipc_camera/app/main.c` | 通过应用生命周期接口发起启动与清理 |
| `README.md` | 更新当前复刻进度 |

推荐顺序：先读[config.c](../sample/ipc_camera/app/config.c)中的`config->preview`，再读[application.c](../sample/ipc_camera/app/application.c)的create与`start_preview()`，最后读[video_capture.c](../sample/ipc_camera/app/video_capture.c)中的start、thread、stop和destroy_pipeline。接口见[video_capture.h](../sample/ipc_camera/include/video_capture.h)。表格是当前职责划分，不表示application.c在历史阶段2就已存在。

`VideoCaptureConfig`只保存设备号、尺寸等参数；`VideoCaptureContext`还包含`thread_id`、`stop_requested`、`frame_count`及资源状态。该运行对象嵌入应用上下文，不由VI模块单独calloc；显示指针是借用引用，VI模块不负责free显示对象。

## 3. 视频采集参数

本阶段沿用原项目实时预览通路的主要参数：

| 参数 | 当前值 | 含义 |
| --- | ---: | --- |
| VI/VIPP 设备 | 4 | 原项目实时显示使用的视频输入设备 |
| ISP 设备 | 0 | 对摄像头 RAW 数据执行 ISP 处理 |
| VI 虚拟通道 | 0 | 应用取帧使用的虚拟通道 |
| 分辨率 | 1920×1080 | 单帧有效图像尺寸 |
| 帧率 | 20 fps | VI 目标采集帧率 |
| 像素格式 | `MM_PIXEL_FORMAT_YVU_SEMIPLANAR_420` | NV21，Y 平面后接 VU 交错平面 |
| 缓冲区数量 | 5 | VIPP 内部申请的帧缓冲数量 |
| 平面数量 | 2 | NV21 对应 Y 和 VU 两个平面 |
| 取帧超时 | 200 ms | GetFrame单次请求超时参数，不是线程join或全应用退出时限 |
| WDR | 关闭 | 当前`attributes.wdr_mode = 0` |

当前采集默认值由 `config.c` 的 `ip_camera_config_defaults()` 填入 `config->preview`，
`application.c` 创建上下文时再复制到VI运行状态。旧 `constructIpCameraContext()`
已移除；当前仍未增加外部配置文件加载，不能将后续计划当作已实现功能。

尺寸、帧率、像素格式和超时来自`config->preview`；5个采集缓冲、2个平面及WDR设置目前直接写在`video_capture_start()`的`VI_ATTR_S`中，不是所有参数都能只在config.c修改。

## 4. 初始化顺序

当前由`main()`调用应用start，`application.c`先执行`platform_init()`，再准备显示，
最后在`start_preview()`中调用`video_capture_start()`。VI内部初始化顺序仍为：

1. 填充 `VI_ATTR_S`，设置缓存类型、内存映射方式、分辨率、格式和帧率。
2. 调用 `AW_MPI_VI_CreateVipp()` 创建物理视频输入通道。
3. 调用 `AW_MPI_VI_SetVippAttr()` 写入 VIPP 属性。
4. 调用 `AW_MPI_ISP_Run()` 启动 ISP。
5. 调用 `AW_MPI_VI_EnableVipp()` 使能物理通道。
6. 调用 `AW_MPI_VI_CreateVirChn()` 创建虚拟通道。
7. 调用 `AW_MPI_VI_EnableVirChn()` 使能虚拟通道。
8. 创建视频采集线程。

只有上述操作全部成功，`video_capture_start()` 才返回成功。任一步失败都会进入统一的回滚路径，销毁此前已经创建的资源。

## 5. 采集线程工作方式

采集线程持续执行以下操作：

```text
AW_MPI_VI_GetFrame()
        ↓ 成功
读取帧编号、宽高和 PTS
        ↓ display非NULL
video_display_submit()：硬件旋转、尝试送VO
        ↓
AW_MPI_VI_ReleaseFrame()
        ↓
继续获取下一帧
```

`VIDEO_FRAME_INFO_S frame`是采集线程栈上的描述结构；其中的图像地址指向MPP/VI管理的缓冲，不是应用自己malloc的像素数据。不能free这些地址，也不能归还后继续读取它们。

有显示目标时，VI源帧必须保留到`video_display_submit()`返回：G2D同步读取VI源帧并写入独立MMZ目标帧，VO后续使用的是目标帧。采集线程随后归还VI源帧，不需要等LCD显示完成。没有显示目标时，读取元数据后直接归还。

显示返回0表示已提交；返回1表示没有空闲输出缓冲，丢弃本次预览显示；返回负值表示处理失败。当前采集线程只对负值打印显示处理警告，三种情况都会走后面的VI归还逻辑。GetFrame失败时没有取得有效帧，不调用ReleaseFrame；ReleaseFrame失败则记录错误，不能把调用过接口就等同于归还成功。

如果只取帧但不归还，VI 可用缓冲会逐渐耗尽，最终出现取帧超时或采集停滞。这是本模块必须保证成对调用 `GetFrame/ReleaseFrame` 的原因。

为避免日志刷屏，线程仅在以下时刻打印帧信息：

- 成功取得第 1 帧；
- 此后每累计 100 帧；
- 首次取帧失败以及连续每 25 次失败。

正常日志中的 `PTS` 单位为微秒，可在后续阶段用于音视频同步、编码和录像时间戳计算。

本模块的`frame_count`是成功取到的帧数，不是传感器总帧数、VO成功显示数或VENC编码数。当前线程没有直接把像素送给VENC；编码通路由另一模块绑定VIPP0与VENC，详见阶段4。

## 6. 安全退出与资源释放

收到退出信号后，main退出主循环，再由`ip_camera_application_stop()`在前面的规则、检测和编码等模块清理后调用`video_capture_stop()`。VI模块自身的顺序是：

1. 设置采集线程停止标志。
2. 调用`pthread_join()`等待采集线程退出；正在处理的帧仍需走完处理和归还路径。
3. 禁用 VI 虚拟通道。
4. 销毁 VI 虚拟通道。
5. 禁用 VIPP。
6. 停止 ISP。
7. 销毁 VIPP。
8. 返回应用清理层；当前`application.c`继续清理显示等资源，最后执行`platform_deinit()`。

这里必须先停止采集线程，再销毁 VI 通道，否则线程可能在 VI 已被销毁后继续调用 `GetFrame/ReleaseFrame`，形成资源竞争甚至崩溃。

`VideoCaptureContext` 中为每类资源设置了状态字段，例如 `vipp_created`、`isp_running` 和 `thread_started`。清理函数只释放已经成功创建的资源，因此正常退出和初始化中途失败可以复用同一套清理逻辑。

200 ms只限制正常情况下GetFrame的一次等待请求。join没有超时参数，G2D ioctl、ReleaseFrame和其他驱动调用也不受这个参数统一约束，因此不能承诺整个VI停止或应用退出最多200 ms。`stop_requested`是协作退出标志，不是强制取消线程；底层阻塞仍需单独定位。

## 7. 编译与运行验证

在Linux虚拟机的实际仓库目录编译当前代码。已有未提交修改时先检查，不要为学习强制覆盖：

```sh
# 先进入你自己的仓库目录，路径不必叫~/sample_demo
git status
# 仅在需要同步且本地状态适合时执行：git pull --ff-only origin main
./build.sh
```

将 `output/sample_strip` 复制到开发板后运行：

```sh
chmod +x sample_strip
./sample_strip
```

这个命令启动完整监控，依赖当前固件的媒体库、网络和NPU模型等。下面只列出学习VI时关注的日志，省略其他模块输出；看到编码或推流日志不代表程序运行错误。不要使用main不支持的历史诊断选项。

预期应看到类似日志：

```text
[VI] Starting: vipp=4, isp=0, chn=0, 1920x1080@20fps
[VI] Video capture started successfully
[VI] Capture thread started
[VI] Frame=1, id=..., size=1920x1080, pts=... us
[VI] Frame=100, id=..., size=1920x1080, pts=... us
```

按 `Ctrl+C` 后，预期看到：

```text
[Main] Exit signal received: 2
[VI] Capture thread stopped, total frames=...
[VI] Video capture stopped
[Main] Application exited with code: 0
```

## 8. 学习检查、回归指标与当前风险

本阶段应满足以下条件：

- 工程能够完成交叉编译和链接；
- 开发板能够成功创建 VIPP、启动 ISP 并使能 VI 通道；
- 日志中的帧计数持续增长；
- 实际帧尺寸为 1920×1080；
- PTS 随帧持续递增；
- 运行期间没有连续 `GetFrame failed`；
- 按 `Ctrl+C` 后线程退出，VI、ISP 和 MPP 均正常释放；
- 再次启动程序仍能正常采集，证明上次退出没有残留占用。

以上是检查目标，不是“当前已经全部通过”的声明。阅读代码后，应能解释：为什么显示对象先启动、为什么每个成功GetFrame都要归还、VI源帧与显示MMZ帧有什么区别、为什么join之前不能销毁通道。

2026-10-03约21分钟完整监控日志中，VI最终取帧25416次，程序退出码0；同时共出现70条以下内核错误，运行末尾仍有复现：

```text
[1357.677709] [VIN_ERR]video4 fifo overflow, CSI frame count is 25214
```

这表明VIPP4硬件FIFO溢出仍是未解决项。70条错误日志不等于已经确定丢了70帧，日志也不足以确认具体根因。VO的`dropped=0`不能证明上游硬件没有溢出；历史阶段2短时取帧通过不能替代当前全功能负载验收。排查记录见[集成压力测试复盘](阶段9.7_集成压力测试与退出阻塞复盘.md)与[VIPP4诊断记录](阶段9.7_VIPP4诊断结果与回归main记录.md)。

## 9. 与原项目的关系

原项目在取帧后通过G2D处理，并把目标帧交给帧管理器。当前复刻工程使用相同的GetFrame/ReleaseFrame基础，但由`video_display_submit()`管理独立MMZ池和VO提交，不能把原项目帧管理器接口当作这里的当前实现。

历史阶段2版本曾只读取元数据后直接归还，阶段3才接入显示。现在两个能力已经同时存在；继续学习[阶段3说明](阶段3_G2D旋转与VO实时显示技术说明.md)，不需要恢复Git历史。

## 10. 历史记录：阶段2开发板验证（2026-09-30）

以下是当时未接显示模块的采集版本记录。数字、日志和当时结论作为历史证据保留，不代表当前整机版本已通过长期稳定性验收。

本阶段代码已经完成交叉编译，并于 2026 年 9 月 30 日在 V853 开发板上进行实际运行验证。

### 10.1 已通过项目

- MPP 和 ISP600 初始化成功；
- `vin_video4` 打开成功，并确认 VIPP 4 对应 ISP 0；
- 识别到 `gc2053_mipi` 摄像头及其 ISP 参数；
- VIPP 使用 5 个缓冲区，离线模式启动成功；
- VI 虚拟通道和采集线程启动成功；
- 实际取得的图像帧尺寸为 1920×1080；
- 帧计数从 1 持续增长到至少 400，没有发生采集停滞；
- 第 1 帧 PTS 为 12159821014 us，第 400 帧 PTS 为 12179945827 us；
- 399 个帧间隔共经过约 20.125 秒，实测帧率约为 19.83 fps，与配置的 20 fps 相符。

关键运行日志如下：

```text
[VI] Starting: vipp=4, isp=0, chn=0, 1920x1080@20fps
[ISP]open video device[4], detect isp0 success!
[VI] Video capture started successfully
[VI] Capture thread started
[VI] Frame=1, id=0, size=1920x1080, pts=12159821014 us
[VI] Frame=100, id=4, size=1920x1080, pts=12164808050 us
[VI] Frame=200, id=4, size=1920x1080, pts=12169853976 us
[VI] Frame=300, id=4, size=1920x1080, pts=12174899902 us
[VI] Frame=400, id=4, size=1920x1080, pts=12179945827 us
```

### 10.2 日志中警告的说明

`height is towards 16 alignment` 表示底层 VIN/ISP 按 16 像素对高度做内存对齐。传感器和 ISP 工作尺寸为 1920×1088，而应用取得的有效图像仍为 1920×1080，因此该提示不是采集失败。

`isp0_1920_1088_20_0_gc2053_mipi_ctx_saved.bin failed` 表示没有找到可选的 ISP 上下文缓存文件。随后日志显示系统已经成功加载 GC2053 对应的 ISP 配置，因此不影响本次采集。

启动初期出现一次`GetFrame failed`，随后持续出帧。当时判断可能与首帧尚未就绪和取帧等待有关，但只有`ret=-1`日志，不能单凭它确定具体超时原因。本次没有持续取帧失败；后续出现同类日志时仍需结合返回值定义、内核日志和连续失败次数判断。

`unable to subscribe to tdm event`、一次`AEWB: stats error`以及`get sensor_temp failed`在这次历史测试中未阻断出帧。它们应作为观察项，不能据此认定根因或保证无害；后续复现频率、曝光/画面变化及采集异常都需要一起判断。

### 10.3 安全退出验证

程序持续采集到第 905 帧后收到 `SIGINT`（信号 2），采集线程立即停止，随后依次关闭 VIPP、LDCI、ISP 和 MPP，最终以返回码 0 正常退出：

```text
[Main] Exit signal received: 2
[VI] Capture thread stopped, total frames=905
DisableVipp[4]: vfmt.bufs:5, online:0
[ISP]close isp device[0] success!
[VI] Video capture stopped
MonitorEnvVar thread will exit!
[Main] Application exited with code: 0
```

从收到退出信号到采集线程结束约为 18 ms，没有出现线程等待超时、通道销毁失败或 ISP 关闭失败。退出顺序符合设计要求：先停止使用视频帧的线程，再关闭 VI/ISP，最后退出整个 MPP 平台。

ISP 在退出时还成功生成了 `/mnt/extsd/isp0_1920_1088_20_0_gc2053_mipi_ctx_saved.bin`。下次启动时可以直接读取该上下文缓存，首次运行时的“找不到 ISP 上下文缓存文件”警告预计不会再次出现。

### 10.4 阶段结论

阶段 2 的交叉编译、摄像头识别、ISP 启动、VI 连续取帧、帧归还、PTS 递增和信号触发安全退出均已通过 V853 实机验证。本阶段验收完成，可以进入阶段 3：G2D 图像旋转与 VO/LCD 实时显示。

## 11. 本次文档修订记录

2026-10-06：正文改为当前源码学习说明；明确正常监控包含显示处理、纯配置与运行状态的区别、两种图像缓冲的归还时机和join无固定时限；保留历史日志并补充10月3日FIFO仍复现的边界。只核对文档与源码，本次没有编译、上板复测或修改程序。
