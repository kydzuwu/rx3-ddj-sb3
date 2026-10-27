#!/usr/bin/env python3
"""Assemble a disposable Linux guest; the host source share stays read-only."""
import gzip
import argparse
from pathlib import Path
import stat
import tarfile
import shutil
from newc import Entry, read, write

REPO = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native-guest", action="store_true")
    args = parser.parse_args()
    download = REPO / ("work/downloads/debian-arm64-initrd.gz" if args.native_guest
                       else "work/downloads/debian-initrd.gz")
    entries = read(gzip.decompress(download.read_bytes()))
    entries = {n: e for n, e in entries.items() if not n.startswith("lib/modules/")}
    kernels = []
    with tarfile.open(REPO / ("work/native-kernel.tar" if args.native_guest
                             else "work/guest-kernel.tar")) as archive:
        for item in archive:
            if item.name.startswith("boot/vmlinuz-") and item.isfile():
                kernels.append((item.name, archive.extractfile(item).read()))
            if item.name.startswith("lib/modules/"):
                if item.isdir():
                    entries[item.name] = Entry(item.name, stat.S_IFDIR | 0o755)
                elif item.isfile():
                    entries[item.name] = Entry(item.name, stat.S_IFREG | 0o644, archive.extractfile(item).read())
    if len(kernels) != 1:
        raise SystemExit("Export exactly one matching guest kernel from the build container first.")
    entries["init"] = Entry("init", stat.S_IFREG | 0o755, (REPO / "macos/guest-init.sh").read_bytes())
    if args.native_guest:
        entries["bin/qemu-arm-static"] = Entry("bin/qemu-arm-static", stat.S_IFREG | 0o755,
                                               (REPO / "work/macos/qemu-arm-static").read_bytes())
        entries["native-guest"] = Entry("native-guest", stat.S_IFREG | 0o644)
        with tarfile.open(REPO / "work/native-audio.tar") as archive:
            for item in archive:
                name = item.name.removeprefix("./")
                if not name or name == ".": continue
                if item.isdir():
                    entries[name] = Entry(name, stat.S_IFDIR | 0o755)
                elif item.isfile():
                    entries[name] = Entry(name, stat.S_IFREG | item.mode, archive.extractfile(item).read())
                elif item.issym():
                    entries[name] = Entry(name, stat.S_IFLNK | 0o777, item.linkname.encode())
    midi = REPO / "work/macos/midi-guest"
    if midi.exists():
        entries["bin/midi-guest"] = Entry("bin/midi-guest", stat.S_IFREG | 0o755, midi.read_bytes())
    timeout = REPO / "work/macos/timeout"
    if timeout.exists():
        entries["bin/timeout"] = Entry("bin/timeout", stat.S_IFREG | 0o755, timeout.read_bytes())
    output = REPO / "work/macos"
    output.mkdir(parents=True, exist_ok=True)
    share = output / "share"
    share.mkdir(exist_ok=True)
    for source in (REPO / "work/rbpi4b-pi4.tgz", REPO / "scripts/shims/seqinject2",
                   REPO / "macos/shim-env.sh", REPO / "work/macos/media-id.fat"):
        if source.is_file():
            shutil.copy(source, share / source.name)
    suffix = "-native" if args.native_guest else ""
    (output / ("initrd" + suffix + ".gz")).write_bytes(gzip.compress(write(entries.values()), compresslevel=3, mtime=0))
    (output / ("vmlinuz" + suffix)).write_bytes(kernels[0][1])
    print(f"Guest assembled: {kernels[0][0]}, {len(entries)} archive entries")


if __name__ == "__main__":
    main()
