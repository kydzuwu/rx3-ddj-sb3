#!/usr/bin/env python3
"""Build the small native Dock launcher inside the ignored work directory."""
from pathlib import Path
import plistlib
import struct
import subprocess

REPO = Path(__file__).resolve().parents[1]
APP = REPO / "work/macos/RX3 Prototype.app"
CONTENTS = APP / "Contents"
RESOURCES = CONTENTS / "Resources"


def main():
    (CONTENTS / "MacOS").mkdir(parents=True, exist_ok=True)
    RESOURCES.mkdir(exist_ok=True)
    with (CONTENTS / "Info.plist").open("wb") as stream:
        plistlib.dump({
            "CFBundleDevelopmentRegion": "en",
            "CFBundleExecutable": "RX3Launcher",
            "CFBundleIdentifier": "local.rbp.rx3-prototype.launcher",
            "CFBundleName": "RX3 Prototype",
            "CFBundleDisplayName": "RX3 Prototype",
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": "1.1",
            "CFBundleVersion": "2",
            "LSMinimumSystemVersion": "12.0",
            "LSUIElement": False,
            "NSHighResolutionCapable": True,
        }, stream)
    with (RESOURCES / "Prototype.plist").open("wb") as stream:
        plistlib.dump({"RepositoryPath": str(REPO)}, stream)
    cache = REPO / "work/macos/swift-module-cache"
    cache.mkdir(exist_ok=True)
    subprocess.run([
        "xcrun", "swiftc", "-O", "-framework", "AppKit",
        "-module-cache-path", str(cache), str(REPO / "macos/DockLauncher.swift"),
        "-o", str(CONTENTS / "MacOS/RX3Launcher"),
    ], check=True)
    subprocess.run([
        "xcrun", "swiftc", "-O", "-module-cache-path", str(cache),
        str(REPO / "macos/PinDockLauncher.swift"),
        "-o", str(REPO / "work/macos/pin-dock-launcher"),
    ], check=True)
    # Use QEMU's installed icon when available; no extra asset download needed.
    icon_chunks = []
    for size, kind in ((128, b"ic07"), (256, b"ic08"), (512, b"ic09")):
        icons = Path(f"/opt/homebrew/share/icons/hicolor/{size}x{size}/apps/qemu.png")
        if icons.is_file():
            png = icons.read_bytes()
            icon_chunks.append(kind + struct.pack(">I", len(png) + 8) + png)
    if icon_chunks:
        payload = b"".join(icon_chunks)
        (RESOURCES / "RX3.icns").write_bytes(b"icns" + struct.pack(">I", len(payload) + 8) + payload)
        info_path = CONTENTS / "Info.plist"
        with info_path.open("rb") as stream:
            info = plistlib.load(stream)
        info["CFBundleIconFile"] = "RX3.icns"
        with info_path.open("wb") as stream:
            plistlib.dump(info, stream)
    subprocess.run(["codesign", "--force", "--sign", "-", str(APP)], check=True)
    print(APP)


if __name__ == "__main__":
    main()
