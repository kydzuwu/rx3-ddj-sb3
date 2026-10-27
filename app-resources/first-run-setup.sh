#!/bin/bash
# First launch of the shareable app: build the firmware runtime on this Mac
# from AlphaTheta's official downloads. The app ships no Pioneer/AlphaTheta files.
# Usage: first_run_setup.sh "/path/to/RX3 Prototype.app"
set -euo pipefail
APP="${1:?usage: first_run_setup.sh /path/to/RX3\ Prototype.app}"
SUPPORT="$HOME/Library/Application Support/RX3Prototype"
SRC="$SUPPORT/build-source"
export PATH="/usr/local/bin:/opt/homebrew/bin:/Applications/Docker.app/Contents/Resources/bin:$PATH"

step() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
fail() {
    printf '\n\033[31m%s\033[0m\n' "$1"
    echo "Fix the problem above, then open RX3 Prototype again to retry."
    echo "Press Return to close this window."
    read -r _
    exit 1
}
trap 'fail "Setup stopped (line $LINENO). Nothing was installed."' ERR

clear 2>/dev/null || true
cat <<'EOF'
RX3 Prototype: first-time setup

This downloads the official XDJ-RX3 v1.20 update and AlphaTheta's published
open-source archives, then builds the player for this Mac inside Docker.
The result stays on this Mac. It takes about 20-40 minutes.

Unofficial project, not affiliated with Pioneer DJ or AlphaTheta.
EOF

step "Checking this Mac"
[ "$(uname -m)" = arm64 ] || fail "This needs a Mac with Apple silicon (M1 or later)."
command -v docker >/dev/null || fail "Docker Desktop is not installed. Get it from https://www.docker.com/products/docker-desktop/"
if ! docker info >/dev/null 2>&1; then
    echo "Starting Docker Desktop..."
    open -a Docker || true
    for _ in $(seq 1 60); do docker info >/dev/null 2>&1 && break; sleep 3; done
    docker info >/dev/null 2>&1 || fail "Docker Desktop is not running. Open it, wait until it says 'Running', then retry."
fi
free_gb=$(df -g "$HOME" | awk 'NR==2 {print $4}')
[ "$free_gb" -ge 15 ] || fail "At least 15 GB of free disk space is needed (found ${free_gb} GB)."
echo "OK"

step "Copying build scripts"
mkdir -p "$SRC"
rsync -a --delete --exclude work --exclude extracted "$APP/Contents/Resources/build-source/" "$SRC/"
cd "$SRC"

step "Preparing build tools (Docker images)"
docker build -t rbpi4b-build:macos tools/build-toolchain
docker build -t rbpi4b-macos-tools:dev macos
MOUNT="type=bind,source=$SRC,target=/src"

step "Downloading official firmware and source archives"
docker run --rm --mount "$MOUNT" rbpi4b-macos-tools:dev python3 macos/download_assets.py

step "Extracting the firmware (offline)"
docker run --rm --network none --mount "$MOUNT" rbpi4b-macos-tools:dev python3 macos/extract_assets.py

step "Building the player runtime (offline)"
docker run --rm --network none --mount "$MOUNT" rbpi4b-macos-tools:dev bash macos/build_runtime.sh

step "Installing"
[ -s work/rbpi4b-pi4.tgz ] || fail "The build did not produce work/rbpi4b-pi4.tgz."
mkdir -p "$SUPPORT/share"
cp work/rbpi4b-pi4.tgz "$SUPPORT/share/rbpi4b-pi4.tgz.partial"
mv "$SUPPORT/share/rbpi4b-pi4.tgz.partial" "$SUPPORT/share/rbpi4b-pi4.tgz"
cd "$SUPPORT"
rm -rf "$SRC"
trap - ERR

step "Done"
cat <<'EOF'
Setup finished. RX3 Prototype is opening now.

Optional: to free about 5 GB, remove the build images in Docker Desktop
(Images > rbpi4b-macos-tools, rbpi4b-build).
EOF
open "$APP"
sleep 2
