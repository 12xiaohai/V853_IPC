#!/bin/sh

set -eu

# 无论从哪个目录调用脚本，sdk_dir 都指向 build.sh 所在的项目根目录。
sdk_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_mode=${1:-app}

APP_BIN_NAME=sample

#这里不同硬件产品的型号，传递给makefile
PRODUCT_MODEL=perf1

# 工具链已随项目保存，所以不使用 Tina SDK 外部环境变量。
toolchain_dir=${sdk_dir}/sdk/toolchain/toolchain-sunxi-musl/toolchain/bin
toolchain_name=arm-openwrt-linux-muslgnueabi-

# -C 让 make 先进入项目根目录，后面的参数覆盖 Makefile 中的同名变量。
make -C "$sdk_dir" \
    APP_BIN_NAME="$APP_BIN_NAME" \
    PRODUCT_MODEL="$PRODUCT_MODEL" \
    COMPILE_PREX="${toolchain_dir}/${toolchain_name}" \
    build_app

case "$build_mode" in
    app)
        ;;
    firmware)
        # 将新程序安装到打包 rootfs，并生成可烧录固件。
        "$sdk_dir/mk_firmware/build_part.sh" "$APP_BIN_NAME"
        ;;
    *)
        echo "Usage: $0 [app|firmware]" >&2
        exit 2
        ;;
esac

