#!/bin/sh

set -eu

demo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
toolchain_prefix="$demo_dir/sdk/toolchain/toolchain-sunxi-musl/toolchain/bin/arm-openwrt-linux-muslgnueabi-"
build_mode=${1:-app}

make -C "$demo_dir" \
    APP_BIN_NAME=sample_demo \
    PRODUCT_MODEL=perf1 \
    COMPILE_PREX="$toolchain_prefix" \
    build_app

case "$build_mode" in
    app)
        ;;
    firmware)
        "$demo_dir/mk_firmware/build_part.sh" sample_demo
        ;;
    *)
        echo "Usage: $0 [app|firmware]" >&2
        exit 2
        ;;
esac
