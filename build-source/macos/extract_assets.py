#!/usr/bin/env python3
"""Prepare locally downloaded official RX3 firmware, never print or commit keys."""
import hashlib
import io
import posixpath
from pathlib import Path, PurePosixPath
import shutil
import stat
import struct
import subprocess
import tarfile
import zlib

REPO = Path(__file__).resolve().parents[1]


def cramfs_entries(image):
    if struct.unpack_from("<I", image)[0] != 0x28CD3D45:
        raise ValueError("Not a little-endian cramfs image")
    seen = set()

    def inode(at):
        mode_uid, size_gid, name_offset = struct.unpack_from("<III", image, at)
        return mode_uid & 0xFFFF, size_gid & 0xFFFFFF, (name_offset >> 6) << 2, (name_offset & 63) << 2

    def content(size, offset):
        count = (size + 4095) // 4096
        start = offset + count * 4
        data = bytearray()
        for index in range(count):
            end = struct.unpack_from("<I", image, offset + index * 4)[0]
            if end & 0xC0000000 or end < start or end > len(image):
                raise ValueError("Unsupported or invalid cramfs block pointer")
            data.extend(zlib.decompress(image[start:end]) if end != start else bytes(4096))
            start = end
        return bytes(data[:size])

    def walk(path, at):
        mode, size, offset, _ = inode(at)
        if stat.S_ISDIR(mode):
            if not size:
                yield path, mode, b""
                return
            if offset in seen:
                raise ValueError("Directory cycle in cramfs")
            seen.add(offset)
            yield path, mode, b""
            cursor = offset
            while cursor < offset + size:
                _, _, _, namesize = inode(cursor)
                name = image[cursor + 12:cursor + 12 + namesize].rstrip(b"\0").decode()
                if name in ("", ".", "..") or "/" in name:
                    raise ValueError("Unsafe cramfs entry")
                yield from walk(path / name, cursor)
                cursor += 12 + namesize
            seen.remove(offset)
        elif stat.S_ISREG(mode) or stat.S_ISLNK(mode):
            yield path, mode, content(size, offset)
    yield from walk(PurePosixPath("."), 64)


def extract_cramfs(image, output):
    output.mkdir(parents=True, exist_ok=True)
    links = []
    for relative, mode, data in cramfs_entries(image):
        dest = output / relative
        if stat.S_ISDIR(mode):
            dest.mkdir(parents=True, exist_ok=True)
        elif stat.S_ISLNK(mode):
            links.append((dest, data.decode()))
        else:
            if dest.exists():
                dest.chmod(stat.S_IMODE(mode) | 0o200)
            dest.write_bytes(data)
            dest.chmod(stat.S_IMODE(mode))
    # Absolute Linux links remain links; no extraction writes through them.
    for dest, target in links:
        if not dest.is_symlink():
            dest.symlink_to(target)


def safe_tar_extract(path, output):
    output.mkdir(parents=True, exist_ok=True)
    links = []
    with tarfile.open(path) as archive:
        for member in archive:
            name = PurePosixPath(member.name)
            if name.is_absolute() or ".." in name.parts:
                raise ValueError("Unsafe archive path")
            dest = output / name
            if member.isdir():
                dest.mkdir(parents=True, exist_ok=True)
            elif member.isfile() or member.islnk():
                dest.parent.mkdir(parents=True, exist_ok=True)
                if dest.is_symlink():
                    raise ValueError("Refusing to write through an archive symlink")
                with archive.extractfile(member) as source, dest.open("wb") as target:
                    shutil.copyfileobj(source, target)
                dest.chmod(member.mode & 0o777)
            elif member.issym():
                target = PurePosixPath(member.linkname)
                normalized = posixpath.normpath(str(name.parent / target))
                if target.is_absolute() or normalized == ".." or normalized.startswith("../"):
                    raise ValueError("Unsafe archive symlink")
                links.append((dest, member.linkname))
        for dest, target in links:
            if not dest.is_symlink():
                dest.symlink_to(target)


def main():
    download = REPO / "work/downloads"
    for name in ("rx3-gpl-00.zip", "rx3-gpl-01.zip", "XDJ-RX3_v120.zip"):
        subprocess.run(["7z", "x", "-y", f"-o{download}", str(download / name)],
                       check=True, stdout=subprocess.DEVNULL)
    combined = download / "pioneerdj_xdj_rx3.tar.bz2"
    with combined.open("wb") as dest:
        for suffix in ("00", "01"):
            with (download / f"pioneerdj_xdj_rx3.tar.bz2.{suffix}").open("rb") as source:
                shutil.copyfileobj(source, dest)
    keypath = REPO / "work/keys/aes256.key"
    keypath.parent.mkdir(parents=True, exist_ok=True)
    if not keypath.exists():
        with tarfile.open(combined, "r|bz2") as archive:
            for member in archive:
                if not member.isfile():
                    continue
                if member.name.endswith("/aes256.key"):
                    keypath.write_bytes(archive.extractfile(member).read())
                    break
                if member.name.endswith(".cramfs"):
                    image = archive.extractfile(member).read()
                    for name, mode, data in cramfs_entries(image):
                        if name.name == "aes256.key" and stat.S_ISREG(mode):
                            keypath.write_bytes(data)
                            break
                    if keypath.exists():
                        break
                if member.name.endswith("/initramfs.tar.gz"):
                    nested = archive.extractfile(member).read()
                    with tarfile.open(fileobj=io.BytesIO(nested)) as initramfs:
                        for item in initramfs:
                            if item.isfile() and item.name.endswith("/aes256.key"):
                                keypath.write_bytes(initramfs.extractfile(item).read())
                                break
                    if keypath.exists():
                        break
    if not keypath.exists():
        raise SystemExit("No key found in the official source distribution; firmware launch is blocked.")
    keypath.chmod(0o600)
    print("Published firmware key found and stored locally.", flush=True)
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    key = keypath.read_bytes().split(b"\n", 1)[0][:31].ljust(32, b"\0")
    body = (download / "XDJRX3.UPD").read_bytes()[:-16]
    if len(body) % 512:
        raise SystemExit("Unexpected firmware sector layout")
    clear = bytearray()
    for index in range(len(body) // 512):
        iv = struct.pack("<I", index) + bytes(12)
        decryptor = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
        clear.extend(decryptor.update(body[index * 512:(index + 1) * 512]) + decryptor.finalize())
    if clear[32769:32774] != b"CD001":
        raise SystemExit("Firmware ISO validation failed; no assets were extracted")
    extracted = REPO / "extracted"
    extracted.mkdir(exist_ok=True)
    iso = extracted / "XDJRX3.iso"
    iso.write_bytes(clear)
    iso_root = extracted / "XDJRX3"
    subprocess.run(["7z", "x", "-y", f"-o{iso_root}", str(iso)], check=True, stdout=subprocess.DEVNULL)
    safe_tar_extract(iso_root / "images/pdj.tar.gz", iso_root)
    safe_tar_extract(iso_root / "images/gui.tar.gz", extracted / "XDJRX3-gui")
    player = iso_root / "pdj/rbp"
    digest = hashlib.md5(player.read_bytes()).hexdigest()
    if digest != "4f2efcfc0c9e3f539289f863acfddcc6":
        raise SystemExit(f"Player is not the expected v1.20 build: {digest}")
    extract_cramfs((iso_root / "images/rootfs.cramfs").read_bytes(), extracted / "XDJRX3-rootfs")
    subprocess.run(["python3", str(REPO / "tools/patch-rbp/rbp_patch.py"), str(player),
                    "-o", str(extracted / "rbp-audio")], check=True)
    print("XDJ-RX3 v1.20 player hash verified; local runtime assets prepared.")


if __name__ == "__main__":
    main()
