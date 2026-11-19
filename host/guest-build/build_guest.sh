#!/bin/bash
# Rebuilds knobshim.so and fbshim.so (the player-side code) in Docker and installs it into the
# guest bundle. Replaces the old scripts in /tmp/sb3tools, which did not survive.
# Base: the tgz currently installed. A copy is kept in backups/ before the first change.
set -euo pipefail
export PATH="/Applications/Docker.app/Contents/Resources/bin:/usr/local/bin:/opt/homebrew/bin:$PATH"
HERE="$(cd "$(dirname "$0")" && pwd)"
SHIMS="$HERE/../RX3 Prototype.app/Contents/Resources/build-source/scripts/shims"
TGZ="$HOME/Library/Application Support/RX3Prototype/share/rbpi4b-pi4.tgz"
rm -rf "$HERE/work"; mkdir -p "$HERE/work"
cp -R "$SHIMS" "$HERE/work/shims"
cp "$TGZ" "$HERE/work/base.tgz"
docker run --rm -v "$HERE/work:/w" rbpi4b-macos-tools:dev bash -euc '
  mkdir /tmp/r; tar -xzpf /w/base.tgz -C /tmp/r
  make -C /w/shims RX3=/tmp/r/rbx3-run knobshim.so fbshim.so 2>&1 | grep -E "error|warning|rbp_stems" || true
  for so in knobshim.so fbshim.so; do
    test -s /w/shims/$so
    arm-linux-gnueabi-strip /w/shims/$so
    cp /w/shims/$so /tmp/r/rbx3-run/usr/lib/$so
    cp /w/shims/$so /tmp/r/rbx3-run/root/pdj/$so
  done
  tar -czf /w/new.tgz --numeric-owner -C /tmp/r .
'
cp "$HERE/work/new.tgz" "$TGZ.partial"; mv "$TGZ.partial" "$TGZ"
rm -rf "$HERE/work"
echo "guest installed: $(ls -lh "$TGZ" | awk '{print $5}')"
