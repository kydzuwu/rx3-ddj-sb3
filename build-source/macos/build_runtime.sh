#!/bin/bash
# Run inside rbpi4b-macos-tools, with /src mounted and networking disabled.
set -euo pipefail
cd /src
ROOTFS=/src/extracted/XDJRX3-rootfs
test -f "$ROOTFS/lib/libc.so.6"
mkdir -p work/vendor/build
cd work/vendor/build

if [ ! -d flux ]; then
    mkdir flux
    tar -xzf ../flux.tar.gz -C flux --strip-components=1
fi
if [ ! -x flux/src/fluxcomp ]; then
    (cd flux; autoreconf -fi; CXX=g++ ./configure; make -j4)
fi
export PATH="$PWD/flux/src:$PATH"

if [ ! -d directfb ]; then
    mkdir directfb
    tar -xzf ../directfb.tar.gz -C directfb --strip-components=1
    (cd directfb; patch -p1 < /src/tools/build-directfb/directfb-full.diff)
fi
cd directfb
export CC=arm-linux-gnueabi-gcc
export CXX=arm-linux-gnueabi-g++
export CFLAGS="-O2 -march=armv5t -mfloat-abi=soft --sysroot=$ROOTFS -I/usr/include/freetype2 -I/usr/include/libpng16"
export LDFLAGS="--sysroot=$ROOTFS -L$ROOTFS/lib -L$ROOTFS/usr/lib -Wl,-rpath-link,$ROOTFS/lib:$ROOTFS/usr/lib"
if [ ! -f configure ]; then
    ./autogen.sh --host=arm-linux-gnueabi --prefix=/usr --libdir=/lib \
        --disable-x11 --disable-sdl --disable-vnc --disable-avifile \
        --with-gfxdrivers=none --disable-osx --disable-devmem --disable-png --disable-jpeg
else
    ./configure --host=arm-linux-gnueabi --prefix=/usr --libdir=/lib \
        --disable-x11 --disable-sdl --disable-vnc --disable-avifile \
        --with-gfxdrivers=none --disable-osx --disable-devmem --disable-png --disable-jpeg
fi
# The unused command-line C++ tools require a second cross compiler. Build only
# runtime components; the firmware supplies the player and EDB daemon.
make -j4 TOOLS_DIR=
make install DESTDIR=/src/work/dfb TOOLS_DIR=
cd /src/work/dfb
for base in libdirect libfusion libdirectfb; do
    file="lib/$base-1.4.so.6.0.10"
    if [ -f "$file" ]; then
        patchelf --set-soname "$base-1.4.so.0" "$file"
        mv "$file" "lib/$base-1.4.so.0.0.0"
    fi
    rm -f "lib/$base-1.4.so.6" "lib/$base-1.4.so"
    ln -sfn "$base-1.4.so.0.0.0" "lib/$base-1.4.so.0"
done
for file in lib/lib*-1.4.so.0.0.0 lib/directfb-1.4-6/*/*.so; do
    for base in libdirect libfusion libdirectfb; do
        if arm-linux-gnueabi-readelf -d "$file" | grep -q "\[$base-1.4.so.6\]"; then
            patchelf --replace-needed "$base-1.4.so.6" "$base-1.4.so.0" "$file"
        fi
    done
done
cd /src
make -C scripts/shims RX3="$ROOTFS" all check
RX3=/src/extracted DFB=/src/work/dfb bash scripts/build-chroot.sh
