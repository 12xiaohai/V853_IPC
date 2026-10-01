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
encoder pipeline. Board validation of the MP4 output is the current task.
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
