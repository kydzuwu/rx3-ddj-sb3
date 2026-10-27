#!/bin/bash
cd "$(dirname "$0")" || exit 1
APP="RX3 Prototype.app"
pkill -f "$APP/Contents" 2>/dev/null; sleep 1
echo "Testing SB3 map..."
clang test_sb3_map.c -o test_sb3_map && ./test_sb3_map || { echo "MAP TEST FAILED"; read; exit 1; }
echo "Building midi-host..."
clang -O2 midi_host.c -framework CoreMIDI -framework CoreFoundation -o midi-host || { echo "BUILD FAILED"; read; exit 1; }
cp midi-host "$APP/Contents/Helpers/midi-host"
clang -Wno-deprecated-declarations -dynamiclib coreaudio_adapter.c -framework CoreAudio -framework CoreFoundation -o coreaudio-adapter.dylib || { echo "AUDIO BUILD FAILED"; read; exit 1; }
cp coreaudio-adapter.dylib "$APP/Contents/Frameworks/coreaudio-adapter.dylib"
for lib in "$APP"/Contents/Frameworks/*; do codesign --force --sign - "$lib" 2>/dev/null; done
codesign --force --sign - "$APP/Contents/Helpers/midi-host"
codesign --force --sign - --entitlements hvf-entitlements.plist "$APP/Contents/Helpers/qemu-system-aarch64"
codesign --force --sign - "$APP"
codesign --verify --strict --deep "$APP" && echo "Signature OK"
echo "Opening RX3 Prototype..."
open --env RX3_MIDI_DEBUG=1 "$APP"
sleep 20
mkdir -p rx3-logs; cp ~/Library/Logs/"RX3 Prototype"/*.log rx3-logs/ 2>/dev/null
pgrep -fl "RX3 Prototype.app" > rx3-logs/processes.txt
echo "Logs copied to rx3-logs"
