#!/bin/bash
# Shows rekordbox 5 the DDJ-SB3 as a DDJ-SB2. Quit the RX3 app first, then start rekordbox.
# Keep this window open while you play; close it (or Ctrl+C) to stop.
cd "$(dirname "$0")" || exit 1
if launchctl print "gui/$(id -u)/com.kydz.sb3-as-sb2" >/dev/null 2>&1; then
    echo "SB3 as SB2 already starts at login and is running. Nothing to do: just open rekordbox."
    read -r _; exit 0
fi
if pgrep -f "RX3 Prototype.app/Contents" >/dev/null; then
    echo "The RX3 app is running and also talks to the SB3. Quit it first, then run this again."
    read -r _; exit 1
fi
[ -x sb3-as-sb2 ] || clang -O2 -Wno-deprecated-declarations sb3_as_sb2.c -framework CoreMIDI -framework CoreFoundation -o sb3-as-sb2 || { read -r _; exit 1; }
exec ./sb3-as-sb2 "$@"
