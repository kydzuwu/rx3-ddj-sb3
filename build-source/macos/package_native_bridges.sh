#!/bin/bash
# Runs inside the ARM64 tools image, not on macOS.
set -euo pipefail
cd /src
gcc -Wall -Wextra -Werror -O2 macos/native_audio.c -lasound -o work/macos/native-audio
gcc -Wall -Wextra -Werror -O2 macos/native_input.c -o work/macos/native-input
gcc -static -Wall -Wextra -Werror -O2 macos/inspect_cfx.c -o work/macos/share/inspect-cfx
gcc -Wall -Wextra -Werror macos/test_midi_raw.c -o /tmp/test-midi-raw
/tmp/test-midi-raw
mkdir -p /tmp/native-bridges/bin /tmp/native-bridges/lib/aarch64-linux-gnu /tmp/native-bridges/usr/share
cp work/macos/native-audio work/macos/native-input /tmp/native-bridges/bin/
cp -L /lib/aarch64-linux-gnu/libasound.so.2 /lib/aarch64-linux-gnu/libm.so.6 /tmp/native-bridges/lib/aarch64-linux-gnu/
cp -r /usr/share/alsa /tmp/native-bridges/usr/share/
tar -C /tmp/native-bridges -cf /src/work/native-audio.tar .
