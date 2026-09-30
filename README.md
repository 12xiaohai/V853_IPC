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
7. OSD time watermark.
8. MP4 recording and cyclic file management.
9. NPU detection, line crossing, region intrusion, and audio alarm.

## Current progress

Stage 1 initializes the MPP system, establishes the global PTS base, handles
`SIGINT`/`SIGTERM`, and shuts the platform down cleanly. Stage 2 creates the
MIPI CSI/VI capture path, continuously obtains and releases video frames in a
worker thread, and destroys the VI/ISP resources in reverse order on exit.
Stage 3 rotates each captured frame by 270 degrees with G2D and submits the
result to VO for a 480x800 LCD preview. VO release callbacks return the MMZ
output buffers to a five-frame pool.

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
