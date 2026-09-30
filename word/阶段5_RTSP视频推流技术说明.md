# 阶段 5：RTSP 视频推流技术说明

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
| `sample/ipc_camera/app/main.c` | 创建、启动和逆序销毁 RTSP 模块 |
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

当前保持与原项目一致，使用 `wlan0`。如果开发板实际通过有线网口连接，应将 `main.c` 中的 `RTSP_NET_TYPE_WLAN0` 改为 `RTSP_NET_TYPE_ETH0`。

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

阶段 5 代码已完成，并通过代码边界、资源所有权和逆序销毁检查。当前 Windows 主机无可用的 Linux/ARM 运行环境，因此 ARM 交叉编译和 V853 实机 RTSP 播放仍待按第 8 节执行。实机验收通过后，应在本节记录实际 URL、发送帧数、丢帧数和播放结果。
