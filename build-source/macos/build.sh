#!/bin/bash
# Build local proprietary assets separately from distributable source code.
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
command -v qemu-system-arm >/dev/null || { echo 'Install QEMU: brew install qemu'; exit 1; }
command -v clang >/dev/null || { echo 'Install Apple command line tools: xcode-select --install'; exit 1; }
docker info >/dev/null 2>&1 || { echo 'Start Docker Desktop before building.'; exit 1; }
python3 macos/download_assets.py "$@"
docker build -t rbpi4b-build:macos tools/build-toolchain
docker build -t rbpi4b-macos-tools:dev macos
mkdir -p work/macos
docker run --rm --network none --mount "type=bind,source=$PWD,target=/src" \
    rbpi4b-macos-tools:dev python3 macos/extract_assets.py
docker run --rm --network none --mount "type=bind,source=$PWD,target=/src" \
    rbpi4b-macos-tools:dev bash macos/build_runtime.sh
docker run --rm --network none --mount "type=bind,source=$PWD,target=/src" \
    rbpi4b-macos-tools:dev sh -c \
    'arm-linux-gnueabi-gcc -static -Wall -Wextra -Werror macos/midi_guest.c -o work/macos/midi-guest && arm-linux-gnueabi-gcc -static -Wall -Wextra -Werror scripts/device/timeout.c -o work/macos/timeout'
docker run --rm --network none --mount "type=bind,source=$PWD,target=/src" \
    rbpi4b-macos-tools:dev sh -c \
    'arm-linux-gnueabi-gcc -Wall -Wextra -Werror -O2 -march=armv5t -mfloat-abi=soft -o work/macos/test-gpio macos/test_gpio_reuse.c -Lscripts/shims/compat -Lextracted/XDJRX3-rootfs/lib -Lextracted/XDJRX3-rootfs/usr/lib -Wl,-rpath-link,extracted/XDJRX3-rootfs/lib:extracted/XDJRX3-rootfs/usr/lib && qemu-arm -L extracted/XDJRX3-rootfs -E LD_PRELOAD=/src/scripts/shims/fbshim.so work/macos/test-gpio'
docker run --rm --network none --mount "type=bind,source=$PWD,target=/src" \
    rbpi4b-macos-tools:dev sh -c \
    'arm-linux-gnueabi-gcc -Wall -Wextra -Werror -O2 -march=armv5t -mfloat-abi=soft -o work/macos/test-vsync macos/test_vsync.c -lrt -Lscripts/shims/compat -Lextracted/XDJRX3-rootfs/lib -Lextracted/XDJRX3-rootfs/usr/lib -Wl,-rpath-link,extracted/XDJRX3-rootfs/lib:extracted/XDJRX3-rootfs/usr/lib && qemu-arm -L extracted/XDJRX3-rootfs -E FB_VSYNC_HZ=60 -E LD_PRELOAD=/src/scripts/shims/fbshim.so work/macos/test-vsync'
# Keep Linux module filenames in an archive: default macOS filesystems cannot
# distinguish, for example, xt_dscp.ko from xt_DSCP.ko.
docker run --rm --network none rbpi4b-macos-tools:dev \
    tar -C /opt/guest-kernel/root -cf - boot lib/modules > work/guest-kernel.tar
docker run --rm --network none rbpi4b-macos-tools:dev \
    cat /opt/media-id.fat > work/macos/media-id.fat
python3 macos/build_media.py
clang -Wall -Wextra -Werror -Wno-deprecated-declarations macos/midi_host.c \
    -framework CoreMIDI -framework CoreFoundation -o work/macos/midi-host
clang -Wall -Wextra -Werror macos/test_midi_output.c -o work/macos/test-midi-output
work/macos/test-midi-output
clang -Wall -Wextra -Werror -Wno-deprecated-declarations -dynamiclib \
    macos/coreaudio_adapter.c -framework CoreAudio -o work/macos/coreaudio-adapter.dylib
clang -Wall -Wextra -Werror macos/test_stereo_map.c -o work/macos/test-stereo-map
work/macos/test-stereo-map
clang -Wall -Wextra -Werror macos/test_input_wire.c -o work/macos/test-input-wire
work/macos/test-input-wire
clang -Wall -Wextra -Werror -Wno-deprecated-declarations macos/test_midi_host.c \
    -framework CoreMIDI -framework CoreFoundation -o work/macos/test-midi-host
clang -Wall -Wextra -Werror -Wno-deprecated-declarations macos/test_midi_feedback.c \
    -framework CoreMIDI -framework CoreFoundation -o work/macos/test-midi-feedback
python3 macos/build_initramfs.py
if [ "$(uname -m)" = arm64 ]; then
    bash macos/build_native_guest.sh
fi
echo 'Build complete. Open macos/Launch RX3 Prototype.command.'
