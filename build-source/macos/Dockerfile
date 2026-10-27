FROM rbpi4b-build:macos

# Build tools only. Runtime execution takes place in a separate ARM32 VM.
RUN dpkg --add-architecture armel && dpkg --add-architecture armhf \
    && apt-get update && apt-get install -y --no-install-recommends \
       g++ autoconf automake libtool pkg-config gettext git patch patchelf \
       p7zip-full python3-cryptography cpio kmod curl libfreetype-dev \
       libpng-dev libjpeg62-turbo-dev

RUN mkdir -p /opt/guest-kernel \
    && cd /opt/guest-kernel \
    && KERNEL_PACKAGE="$(apt-cache depends linux-image-armmp:armhf | sed -n 's/^  Depends: //p' | head -1)" \
    && apt-get download "$KERNEL_PACKAGE" \
    && dpkg-deb -x ./*.deb root \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# The player probes /dev/sda1 using libblkid before opening export.pdb.
# An empty FAT image supplies media identity; audio stays in the host share.
RUN apt-get update && apt-get install -y --no-install-recommends dosfstools \
    && truncate -s 16M /opt/media-id.fat \
    && mkfs.fat -F 16 -n RX3_MAC_USB -i 52335833 /opt/media-id.fat \
    && rm -rf /var/lib/apt/lists/*
