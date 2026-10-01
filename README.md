# sample_demo

This directory rebuilds the V853 intelligent camera project step by step. The
application follows the original `sample/ipc_camera` source layout. The V853
cross toolchain, headers, libraries, rootfs, and firmware packaging inputs are
kept under `sdk`, so the project does not depend on its parent directory.

## Development stages

1. Process lifecycle and MPP system initialization.
2. Camera capture with VI.
3. LCD preview with G2D rotation and VO.
4. H.264 encoding with VENC.
5. RTSP video streaming.
6. Audio capture, AAC encoding, and A/V synchronization.
7. OSD time watermark on the encoded video path.
8. MP4 recording and cyclic file management.
9. NPU detection, line crossing, region intrusion, and audio alarm.

The complete Stage 9 model-conversion explanation, branch merge gates, and
implementation roadmap are documented in
`word/阶段9_YOLOv8端侧部署与智能监控集成规划.md`.

## Current progress

Stage 1 initializes the MPP system, establishes the global PTS base, handles
`SIGINT`/`SIGTERM`, and shuts the platform down cleanly. Stage 2 creates the
MIPI CSI/VI capture path, continuously obtains and releases video frames in a
worker thread, and destroys the VI/ISP resources in reverse order on exit.
Stage 3 rotates each captured frame by 270 degrees with G2D and submits the
result to VO for a 480x800 LCD preview. VO release callbacks return the MMZ
output buffers to a five-frame pool. Stage 4 adds the original main-stream
topology: VIPP 0 is bound inside MPP to VENC channel 0 for 1920x1080 H.264
encoding, while an application thread extracts the encoded stream to
`/mnt/UDISK/sample_demo.h264`. Stage 5 adds a bounded H.264 frame queue and a
dedicated RTSP sender thread. The SDK TinyServer publishes the main stream at
`rtsp://<wlan0-ip>:8554/ch0`; SPS/PPS data is prepended to every IDR frame so a
client can join the stream at a key-frame boundary. Stage 6.1 verified AI
device 0 by capturing 16 kHz, 16-bit mono PCM. Stage 6.2 now binds AI channel 0
to AENC channel 0 inside MPP and writes ADTS-framed AAC to
`/mnt/UDISK/sample_demo.aac`. The raw PCM implementation remains as a
standalone diagnostic example, but it is no longer started by `main.c`. Stage
6.3 adds a separate bounded AAC queue to the RTSP service. H.264 and AAC frames
retain their original MPP microsecond PTS and are submitted to TinyServer in
media-time order for synchronized network playback. Stage 7 follows the
original project by using `librgb_ctrl` with the target's `asc64.lz4` font,
attaching an RGB8888 overlay region to VENC channel 0, and refreshing the clock
once per second. Both the local H.264 file and RTSP video contain the watermark.
The Stage 7 implementation has been verified on the V853 board; after BusyBox
NTP synchronization, the watermark shows the correct local date and updates
once per second. Stage 8.1 adds an MPP MUX recorder that reuses the existing
H.264 and AAC frames, preserves their original timestamps, waits for an H.264
key frame, and writes `/mnt/UDISK/sample_demo.mp4` without creating a second
encoder pipeline. Stage 8.1 has passed board validation: the recorder maps the
video and audio stream IDs correctly, strips ADTS headers only from AAC samples
sent to the MP4 muxer, closes the file cleanly, and produces a recording whose
video and audio play correctly. Stage 8.2 now adds timestamp-based file names
and 60-second MPP MUX file rotation. A dedicated worker handles next-file
requests outside the MPP callback, and the minimum-duration policy switches on
a key-frame boundary. Board validation confirmed that both the automatically
completed segment and the final partial segment contain playable video and
audio. Stage 8.2 is complete. Stage 8.3 now scans only strictly named recording
files, keeps at most ten segments, reserves 512 MiB of free space, protects the
active file, and removes the oldest managed recording when a limit is reached.
Board validation confirmed normal segment promotion, clean finalization, and
removal of unused zero-byte pending segments. Stage 8.3 is complete; the
configured ten-file deletion limit remains part of the long-duration test.
Stage 9.1 adds an isolated AWNN/NPU single-frame self-test for `yolov8n.nb`.
It accepts 320x320 NV12 input, decodes the fixed 84x2100 output tensor without
OpenCV, retains the COCO person class, and performs NMS. A generated black
frame can validate model loading and NPU execution before the real-time VI
path is connected. The checked-in `yolov8-nv12` conversion workspace shows
that the NBG model contains a 320x320 NV12 preprocessing node. Real-image,
repeated-run, and existing-feature regression validation have passed on the
isolated `feature/yolov8-edge-validation` branch. Its cleaned final snapshot
has been merged into `main`; Stage 9.1 is complete, and Stages 9.2 through 9.7
continue directly on `main`.

Run the Stage 9.1 NPU self-test on the board:

```sh
./sample_strip --npu-self-test
./sample_strip --npu-self-test /mnt/UDISK/yolov8n.nb \
    /mnt/UDISK/npu_test_320x320.nv12
```
Build in a Linux environment:

```sh
cd sample_demo
sh build.sh
```

Build only the application:

```sh
./build.sh
```

Build the application, install it as `/usr/bin/sample` in the bundled Tina
rootfs, and generate a new firmware image:

```sh
./build.sh firmware
```

The resulting image is written to `output/tina_ipc_uart0.img`. OpenCV and other
shared libraries are supplied by `/usr/lib` in the firmware rootfs, matching
the deployment model of the original project. The standalone executable is not
intended to run directly from `/mnt/UDISK` on firmware without those libraries.

## Local SDK layout

```text
sdk/
|-- toolchain/          ARM musl cross compiler
|-- aw_pack_src/        MPP libraries and Tina firmware packaging inputs
|-- share_include/      third-party public headers
`-- share_lib/          target shared libraries used while linking
```
