# Third-party components and their source code

This app redistributes unmodified binaries of the following free software. Each is
licensed under the terms in this folder (GPL-2.0, LGPL-2.1 or similar).

* QEMU and its libraries (capstone, dtc, gettext, glib, gmp, gnutls, jpeg-turbo, libidn2, libpng, libslirp, libssh, libtasn1, libunistring, libusb, lzo, ncurses, nettle, openssl@3, p11-kit, pcre2, pixman, qemu, snappy, vde, zstd), built by Homebrew.
  Source: https://www.qemu.org/download/ and https://github.com/Homebrew/homebrew-core
  (each formula lists its exact source archive and checksum).
* The guest Linux kernel, modules and installer initramfs are Debian bookworm
  packages (linux-image-arm64, debian-installer). Source: https://sources.debian.org/
* qemu-arm-static (qemu-user-static) and libasound from Debian bookworm.
  Source: https://sources.debian.org/
* rbpi4b shims, launcher and helpers: MIT, see rbpi4b-LICENSE. Source is in
  Contents/Resources/build-source (shareable edition) or from the project author.

On request, the distributor of this app will provide the corresponding source code
for any GPL component above for at least three years.
