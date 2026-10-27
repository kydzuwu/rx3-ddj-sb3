#!/usr/bin/env python3
"""Build drag-and-drop RX3 Prototype apps and DMGs from an existing local build.

personal:  includes the firmware runtime. For this Mac / your own Macs only;
           it contains decrypted, patched AlphaTheta firmware. Never share it.
shareable: everything except the firmware. The first launch builds that on the
           recipient's Mac from AlphaTheta's official downloads (needs Docker).

Run `bash macos/build.sh` first. Output goes to work/macos/dist/.
"""
import argparse
import os
from pathlib import Path
import plistlib
import shutil
import subprocess

REPO = Path(__file__).resolve().parents[1]
BUILD = REPO / "work/macos"
DIST = BUILD / "dist"
QEMU = Path(shutil.which("qemu-system-aarch64") or "/opt/homebrew/bin/qemu-system-aarch64").resolve()
SYSTEM = ("/usr/lib/", "/System/")
VERSION = "1.2"
ENTITLEMENTS = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.hypervisor</key><true/></dict></plist>
"""


def run(*command, **options):
    return subprocess.run([str(part) for part in command], check=True, **options)


def output(*command):
    return run(*command, capture_output=True, text=True).stdout


def dependencies(binary):
    """Non-system libraries a Mach-O file loads, as (install name, real path)."""
    rpaths = [line.split()[1] for chunk in output("otool", "-l", binary).split("Load command")
              if "LC_RPATH" in chunk for line in chunk.splitlines() if line.strip().startswith("path ")]
    found = []
    for line in output("otool", "-L", binary).splitlines()[1:]:
        name = line.strip().split(" (compatibility")[0]
        if not name or name.startswith(SYSTEM):
            continue
        if name.startswith("@loader_path/"):
            path = binary.parent / name.removeprefix("@loader_path/")
        elif name.startswith("@rpath/"):
            candidates = [Path(r.replace("@loader_path", str(binary.parent))) / name.removeprefix("@rpath/")
                          for r in rpaths]
            path = next((c for c in candidates if c.exists()), None)
            if path is None:
                raise SystemExit(f"Cannot resolve {name} for {binary}")
        else:
            path = Path(name)
        found.append((name, path.resolve()))
    return found


def bundle_qemu(contents):
    """Copy QEMU and every Homebrew library it needs, rewritten to @rpath."""
    helpers, frameworks = contents / "Helpers", contents / "Frameworks"
    qemu = helpers / "qemu-system-aarch64"
    shutil.copy2(QEMU, qemu)
    queue, copied, formulas = [(QEMU, qemu)], {}, {"qemu"}
    while queue:
        original, target = queue.pop()
        target.chmod(0o755)
        for name, path in dependencies(original):
            if path.name not in copied:
                copied[path.name] = frameworks / path.name
                shutil.copy2(path, copied[path.name])
                queue.append((path, copied[path.name]))
                if "/Cellar/" in str(path):
                    formulas.add(str(path).split("/Cellar/")[1].split("/")[0])
            run("install_name_tool", "-change", name, f"@rpath/{path.name}", target, capture_output=True)
        if target.suffix == ".dylib":
            run("install_name_tool", "-id", f"@rpath/{target.name}", target, capture_output=True)
    run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", qemu)
    return formulas


def licenses(contents, formulas):
    target = contents / "Resources/licenses"
    target.mkdir(parents=True)
    for formula in sorted(formulas):
        cellar = Path(output("brew", "--cellar", formula).strip())
        versions = sorted(cellar.iterdir()) if cellar.is_dir() else []
        if not versions:
            continue
        for file in versions[-1].iterdir():
            if file.is_file() and file.name.upper().startswith(("COPYING", "LICENSE", "NOTICE")):
                shutil.copy2(file, target / f"{formula}-{versions[-1].name}-{file.name}")
    for file in ("LICENSE", "NOTICE.md"):
        shutil.copy2(REPO / file, target / f"rbpi4b-{file}")
    (target / "SOURCES.md").write_text(f"""# Third-party components and their source code

This app redistributes unmodified binaries of the following free software. Each is
licensed under the terms in this folder (GPL-2.0, LGPL-2.1 or similar).

* QEMU and its libraries ({", ".join(sorted(formulas))}), built by Homebrew.
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
""")


def sign(app, contents):
    entitlements = BUILD / "qemu-hypervisor.entitlements"
    entitlements.write_text(ENTITLEMENTS)
    for library in sorted((contents / "Frameworks").iterdir()):
        run("codesign", "--force", "--sign", "-", library, capture_output=True)
    run("codesign", "--force", "--sign", "-", contents / "Helpers/midi-host", capture_output=True)
    run("codesign", "--force", "--sign", "-", "--entitlements", entitlements,
        contents / "Helpers/qemu-system-aarch64", capture_output=True)
    run("codesign", "--force", "--sign", "-", app)
    run("codesign", "--verify", "--strict", "--deep", app)


def minimum_macos():
    for line in output("vtool", "-show-build", QEMU).splitlines():
        if line.strip().startswith("minos"):
            return line.split()[1]
    return "26.0"


def source_files():
    files = output("git", "-C", REPO, "ls-files", "-co", "--exclude-standard",
                   "macos", "scripts", "tools", "LICENSE", "NOTICE.md", "README.md").splitlines()
    return [f for f in files if (REPO / f).is_file()]


def build_app(edition, minimum):
    app = DIST / edition / "RX3 Prototype.app"
    if app.exists():
        shutil.rmtree(app)
    contents = app / "Contents"
    resources, runtime = contents / "Resources", contents / "Resources/runtime"
    for folder in ("MacOS", "Helpers", "Frameworks"):
        (contents / folder).mkdir(parents=True)
    (runtime / "share").mkdir(parents=True)
    with (contents / "Info.plist").open("wb") as stream:
        plistlib.dump({
            "CFBundleDevelopmentRegion": "en",
            "CFBundleExecutable": "RX3Prototype",
            "CFBundleIdentifier": "local.rbp.rx3-prototype",
            "CFBundleName": "RX3 Prototype",
            "CFBundleDisplayName": "RX3 Prototype",
            "CFBundleIconFile": "RX3.icns",
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": VERSION,
            "CFBundleVersion": "3",
            "LSArchitecturePriority": ["arm64"],
            "LSMinimumSystemVersion": minimum,
            "NSHighResolutionCapable": True,
            "NSMicrophoneUsageDescription": "QEMU's CoreAudio backend may open audio devices.",
        }, stream)
    cache = BUILD / "swift-module-cache"
    cache.mkdir(exist_ok=True)
    run("xcrun", "swiftc", "-O", "-target", f"arm64-apple-macos{minimum}", "-framework", "AppKit",
        "-module-cache-path", cache, REPO / "macos/AppLauncher.swift", "-o", contents / "MacOS/RX3Prototype")
    run("iconutil", "-c", "icns", BUILD / "RX3.iconset", "-o", resources / "RX3.icns")
    formulas = bundle_qemu(contents)
    shutil.copy2(BUILD / "midi-host", contents / "Helpers/midi-host")
    shutil.copy2(BUILD / "coreaudio-adapter.dylib", contents / "Frameworks/coreaudio-adapter.dylib")
    for name in ("vmlinuz-native", "initrd-native.gz", "media-id.img"):
        shutil.copy2(BUILD / name, runtime / name)
    shares = ["shim-env.sh", "media-id.fat", "inspect-cfx", "seqinject2"]
    if edition == "personal":
        shares.append("rbpi4b-pi4.tgz")
    for name in shares:
        shutil.copy2(BUILD / "share" / name, runtime / "share" / name)
    if edition == "shareable":
        shutil.copy2(REPO / "macos/first_run_setup.sh", resources / "first-run-setup.sh")
        for file in source_files():
            target = resources / "build-source" / file
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / file, target)
    licenses(contents, formulas)
    sign(app, contents)
    return app


def dmg(edition, app):
    staging = DIST / edition / "dmg"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir()
    shutil.copytree(app, staging / app.name, symlinks=True)
    os.symlink("/Applications", staging / "Applications")
    shutil.copy2(REPO / f"macos/dmg-readme-{edition}.md", staging / "README.md")
    image = DIST / f"RX3-Prototype-{VERSION}-{edition}.dmg"
    image.unlink(missing_ok=True)
    run("hdiutil", "create", "-volname", "RX3 Prototype", "-srcfolder", staging,
        "-fs", "HFS+", "-format", "ULFO", "-ov", image, capture_output=True)
    run("hdiutil", "verify", image, capture_output=True)
    shutil.rmtree(staging)
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("edition", choices=["personal", "shareable", "both"], nargs="?", default="both")
    args = parser.parse_args()
    for name in ("vmlinuz-native", "initrd-native.gz", "media-id.img", "midi-host",
                 "coreaudio-adapter.dylib", "share/rbpi4b-pi4.tgz"):
        if not (BUILD / name).is_file():
            raise SystemExit(f"Missing work/macos/{name}. Run bash macos/build.sh first.")
    minimum = minimum_macos()
    for edition in (["personal", "shareable"] if args.edition == "both" else [args.edition]):
        app = build_app(edition, minimum)
        print(f"{edition}: {app}\n{edition}: {dmg(edition, app)}", flush=True)


if __name__ == "__main__":
    main()
