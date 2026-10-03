# 阶段 3：G2D 旋转与 VO 实时显示技术说明

> 架构同步（2026-10-03）：当前main.c负责信号、运行模式与主循环，
> 默认参数集中在config.c，服务启动/回调/清理由application.c负责。
> 显示默认值在config->display；application先启动显示再启动VI，停止时先VI后显示。
> 早期阶段范围、旧代码示例及实测日志属于当时快照，不表示重构后已完成板端复测。
> 详见[架构重构说明](架构重构_入口配置与应用生命周期技术说明.md)。

## 1. 阶段目标

本阶段在已经通过实机验证的 VI 采集链路后加入 G2D 和 VO，解决摄像头横向图像在竖屏 LCD 上显示的问题：

```text
GC2053 MIPI CSI
      ↓
VI：1920×1080 NV21
      ↓ GetFrame
G2D：顺时针旋转 270°
      ↓
MMZ 输出帧：1080×1920 NV21
      ↓ SendFrame
VO Layer 0 / Channel 0
      ↓ 缩放
480×800 LCD
      ↓
VO 回调归还 MMZ 输出帧
```

本阶段仍不加入视频编码、RTSP 和录像，目的是独立验证实时预览通路。

## 2. 新增与修改的文件

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
| `README.md` | 更新当前复刻进度 |

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

G2D 转换是同步操作，因此 `g2d_convert_frame()` 返回后即可把原始 VI 帧归还给 VI。

## 5. MMZ 输出帧池

VO 显示是异步操作：`AW_MPI_VO_SendFrame()` 成功返回，只表示 VO 已接收该帧，并不表示 LCD 已经显示完毕。因此不能立即覆盖或释放该帧。

显示模块预先申请 5 个 1080×1920 NV21 MMZ 缓冲区，每个缓冲包含：

- Y 平面：1080×1920 字节；
- VU 平面：1080×1920÷2 字节；
- 唯一的帧 ID：0～4；
- `in_use` 使用状态。

单帧约占 3.11 MB，5 帧合计约占 15.55 MB 连续媒体内存。

采集线程每次显示一帧时：

1. 从帧池中查找空闲缓冲并标记为使用中。
2. 使用 G2D 将 VI 帧旋转到该缓冲。
3. 调用 `AW_MPI_VO_SendFrame()` 提交给 VO。
4. 立即调用 `AW_MPI_VI_ReleaseFrame()` 归还原始 VI 帧。
5. 输出缓冲仍由 VO 持有，暂时不能复用。

如果 5 个输出缓冲全部被 VO 占用，当前视频帧会被丢弃，而不是阻塞 VI 采集线程。程序会记录 `dropped` 计数，便于判断显示通路是否处理不及时。

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

必须先启动显示模块，再启动采集线程，否则采集线程可能在 VO 尚未准备完成时提交视频帧。

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

## 9. 编译与开发板验证

在 Linux 虚拟机中同步并编译：

```sh
cd ~/sample_demo
git pull origin main
./build.sh
```

将 `output/sample_strip` 放到开发板运行：

```sh
chmod +x sample_strip
./sample_strip
```

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
[VO] Display stopped: submitted=..., released=..., dropped=...
[G2D] Closed
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

## 11. 开发板验证结果

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
