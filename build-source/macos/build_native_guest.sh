#!/bin/bash
# Called on the Mac after firmware/shims and the shared build image exist.
set -euo pipefail
cd "$(dirname "$0")/.."
docker build -f macos/Dockerfile.native-guest -t rbpi4b-native-guest:dev .
docker run --rm --network none rbpi4b-native-guest:dev \
    tar -C /opt/native-kernel/root -cf - boot lib/modules > work/native-kernel.tar
docker run --rm --network none rbpi4b-native-guest:dev \
    cat /usr/bin/qemu-arm-static > work/macos/qemu-arm-static
docker run --rm --network none --mount "type=bind,source=$PWD,target=/src" \
    rbpi4b-native-guest:dev bash macos/package_native_bridges.sh
python3 macos/build_initramfs.py --native-guest
