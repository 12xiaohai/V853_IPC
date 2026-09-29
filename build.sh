#!/bin/sh

set -eu

sdk_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_mode=${1:-app}

APP_BIN_NAME=sample

#这里不同硬件产品的型号，传递给makefile
PRODUCT_MODEL=perf1

toolchain_dir=${sdk_dir}/sdk/toolchain/toolchain-sunxi-musl/toolchain/bin
toolchain_name=arm-openwrt-linux-muslgnueabi-

make -C "$sdk_dir" \
    APP_BIN_NAME="$APP_BIN_NAME" \
    PRODUCT_MODEL="$PRODUCT_MODEL" \
    COMPILE_PREX="${toolchain_dir}/${toolchain_name}" \
    build_app

case "$build_mode" in
    app)
        ;;
    firmware)
        "$sdk_dir/mk_firmware/build_part.sh" "$APP_BIN_NAME"
        ;;
    *)
        echo "Usage: $0 [app|firmware]" >&2
        exit 2
        ;;
esac

