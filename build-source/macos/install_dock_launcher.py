#!/usr/bin/env python3
"""Install the built app and append its shortcut without replacing Dock items."""
from datetime import datetime
from pathlib import Path
import os
import plistlib
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]
BUNDLE_ID = "local.rbp.rx3-prototype.launcher"


def main():
    source = REPO / "work/macos/RX3 Prototype.app"
    pin = REPO / "work/macos/pin-dock-launcher"
    if not source.is_dir() or not pin.is_file():
        raise SystemExit("Run python3 macos/build_dock_launcher.py first.")
    applications = Path.home() / "Applications"
    target = applications / source.name
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    if target.exists():
        with (target / "Contents/Info.plist").open("rb") as stream:
            if plistlib.load(stream).get("CFBundleIdentifier") != BUNDLE_ID:
                raise SystemExit(f"An unrelated app already exists at {target}.")

    raw = subprocess.check_output(["defaults", "export", "com.apple.dock", "-"])
    backup = REPO / f"work/macos/dock-before-rx3-{timestamp}.plist"
    backup.write_bytes(raw)
    applications.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".rx3-install-", dir=applications) as temporary:
        staged = Path(temporary) / source.name
        shutil.copytree(source, staged)
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(staged)], check=True)
        if target.exists():
            shutil.move(str(target), str(REPO / f"work/macos/RX3 Launcher Backup {timestamp}.app"))
        os.replace(staged, target)

    # Always replace our tile after installation so cached bookmark references
    # cannot continue pointing at the previous bundle moved into backups.
    subprocess.run([str(pin), str(target)], check=True)
    # Register the installed path after replacing the bundle; otherwise launch
    # services can retain an alias to the previous bundle moved into backups.
    registrar = Path("/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister")
    subprocess.run([str(registrar), "-f", str(target)], check=True)
    # Dock must not flush its old in-memory app list over the freshly saved list.
    subprocess.run(["killall", "-KILL", "Dock"], check=False)
    print(f"Installed: {target}")
    print(f"Dock preferences backup: {backup}")
    print("Updated RX3 Prototype in the Dock.")


if __name__ == "__main__":
    main()
