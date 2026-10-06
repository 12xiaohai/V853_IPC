# 阶段 3：G2D 旋转与 VO 实时显示技术说明

> 阅读方式：正文第1～10节讲当前显示模块；第11节保留2026-09-30的历史日志与验收结论。
> 本篇只展开LCD预览，但当前main已经包含编码、RTSP、录像等服务；学习不需要切换历史提交。

- 文档版本：V2.0，更新日期：2026-10-06。
- 源码基准：`main`的`eeeb66b`。
- 当前回归边界：2026-10-03日志中VO提交/回收均为25416、应用侧丢弃0，且正常退出；同一测试仍有70条VIPP4 FIFO溢出，不能称为上游无丢帧或完整预览链路长期稳定。

先读[阶段2采集说明](阶段2_VI视频采集技术说明.md)，应用层职责见[架构重构说明](架构重构_入口配置与应用生命周期技术说明.md)。

## 1. 阶段目标

本篇学习当前LCD预览如何处理方向、目标帧内存和异步显示。默认链路如下：

```text
GC2053 MIPI CSI
      ↓
VIPP4 / VI虚拟通道0：1920×1080 NV21
      ↓ GetFrame
G2D：顺时针旋转 270°
      ↓
MMZ 输出帧：1080×1920 NV21
      ↓ SendFrame
VO Layer 0 / Channel 0
      ↓ 缩放
480×800 LCD
```

VO不再使用目标帧后，通过释放回调把MMZ池节点标为可复用。这不是把VI源帧归还给摄像头：源帧在`video_display_submit()`返回后由采集线程归还，两种缓冲有不同的生命周期。

VIPP0负责编码，VIPP8负责NPU；预览的G2D旋转只作用于VIPP4输出的目标帧，不会自动旋转RTSP或MP4画面。阶段9.3的LCD检测框先画在VIPP4上，再随整帧旋转；本篇不展开画框模块。

## 2. 当前文件职责与接口

| 文件 | 作用 |
| --- | --- |
| `sample/ipc_camera/include/g2d.h` | 定义 G2D 转换上下文及接口 |
| `sample/ipc_camera/app/g2d.c` | 打开 `/dev/g2d`，配置物理地址并执行硬件旋转 |
| `sample/ipc_camera/include/video_display.h` | 定义显示配置和显示模块接口 |
| `sample/ipc_camera/app/video_display.c` | 管理 MMZ 帧池、VO 初始化、送帧、回调与销毁 |
| `sample/ipc_camera/include/context.h` | 让 VI 采集上下文持有显示模块指针 |
| `sample/ipc_camera/app/video_capture.c` | 取得 VI 帧后调用 G2D/VO 显示处理 |
| `sample/ipc_camera/app/config.c`、`application.c` | 默认显示参数在config.c；application.c在VI前启动显示，VI退出后销毁显示 |
| `sample/ipc_camera/app/main.c` | 信号、运行模式、主循环与统一生命周期入口 |
| `README.md` | 完整项目进度，不是阶段3独立运行入口 |

阅读顺序：[video_display.h](../sample/ipc_camera/include/video_display.h) → [application.c](../sample/ipc_camera/app/application.c)的`start_preview()` → [video_capture.c](../sample/ipc_camera/app/video_capture.c)的显示调用 → [video_display.c](../sample/ipc_camera/app/video_display.c)的submit/callback → [g2d.c](../sample/ipc_camera/app/g2d.c)的转换。

当前显示模块的接口为：

```c
VideoDisplayContext *video_display_create(const VideoDisplayConfig *config);
int video_display_start(VideoDisplayContext *display);
int video_display_submit(VideoDisplayContext *display,
                         const VIDEO_FRAME_INFO_S *source);
int video_display_stop(VideoDisplayContext *display);
void video_display_destroy(VideoDisplayContext *display);
```

create只分配应用对象、复制配置、初始化帧池锁并设置设备号，不访问硬件；start打开G2D、分配MMZ并启动VO。应用上下文拥有显示对象，VI只借用指针；`VideoDisplayContext`内部布局不暴露给采集模块。

没有单独的“显示送帧线程”：VI采集线程同步执行G2D和SendFrame；VO内部异步使用目标帧，并从SDK回调路径归还。采集线程取池节点与VO回调释放节点通过`buffer_lock`互斥。

## 3. 为什么需要 G2D

摄像头输出为 1920×1080 横屏图像，设备 LCD 为 480×800 竖屏。如果直接交给 VO 缩放，画面方向仍然不正确。

G2D 是 V853 的二维图形硬件加速单元。本阶段通过 `G2D_CMD_BITBLT_H` 完成 NV21 图像的 270° 旋转，避免 CPU 逐像素搬运带来的高占用。

旋转前后的尺寸关系为：

```text
源图像：1920×1080
旋转角度：270°（顺时针）
目标图像：1080×1920
```

VO 再将 1080×1920 图像缩放到 LCD 的 480×800 显示区域。

旋转和缩放是两步：当前G2D只旋转、交换宽高，LCD窗口缩放交给VO。1080:1920与480:800的比例并不完全一致，代码使用完整显示矩形，没有实现保持比例的letterbox或额外裁剪。270°是本板默认安装方向，不是所有摄像头/LCD的通用正确方向。

## 4. G2D 转换流程

`g2d_open()` 完成以下工作：

1. 校验旋转角度只能为 0°、90°、180° 或 270°。
2. 根据旋转角度计算目标帧尺寸。
3. 打开 `/dev/g2d`，取得 G2D 设备文件描述符。

`g2d_convert_frame()` 使用 VI 源帧和 MMZ 目标帧的物理地址填充 `g2d_blt_h`：

- 源、目标像素格式均由 `PIXEL_FORMAT_E` 转为 G2D 格式；
- 源裁剪区域为完整的 1920×1080 图像；
- 目标区域为完整的 1080×1920 图像；
- 源帧与目标帧均使用物理地址；
- 转换通过一次 `ioctl(G2D_CMD_BITBLT_H)` 在硬件中完成；
- 转换后将源帧 PTS 复制到目标帧，保证显示时间信息不丢失。

当前通过同步ioctl执行G2D：成功返回后不再需要源VI像素，采集线程实际在整个`video_display_submit()`返回后归还源帧。转换失败时没有有效显示结果，但采集线程仍须归还已取得的源帧。

函数会检查输入输出尺寸和像素格式映射，再设置物理地址、全图裁剪区域及旋转标志。它不自行申请目标图像，也不负责VI归还或VO回调。

## 5. MMZ 输出帧池

VO 显示是异步操作：`AW_MPI_VO_SendFrame()` 成功返回，只表示 VO 已接收该帧，并不表示 LCD 已经显示完毕。因此不能立即覆盖或释放该帧。

显示模块预先申请 5 个 1080×1920 NV21 MMZ 缓冲区，每个缓冲包含：

- Y 平面：1080×1920 字节；
- VU 平面：1080×1920÷2 字节；
- 唯一的帧 ID：0～4；
- `in_use` 使用状态。

单帧像素数据为`1080 × 1920 × 3 / 2 = 3,110,400`字节，约3.11 MB（2.97 MiB）；5帧合计15,552,000字节，约15.55 MB（14.83 MiB），不含驱动分配开销。代码分别为每帧Y、VU平面调用`AW_MPI_SYS_MmzAlloc_Cached()`，不能把它理解为单次malloc一个15 MB普通堆块。

采集线程每次显示一帧时：

1. 从帧池中查找空闲缓冲并标记为使用中。
2. 使用 G2D 将 VI 帧旋转到该缓冲。
3. 调用 `AW_MPI_VO_SendFrame()` 提交给 VO。
4. 返回VI采集线程，由采集线程调用`AW_MPI_VI_ReleaseFrame()`归还源帧；不是显示模块调用这个接口。
5. 输出缓冲仍由 VO 持有，暂时不能复用。

如果 5 个输出缓冲全部被 VO 占用，当前视频帧会被丢弃，而不是阻塞 VI 采集线程。程序会记录 `dropped` 计数，便于判断显示通路是否处理不及时。

“不阻塞”特指没有空闲帧池节点时不等待；并不保证互斥锁、G2D ioctl和SDK调用完全不会等待。这里只丢本次LCD预览，不会要求VIPP0编码通路丢掉同一时刻的帧。

`video_display_submit()`的当前返回语义：

| 返回值 | 处理结果 | 池节点与VI源帧 |
| --- | --- | --- |
| 0 | SendFrame成功，`submitted`增加 | 目标节点等待VO回调；VI源帧随后由采集线程归还 |
| 1 | 没有空闲节点，`dropped`增加 | 不做转换；VI源帧仍由采集线程归还 |
| -1 | 配置、G2D或SendFrame失败 | 已取得的目标节点立即标为空闲；不增加VO回调回收计数；VI源帧仍归还 |

因此`dropped`只统计帧池耗尽，不包含全部显示错误、G2D失败或内核VIN FIFO溢出。

## 6. VO 回调与帧所有权

VO 通道注册了 `video_display_callback()`。当 VO 不再使用一帧时，会产生：

```text
MPP_EVENT_RELEASE_VIDEO_BUFFER
```

回调根据 `VIDEO_FRAME_INFO_S.mId` 找到对应的 MMZ 缓冲，将其 `in_use` 恢复为 0，使其能够被下一次 G2D 转换复用。

帧所有权变化如下：

```text
空闲帧池
   ↓ 采集线程取得
G2D 正在写入
   ↓ SendFrame 成功
VO 持有并显示
   ↓ RELEASE_VIDEO_BUFFER 回调
重新进入空闲帧池
```

该回调机制是防止花屏、撕裂和帧内存被提前覆盖的关键。

回调中的“归还”只是`in_use = 0`，不调用`MmzFree()`；目标平面在stop时统一释放。`released`仅统计VO释放回调成功归还的池节点；G2D/SendFrame失败后的本地回收不计入该数。SendFrame成功也不证明物理屏幕已亮起，应结合肉眼画面和硬件连接验证。

## 7. VO 初始化参数

本阶段沿用原项目的显示参数：

| 参数 | 当前值 |
| --- | ---: |
| VO 设备 | 0 |
| UI Layer | `HLAY(2, 0)` |
| 视频 Layer | 0 |
| VO Channel | 0 |
| 显示接口 | `VO_INTF_LCD` |
| 接口同步类型 | `VO_OUTPUT_NTSC` |
| LCD 显示区域 | `(0, 0, 480, 800)` |
| VO 内部显示缓存数 | 2 |
| G2D 输出格式 | NV21 |
| G2D 输出尺寸 | 1080×1920 |

VO 初始化步骤为：使能设备、处理 UI Layer、设置公共属性、使能视频层、设置显示区域、创建通道、注册回调、设置缓存数并启动通道。

精确对照`video_display_start()`时，顺序为：

```text
g2d_open → 分配MMZ池 → AW_MPI_VO_Enable
→ AddOutsideVideoLayer(HLAY(2,0)) → CloseVideoLayer(UI层)
→ GetPubAttr → SetPubAttr(LCD / NTSC枚举)
→ EnableVideoLayer(0) → Get/SetVideoLayerAttr(显示矩形)
→ CreateChn(0,0) → RegisterCallback → SetChnDispBufNum(2) → StartChn
```

默认尺寸、旋转角度、显示矩形来自`config->display`；5个MMZ池节点、VO设备/层/通道号及2个VO显示缓冲目前是video_display.c中的固定设置。`VO_OUTPUT_NTSC`是源码采用的SDK枚举，不表示板载LCD变成NTSC电视分辨率；实际panel时序由底层配置决定。

曾尝试在VO使能前设置公共属性并遇到失败，当前顺序已经是Enable之后Get/SetPubAttr。不要按历史试验代码反过来修改；相关黑屏、接口顺序和硬件重新接线经过见[阶段4说明](阶段4_H264硬件编码技术说明.md)及[历史问题复盘](项目构建部署与历史问题复盘.md)。

## 8. 启动与退出顺序

### 8.1 启动顺序

```text
MPP 初始化
  → 打开 G2D
  → 申请 5 个 MMZ 输出帧
  → 初始化并启动 VO
  → 初始化 VI/ISP
  → 启动 VI 采集线程
```

当前`application.c`的`start_preview()`先完成显示create/start，再设置`ctx->video_capture.display`并启动VI。这样采集线程首次取帧时显示对象已经准备好。上图只是预览子链路，不是完整应用的全部启动步骤。

### 8.2 退出顺序

```text
收到 SIGINT/SIGTERM
  → 停止并 join VI 采集线程
  → 禁用并销毁 VI 通道、VIPP 和 ISP
  → 停止 VO Channel
  → 销毁 VO Channel
  → 禁用 Video Layer
  → 移除 UI Layer
  → 禁用 VO Device
  → 释放 MMZ 帧池
  → 关闭 G2D
  → 退出 MPP
```

实际代码首先停止 VI 采集线程和输入链路，是为了保证 VO 关闭期间不会再收到新帧。G2D 已经把图像复制到独立的 MMZ 输出帧，因此此时关闭 VI/ISP 不会破坏 VO 正在显示的帧。VO 通道停止和销毁完成后才释放 MMZ，避免 VO 仍在读取已经释放的物理内存。

完整应用会先清理规则、报警、检测和编码等其他服务，再进入上述VI/显示清理段。应用层在VI停止后把借用的display指针清空；显示stop之后destroy释放对象和锁，最后退出MPP。

启动失败时start调用stop按状态标志回滚。当前stop会记录驱动清理错误并继续后续清理，包括MMZ释放；它没有实现驱动清理失败后的可靠硬件隔离。因此“VO已不再持有目标内存”的判断以正常Stop/Destroy成功为前提，出现这些接口失败时不能只看最终计数就认定安全，也不能承诺固定退出时限。

## 9. 编译与开发板验证

在Linux虚拟机的实际仓库目录编译；不要把示例目录名当作固定路径：

```sh
git status
# 按需同步，先处理自己的修改：git pull --ff-only origin main
./build.sh
```

将 `output/sample_strip` 放到开发板运行：

```sh
chmod +x sample_strip
./sample_strip
```

该命令运行完整监控，不是阶段3专用的预览程序。下面只摘录G2D/VO/VI相关输出；网络、模型、音频等服务的启动失败仍可能使整个应用退出。当前main不提供`--preview-only`模式。

预期启动日志包含：

```text
[G2D] Opened: 1920x1080 -> 1080x1920, rotation=270
[VO] Allocated 5 MMZ buffers, each 1080x1920 NV21
[VO] Display started: layer=0, chn=0, lcd=480x800
[VI] Video capture started successfully
[VO] Submitted frame=1, pts=... us
[VO] Submitted frame=100, pts=... us
```

LCD 应显示方向正确、连续流畅的摄像头画面。按 `Ctrl+C` 后预期包含：

```text
[VI] Capture thread stopped, total frames=...
[VI] Video capture stopped
[G2D] Closed
[VO] Display stopped: submitted=..., released=..., dropped=...
[Main] Application exited with code: 0
```

## 10. 验收标准

- G2D 设备成功打开；
- 5 个 MMZ 输出缓冲分配成功；
- VO 设备、Layer 和 Channel 启动成功；
- LCD 能看到实时摄像头画面；
- 画面方向正确，不是横置、倒置或镜像；
- 画面尺寸覆盖预期的 480×800 区域；
- `Submitted frame` 计数持续增长；
- 正常运行时没有持续出现 `BITBLT failed` 或 `SendFrame failed`；
- `dropped` 长时间保持为 0 或仅偶发增长；
- Ctrl+C 后 VI、VO、MMZ、G2D 和 MPP 均正常释放；
- 应用退出码为 0，再次运行仍能正常显示。

学习检查：能够画出VI源帧与MMZ目标帧各自的所有权变化；能够说明为什么VI释放不等VO回调、为什么MMZ复用必须等回调、为什么停止VI在关闭显示之前。

2026-10-03用户提供的当前完整运行日志中，VO最终为`submitted=25416, released=25416, dropped=0`，正常退出码0。它支持“已提交的目标帧在本次正常清理中全部通过回调回收”，不证明每个传感器帧均到达应用，也不证明每帧都肉眼显示。

同一日志中仍有70条`video4 fifo overflow`，另外退出阶段出现一条`scaler12 channel ID nember is lost!!!`，需保留观察，不能仅凭它认定退出失败或确定根因。当前风险和复测见[阶段2说明](阶段2_VI视频采集技术说明.md)与[集成压力测试复盘](阶段9.7_集成压力测试与退出阻塞复盘.md)。

如果再次黑屏，按源帧、G2D、VO属性/层、panel背光和接线逐段检查；MP4/VLC正常来自VIPP0，不能排除VIPP4或LCD硬件问题。历史排查曾转储NV21帧，但当前显示模块不会自动生成`preview_frame_1920x1080.nv21`或`g2d_frame_1080x1920`文件。

## 11. 历史记录：阶段3开发板验证（2026-09-30）

以下保存当时单独预览版本的日志和结论，不是当前带编码、推流、录像、AI的整机压力测试结果。

本阶段代码已经完成交叉编译，并于 2026 年 9 月 30 日在 V853 开发板上运行验证。

### 11.1 G2D 与 VO 初始化结果

G2D 成功打开，旋转前后尺寸符合设计；5 个 MMZ 缓冲全部申请成功；VO Layer 0、Channel 0 和 480×800 显示窗口均成功启动：

```text
[G2D] Opened: 1920x1080 -> 1080x1920, rotation=270
[VO] Allocated 5 MMZ buffers, each 1080x1920 NV21
AW_MPI_VO_SetVideoLayerAttr: [0, 0, 320x240]->[0, 0, 480x800]
[VO] Display started: layer=0, chn=0, lcd=480x800
```

VO 渲染器收到首帧后报告的源图像尺寸为 1080×1920，证明 G2D 旋转后的帧尺寸已经被 VO 正确识别：

```text
hwd_layer_set_src: size0[1080x1920], size1[540x960]
VideoRender_ComponentThread: displayRect[0,0][1080x1920], bufSize[1080x1920]
```

### 11.2 连续显示与缓冲回收结果

运行期间 VI 采集计数和 VO 提交计数保持一致，并持续增长：

```text
[VI] Frame=1, size=1920x1080, pts=1492777074 us
[VO] Submitted frame=1, pts=1492777074 us
[VI] Frame=100, size=1920x1080, pts=1497771666 us
[VO] Submitted frame=100, pts=1497771666 us
[VI] Frame=200, size=1920x1080, pts=1502817592 us
[VO] Submitted frame=200, pts=1502817592 us
[VI] Frame=300, size=1920x1080, pts=1507863519 us
[VO] Submitted frame=300, pts=1507863519 us
```

从第 1 帧到第 300 帧约经过 15.09 秒，实际处理帧率约为 19.82 fps，与 VI 配置的 20 fps 相符。运行期间没有出现 `BITBLT failed`、`SendFrame failed` 或输出缓冲耗尽。

退出统计为：

```text
[VO] Display stopped: submitted=334, released=334, dropped=0
```

这三个数字说明：

- 334 个采集帧全部经过 G2D 并成功提交给 VO；
- 334 个 MMZ 输出帧全部通过 VO 回调归还；
- 没有因帧池耗尽而丢弃任何一帧；
- 退出时不存在仍被 VO 持有的输出缓冲。

### 11.3 安全退出结果

收到 `SIGINT` 后，采集线程先停止，随后 VI/ISP、VO、G2D 和 MPP 依次关闭，应用最终返回 0：

```text
[Main] Exit signal received: 2
[VI] Capture thread stopped, total frames=334
[VI] Video capture stopped
VideoLayer[0]: release [2]used inputFrame!
[G2D] Closed
[VO] Display stopped: submitted=334, released=334, dropped=0
[Main] Application exited with code: 0
```

其中 `release [2]used inputFrame` 表示 VO 停止时释放内部仍缓存的两帧，与设置的 VO 显示缓存数 2 一致。随后 `released=334` 与 `submitted=334` 相等，说明释放完整。

### 11.4 内核显示警告说明

日志中的以下信息来自显示驱动，而不是应用层 G2D 或 VO 接口失败：

```text
fcm lut 0 not find, auto retry after init
disp_mgr_set_layer_config: NULL hdl!
```

`fcm lut` 表示显示引擎没有找到对应的色彩管理查找表，并会在初始化后自动重试；本次没有影响 VO 启动、送帧和退出。

`NULL hdl!` 出现在 VO 通道停止、显示层撤销的退出阶段。应用层所有释放接口仍然执行成功，最终返回码为 0，因此当前可作为底层显示驱动的退出提示保留观察。如果后续出现重新启动无法显示或关屏异常，再进一步检查显示层关闭顺序。

### 11.5 阶段结论

G2D 旋转、VO 送帧、回调回收和安全退出均已通过实机验证。LCD 肉眼检查也已确认画面显示、方向和比例没有明显问题，因此阶段 3 正式验收完成，可以进入阶段 4：H.264 硬件编码。

## 12. 本次文档修订记录

2026-10-06：正文改为当前预览子链路学习说明，补充接口、执行线程、错误分支、MMZ字节数、缩放比例及所有权；纠正预期退出日志顺序，区分完整应用运行和历史预览验收，并记录10月3日FIFO仍复现的边界。只更新文档，本次没有编译或新增板端测试。
