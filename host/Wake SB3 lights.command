#!/bin/bash
# The DDJ-SB3 keeps its lights dark until it gets an init message. rekordbox does not send it for an
# SB3, so run this once after plugging in the SB3 (rekordbox can stay open).
cd "$(dirname "$0")" || exit 1
[ -x sb3-as-sb2 ] || clang -O2 -Wno-deprecated-declarations sb3_as_sb2.c -framework CoreMIDI -framework CoreFoundation -o sb3-as-sb2 || exit 1
./sb3-as-sb2 --wake
