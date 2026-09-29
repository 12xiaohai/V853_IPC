# 阶段 2：VI 视频采集技术说明

## 1. 阶段目标

本阶段在阶段 1 的 MPP 平台初始化和安全退出框架上，完成 V853 摄像头视频输入（VI）链路的最小闭环：

```text
MIPI CSI 摄像头
      ↓
ISP 0：RAW 图像处理
      ↓
VIPP 4：输出 1920×1080 NV21 图像
      ↓
VI 虚拟通道 0
      ↓
采集线程：GetFrame → 读取元数据 → ReleaseFrame
```

当前阶段只验证“摄像头能够稳定产出视频帧”。帧取得后立即归还给 MPP，不进行 G2D 旋转、LCD 显示、VENC 编码或文件保存。

## 2. 本阶段新增与修改的文件

| 文件 | 作用 |
| --- | --- |
| `sample/ipc_camera/include/context.h` | 增加 `VideoCaptureContext`，统一保存 VI 配置、线程和资源状态 |
| `sample/ipc_camera/include/video_capture.h` | 声明视频采集模块对外接口 |
| `sample/ipc_camera/app/video_capture.c` | 实现 VI/ISP 创建、采集线程、帧获取与资源销毁 |
| `sample/ipc_camera/app/config.c` | 初始化视频采集默认参数 |
| `sample/ipc_camera/app/main.c` | 在 MPP 初始化后启动 VI，在 MPP 退出前停止 VI |
| `README.md` | 更新当前复刻进度 |

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
| 取帧超时 | 200 ms | `AW_MPI_VI_GetFrame()` 单次等待时间 |
| WDR | 关闭 | 当前阶段先验证基本采集链路 |

这些参数暂时由 `constructIpCameraContext()` 设置。采集链路在开发板验证稳定后，可再恢复原项目通过配置文件加载参数的方式。

## 4. 初始化顺序

`main()` 先调用 `platform_init()` 完成 MPP 系统初始化，再调用 `video_capture_start()`。VI 初始化严格按以下顺序执行：

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
        ↓
AW_MPI_VI_ReleaseFrame()
        ↓
继续获取下一帧
```

`AW_MPI_VI_GetFrame()` 返回的 `VIDEO_FRAME_INFO_S` 不是应用自己申请的普通内存，而是 MPP/VI 管理的图像缓冲。当前阶段不复制图像数据，因此读取完元数据后必须立即调用 `AW_MPI_VI_ReleaseFrame()`。

如果只取帧但不归还，VI 可用缓冲会逐渐耗尽，最终出现取帧超时或采集停滞。这是本模块必须保证成对调用 `GetFrame/ReleaseFrame` 的原因。

为避免日志刷屏，线程仅在以下时刻打印帧信息：

- 成功取得第 1 帧；
- 此后每累计 100 帧；
- 首次取帧失败以及连续每 25 次失败。

正常日志中的 `PTS` 单位为微秒，可在后续阶段用于音视频同步、编码和录像时间戳计算。

## 6. 安全退出与资源释放

收到 `SIGINT` 或 `SIGTERM` 后，主循环退出并调用 `video_capture_stop()`：

1. 设置采集线程停止标志。
2. 等待最多一个取帧超时周期，让线程退出并执行 `pthread_join()`。
3. 禁用 VI 虚拟通道。
4. 销毁 VI 虚拟通道。
5. 禁用 VIPP。
6. 停止 ISP。
7. 销毁 VIPP。
8. 返回 `main()`，最后执行 `platform_deinit()` 退出 MPP 系统。

这里必须先停止采集线程，再销毁 VI 通道，否则线程可能在 VI 已被销毁后继续调用 `GetFrame/ReleaseFrame`，形成资源竞争甚至崩溃。

`VideoCaptureContext` 中为每类资源设置了状态字段，例如 `vipp_created`、`isp_running` 和 `thread_started`。清理函数只释放已经成功创建的资源，因此正常退出和初始化中途失败可以复用同一套清理逻辑。

## 7. 编译与运行验证

在 Linux 虚拟机中同步代码并编译：

```sh
cd ~/sample_demo
git pull origin main
./build.sh
```

将 `output/sample_strip` 复制到开发板后运行：

```sh
chmod +x sample_strip
./sample_strip
```

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

## 8. 验收标准

本阶段应满足以下条件：

- 工程能够完成交叉编译和链接；
- 开发板能够成功创建 VIPP、启动 ISP 并使能 VI 通道；
- 日志中的帧计数持续增长；
- 实际帧尺寸为 1920×1080；
- PTS 随帧持续递增；
- 运行期间没有连续 `GetFrame failed`；
- 按 `Ctrl+C` 后线程退出，VI、ISP 和 MPP 均正常释放；
- 再次启动程序仍能正常采集，证明上次退出没有残留占用。

## 9. 与原项目的关系

原项目的 `video_capture.c` 在取得 VI 帧后，会继续申请目标帧、通过 G2D 做旋转/格式处理，并将处理后的帧送入帧管理器。复刻工程本阶段保留了同样的 `AW_MPI_VI_GetFrame()` 与 `AW_MPI_VI_ReleaseFrame()` 采集基础，但刻意去掉后续处理，便于单独定位摄像头、ISP 或 VI 通道问题。

下一阶段将在当前稳定取帧能力上加入 G2D 旋转和 VO 显示，形成“摄像头采集到 LCD 实时预览”的完整视频通路。

## 10. 开发板验证结果

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

启动初期出现一次 `GetFrame failed`，随后帧持续正常输出。这是 ISP 和视频通道刚启动时首帧尚未就绪造成的 200 ms 超时，不属于持续性故障。

`unable to subscribe to tdm event`、一次 `AEWB: stats error` 以及 `get sensor_temp failed` 均未阻断 ISP 和 VI 出帧。当前可作为底层驱动兼容性提示保留观察；只有后续出现连续取帧失败、曝光异常或画面异常时才需要继续定位。

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
