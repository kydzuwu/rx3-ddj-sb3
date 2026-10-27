#!/usr/bin/env python3
"""Wrap the empty FAT identity volume in an MBR for an emulated USB disk."""
from pathlib import Path
import struct

REPO = Path(__file__).resolve().parents[1]


def main():
    fat = (REPO / "work/macos/media-id.fat").read_bytes()
    if len(fat) % 512 or fat[510:512] != b"\x55\xaa":
        raise SystemExit("Invalid FAT identity image")
    boot = bytearray(512)
    struct.pack_into("<I", boot, 440, 0x52335833)
    boot[446:462] = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", 0x0e,
                                 b"\xfe\xff\xff", 2048, len(fat) // 512)
    boot[510:512] = b"\x55\xaa"
    with (REPO / "work/macos/media-id.img").open("wb") as target:
        target.write(boot)
        target.seek(2048 * 512)
        target.write(fat)
    print("Emulated USB identity disk assembled")


if __name__ == "__main__":
    main()
