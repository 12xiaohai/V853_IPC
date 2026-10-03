# 阶段 5：RTSP 视频推流技术说明

> 架构同步（2026-10-03）：当前main.c负责信号、运行模式与主循环，
> 默认参数集中在config.c，服务启动/回调/清理由application.c负责。
> RTSP参数在config->rtsp；application连接H.264/AAC分发并管理服务生命周期。
> 早期阶段范围、旧代码示例及实测日志属于当时快照，不表示重构后已完成板端复测。
> 详见[架构重构说明](架构重构_入口配置与应用生命周期技术说明.md)。

## 1. 阶段目标

本阶段在阶段 4 的 H.264 硬件编码基础上，将 VENC 输出的码流同时交给 RTSP 服务器，使 PC 端可以通过网络实时播放摄像头画面。

```text
VIPP 0 / VI Chn 0
        ↓ MPP Bind
      VENC 0
        ↓ AW_MPI_VENC_GetStream
        ├─→ sample_demo.h264（本地对照文件）
        └─→ 有界视频队列
                  ↓ RTSP 发送线程
              TinyServer
                  ↓
       rtsp://<wlan0-ip>:8554/ch0
```

本阶段只推送 H.264 视频。音频采集、AAC 编码和音视频同步将在阶段 6 实现。

## 2. 新增与修改的文件

| 文件 | 作用 |
| --- | --- |
| `sample/common/rtsp_server.h` | 定义 RTSP 服务器的 C 调用接口 |
| `sample/common/rtsp_server.cpp` | 封装 SDK `TinyServer` 和 `MediaStream` C++ 接口 |
| `sample/ipc_camera/include/rtsp_stream.h` | 定义 RTSP 码流队列模块接口 |
| `sample/ipc_camera/app/rtsp_stream.c` | 实现码流深拷贝、有界队列和发送线程 |
| `sample/ipc_camera/include/video_encoder.h` | 增加编码帧回调接口 |
| `sample/ipc_camera/app/video_encoder.c` | 保存 SPS/PPS，并将每个编码帧交给 RTSP 模块 |
| `sample/ipc_camera/app/config.c`、`application.c` | 默认网络参数在config.c；application.c在编码前启动RTSP，在生产者退出后销毁 |
| `sample/ipc_camera/app/main.c` | 信号、运行模式、主循环与统一生命周期入口 |
| `Makefile` | 增加 SDK TinyServer 接口头文件路径 |
| `README.md` | 更新阶段 5 开发进度 |

## 3. RTSP 服务参数

| 参数 | 当前值 |
| --- | --- |
| RTSP 会话 ID | 0 |
| 网络接口 | `wlan0` |
| 端口 | 8554 |
| 路径 | `/ch0` |
| 传输模式 | Unicast |
| 视频格式 | H.264 |
| 视频帧率 | 20 fps |
| 码流队列容量 | 16 帧 |
| 播放地址 | `rtsp://<wlan0-ip>:8554/ch0` |

当前保持与原项目一致，使用`wlan0`。如开发板实际使用有线网口，修改`config.c`
中`ip_camera_config_defaults()`的`config->rtsp.net_type`为`RTSP_NET_TYPE_ETH0`，
重新编译部署；不再到main.c查找网卡赋值。

## 4. VENC 码流回调

VENC 取流线程仍然调用：

```text
AW_MPI_VENC_GetStream()
        ↓
获取 mLen0/mLen1/mLen2、PTS 和帧类型
        ↓
写入本地 H.264 文件
        ↓
调用 VideoEncoderFrameCallback
        ↓
AW_MPI_VENC_ReleaseStream()
```

回调必须在 `AW_MPI_VENC_ReleaseStream()` 之前完成深拷贝，因为 `mpAddr0/1/2` 指向的缓冲区归 VENC 所有，释放码流后应用不能再访问。

## 5. 为什么使用独立发送线程

如果在 VENC 取流线程中直接执行网络发送，客户端速度、网络抖动或 RTSP 内部锁竞争都可能延迟 `AW_MPI_VENC_ReleaseStream()`，进而堵塞编码通路。

本阶段采用生产者—消费者模型：

- VENC 线程是生产者，只负责深拷贝码流并入队；
- RTSP 线程是消费者，从队列取帧并调用 TinyServer；
- 队列容量固定为 16 帧，防止异常网络造成内存无限增长；
- 队列满时丢弃最旧帧，优先保持直播的实时性。

## 6. SPS/PPS 与关键帧

RTSP 客户端可能在任意时刻连入。如果客户端只收到 IDR 图像而没有 SPS/PPS，则无法得知分辨率、Profile 等解码参数。

编码器启动时通过 `AW_MPI_VENC_GetH264SpsPpsInfo()` 取得 SPS/PPS，并在应用内存中保存副本。每当 VENC 输出 I 帧时，RTSP 队列将按以下顺序组装数据：

```text
SPS/PPS + mLen0 + mLen1 + mLen2
```

P 帧不重复附加 SPS/PPS。这样客户端最迟在下一个关键帧到达时开始正常解码。当前 GOP 为 75、帧率为 20 fps，最长等待时间约为 3.75 秒。

## 7. 启动与退出顺序

启动顺序：

```text
MPP 平台
  → LCD 预览
  → VI 采集
  → RTSP Server + RTSP 发送线程
  → VENC + VENC 取流线程
```

退出时按逆序执行：

```text
停止 VENC，确保不再产生新的 RTSP 帧
  → 唤醒并加入 RTSP 发送线程
  → 停止 TinyServer
  → 释放队列中的剩余帧
  → 停止预览和 MPP 平台
```

这个顺序避免了编码线程在 RTSP 对象销毁后继续入队的悬空指针问题。

## 8. 编译与开发板验证

在 Linux 虚拟机中执行：

```sh
cd ~/sample_demo
git pull origin main
./build.sh
```

把 `output/sample_strip` 复制到开发板后，先检查网络：

```sh
ifconfig wlan0
ping -c 3 <PC-IP>
```

然后运行：

```sh
cd /mnt/UDISK
./sample_strip
```

日志应输出类似：

```text
[RTSP] URL: rtsp://192.168.x.x:8554/ch0
[RTSP] Video service started: session=0, queue=16
[RTSP] Sender thread started
[VENC] H.264 encoder started: ...
```

在与开发板同一网络的 PC 上执行：

```sh
ffplay -rtsp_transport tcp rtsp://<board-ip>:8554/ch0
```

也可以在 VLC 中选择“打开网络串流”，输入相同地址。

## 9. 验收标准

- RTSP URL 能正常输出，服务器启动无错误；
- PC 可以 ping 通开发板的 `wlan0` IP；
- ffplay/VLC 可连接 `rtsp://<board-ip>:8554/ch0`；
- 客户端显示连续的 1920×1080 H.264 摄像头画面；
- LCD 本地预览同时正常；
- `sample_demo.h264` 仍然可正常生成和播放；
- RTSP 的 `queued` 与 `sent` 计数持续增长；
- 正常网络下 `dropped` 应为 0 或接近 0；
- `Ctrl+C` 后 VENC、RTSP、VI、VO 和 MPP 全部安全退出；
- 程序退出码为 0，再次启动仍可正常工作。

## 10. 当前验证状态

阶段 5 已通过 ARM 交叉编译和 V853 开发板运行验证。本次实测的 RTSP 地址为：

```text
rtsp://172.20.10.14:8554/ch0
```

使用 VLC 打开上述网络串流，连续运行约 142.6 秒。实测结果如下：

| 项目 | 实测结果 |
| --- | ---: |
| VLC 客户端 | 连接成功，画面正常 |
| VENC 编码帧数 | 2796 帧 |
| VENC 平均帧率 | 约 19.6 fps |
| H.264 关键帧数 | 38 帧 |
| 编码数据量 | 23,512,788 字节，约 22.42 MiB |
| RTSP 入队帧数 | 2796 帧 |
| RTSP 发送帧数 | 2796 帧 |
| RTSP 队列丢帧 | 0 帧 |
| LCD 预览提交/释放 | 2804/2804 |
| LCD 预览丢帧 | 0 帧 |
| 程序退出码 | 0 |

关键帧数与 GOP=75 的设置相符。日志证明 TinyServer、RTSP 发送线程、VENC 取流线程和 LCD 预览可以长时间并行工作，且 `Ctrl+C` 后能按正确顺序释放。阶段 5 全部验收完成。

长时间运行期间出现两次 `video4 fifo overflow` 以及少量 Wi-Fi `TXRX_WRN` 重试提示。它们没有造成应用层中断，RTSP 队列和 LCD 帧池的丢帧统计均为 0，因此不影响本阶段验收。如后续长时间压力测试中频繁出现，再分别检查 CSI 带宽/缓冲和 Wi-Fi 信号质量。

## 11. 实际问题与排障记录

### 11.1 RTSP C++源码编译时找不到 `stdlib.h`

编译 `rtsp_server.cpp` 时，libstdc++的 `<cstdlib>` 通过
`#include_next <stdlib.h>` 查找目标C库头文件失败。根因不是RTSP源码缺少
include，而是复制后的工具链系统头文件搜索顺序不完整。Makefile使用
`-idirafter $(TOOLCHAIN_ROOT)/include` 恢复目标C库头文件搜索后，C++源码和
最终程序均可正常编译。

### 11.2 开发板未联网时RTSP客户端无法连接

第一次运行时开发板没有连接网络，RTSP无法形成可供电脑访问的完整链路。
确认 `wlan0` 获得IP并能与电脑互相ping通后，TinyServer输出实际URL，VLC
连接成功。该现象属于网络前置条件未满足，不是H.264编码或RTSP队列故障。

### 11.3 长时间运行的非致命提示

实测期间的少量 `video4 fifo overflow` 和Wi-Fi `TXRX_WRN` 没有造成应用层
丢帧：RTSP发送2796/2796、队列丢帧0、LCD提交/释放2804/2804。因此本次按
非致命底层提示记录；只有频率持续升高或应用统计出现丢帧时才升级排查。

### 11.4 UDP发送出现`Resource temporarily unavailable`

阶段9.2联调期间，TinyServer底层连续输出：

```text
writeSocket(...), sendTo() error: ... Resource temporarily unavailable
```

本次共出现107次。退出时应用RTSP队列统计仍为视频`451/451`、音频`346/346`、
两条队列`dropped=0`，说明编码帧已经从应用队列完整交给TinyServer；失败发生在
TinyServer向RTSP客户端发送UDP/RTP数据时。常见原因是Wi-Fi瞬时拥塞、客户端读取
不及时或内核套接字发送缓存暂时写满（`EAGAIN`）。

验证时优先使用TCP传输：

```sh
ffplay -rtsp_transport tcp rtsp://<board-ip>:8554/ch0
```

VLC可在“输入/编解码器”的实时传输设置中选择RTP over RTSP (TCP)。如果TCP正常
而UDP持续报错，应归类为UDP/Wi-Fi传输问题，不应误判为NPU、VENC或应用RTSP队列
故障。若必须使用UDP，可进一步降低当前5 Mbit/s视频码率、改善Wi-Fi信号或调整
套接字发送缓存。
