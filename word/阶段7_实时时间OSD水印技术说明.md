# 阶段 7：实时时间 OSD 水印技术说明

## 1. 本阶段目标

在1920×1080编码画面的左上角叠加实时日期和时间：

```text
YYYY-MM-DD HH:MM:SS
```

水印附着在VENC通道0，因此以下两个输出都会包含相同水印：

- `/mnt/UDISK/sample_demo.h264` 本地H.264文件；
- `rtsp://<开发板IP>:8554/ch0` RTSP视频轨。

LCD预览使用独立的 `VI → G2D → VO` 通路，不经过VENC，因此本阶段LCD上
不会显示水印。这与原项目把时间区域挂在VENC上的架构一致。

## 2. 数据通路

```text
                        ┌→ VENC原始编码 → H.264
VI Device 0 / Chn 0 ───┤
                        └→ RGN硬件叠加时间位图 → 编码后的H.264 → 文件/RTSP
```

更准确地说，RGN区域附着到VENC通道后，MPP在编码过程中把位图混合到输入
画面。应用不需要获取每一帧NV21图像，也不需要在CPU上逐像素处理整张
1920×1080画面。

## 3. 新增文件

```text
sample/ipc_camera/include/time_osd.h
sample/ipc_camera/app/time_osd.c
```

模块提供以下生命周期：

```text
time_osd_create()
        ↓
time_osd_start()
        ↓
time_osd_stop()
        ↓
time_osd_destroy()
```

主程序在VENC启动成功后创建OSD；退出时先停止并解绑OSD，再销毁VENC。

## 4. 字库实现与原项目保持一致

原项目调用 `librgb_ctrl`，从下列目录加载压缩ASCII字库：

```text
/usr/share/osd/fonts/asc64.lz4
```

经过开发板检查，目标系统已经安装：

```text
asc16.lz4
asc32.lz4
asc64.lz4
```

因此本阶段采用原项目实现方式：先调用 `load_font_file(FONT_SIZE_64)`，
再用 `create_font_rectangle()` 将时间字符串生成为RGB图片。停止服务时调用
`unload_font_file(FONT_SIZE_64)` 释放字库资源。如果以后更换根文件系统，
必须继续提供 `/usr/share/osd/fonts/asc64.lz4`。

## 5. 位图格式与尺寸

与原项目相同，字库图片使用 `OSD_RGB_32`，RGN和BITMAP使用
`MM_PIXEL_FORMAT_RGB_8888`。四个前景分量均设置为 `0xff`，得到白色文字；
`enable_bg=0`，使文字以透明背景覆盖视频。

图片原始宽高由 `create_font_rectangle()` 根据字符串和64号字体计算，RGN
尺寸再向上进行16像素对齐。程序会在启动日志中输出最终位图尺寸。

区域放置在编码画面的 `(32, 32)`，坐标满足4像素对齐要求。

## 6. 初始化流程

`time_osd_start()` 依次完成：

1. 计算固定时间字符串位图尺寸；
2. 调用 `AW_MPI_RGN_Create()` 创建 `OVERLAY_RGN`；
3. 通过 `create_font_rectangle()` 生成第一张RGB8888时间位图；
4. 调用 `AW_MPI_RGN_SetBitMap()` 设置位图；
5. 调用 `AW_MPI_RGN_AttachToChn()` 附着到VENC通道0；
6. 创建更新时间线程。

每个资源成功创建后都会记录状态。任一步失败时，停止函数只回收已经创建
成功的资源。

## 7. 每秒更新时间

更新时间线程每秒执行一次：

```text
time()/localtime_r()
        ↓
strftime("%Y-%m-%d %H:%M:%S")
        ↓
create_font_rectangle()生成RGB8888位图
        ↓
AW_MPI_RGN_SetBitMap()
        ↓
释放用户态临时位图
```

`SetBitMap`完成后，MPP已经接收该位图，用户态临时内存可以释放。线程将
一秒休眠拆成100毫秒的小段，以便按下 `Ctrl+C` 后快速响应退出。

应用只生成一张较小的时间位图且每秒更新一次；逐帧叠加由MPP完成，因此
CPU负担远低于应用逐帧获取1920×1080图像进行软件绘制。

## 8. 安全退出

退出顺序如下：

1. 设置OSD线程退出标志；
2. `pthread_join()` 等待更新线程停止；
3. `AW_MPI_RGN_DetachFromChn()` 从VENC解绑区域；
4. `AW_MPI_RGN_Destroy()` 销毁区域；
5. 停止并销毁VENC。

必须在VENC仍存在时解绑RGN，否则区域内部仍可能引用已经销毁的编码通道。

## 9. 开发板验证

重新编译并复制程序后运行：

```sh
cd /mnt/UDISK
./sample_strip
```

正常启动日志应包含：

```text
[OSD] Time updated: YYYY-MM-DD HH:MM:SS, count=1
[OSD] Time watermark started: venc=0, handle=0, position=(32,32), bitmap=...x..., font=FONT_SIZE_64
[OSD] Update thread started
```

使用VLC打开RTSP地址，检查画面左上角：

1. 日期和时间字符完整、清晰；
2. 秒数每秒递增；
3. 水印没有遮挡大面积画面；
4. 视频和音频仍连续播放；
5. 音画同步没有受到影响。

运行10～20秒后退出，再用ffplay播放本地文件：

```sh
ffplay sample_demo.h264
```

本地H.264画面也应包含相同水印。退出日志应包含：

```text
[OSD] Update thread stopped, updates=...
[OSD] Time watermark stopped: updates=...
```

程序最终退出码应为0。

## 10. 时间不正确时怎么办

水印显示的是Linux系统本地时间。若开发板没有RTC同步或联网校时，水印
可能显示错误日期；这不属于OSD绘制错误。可以先查看：

```sh
date
```

确保系统时间和时区正确后再启动应用。后续如需联网自动校时，可单独配置
NTP服务。

本次实测中，系统最初显示 `1970-01-01`。网络连通并不等于系统已经自动
校时，使用 Tina Linux/BusyBox 提供的 NTP 客户端完成一次校时：

```sh
busybox ntpd -n -q -p ntp.aliyun.com
date
```

校时后系统时间更新为 `2026-10-01`，重新运行应用后，RTSP画面左上角的
OSD立即显示正确日期和时间。若开发板重启后再次回到1970年，需要检查RTC，
或在联网成功后自动执行上述NTP校时命令。

## 11. 验收标准

1. FONT_SIZE_64字库加载、RGN创建、位图设置和VENC附着均成功；
2. RTSP和本地H.264都显示时间；
3. 秒数正常刷新；
4. 原有LCD预览、H.264、AAC和RTSP音视频功能不受影响；
5. 退出时OSD线程、附着关系和区域均正常回收；
6. 程序退出码为0。

当前状态：已通过开发板实测。VLC画面中的白色时间水印清晰完整，日期正确，
秒数正常递增；NTP校时后由 `time()/localtime_r()` 获取的系统时间能够正确
反映到OSD中。阶段7验收通过。

## 12. 下一步

时间OSD验证通过后进入阶段8：MP4本地录像、按时间自动命名、循环覆盖和
存储空间管理。
