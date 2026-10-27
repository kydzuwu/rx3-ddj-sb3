#!/usr/bin/env python3
"""Launch the RX3 Linux prototype on macOS. No privileged host operations."""
import argparse
import fcntl
import os
import platform
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def detect_usb():
    volumes = Path("/Volumes")
    if not volumes.is_dir():
        return None
    candidates = [volume for volume in volumes.iterdir()
                  if (volume / "PIONEER/rekordbox/export.pdb").is_file()]
    if len(candidates) > 1:
        raise SystemExit("Multiple rekordbox USBs are mounted. Choose one with --usb-directory.")
    return candidates[0] if candidates else None


def command(headless=False, usb_directory=None, audio="muted", record_audio=None, windowed=False, native_guest=False, latency="responsive", display_quality="smooth"):
    qemu = shutil.which("qemu-system-aarch64" if native_guest else "qemu-system-arm")
    if not qemu:
        raise SystemExit("QEMU is missing. Install it using brew install qemu.")
    build = REPO / "work/macos"
    suffix = "-native" if native_guest else ""
    kernel = "vmlinuz" + suffix
    initrd = "initrd" + suffix + ".gz"
    for name in (kernel, initrd):
        if not (build / name).is_file():
            raise SystemExit("Build the guest first with macos/build_initramfs.py.")
    # fsdev option strings treat commas specially; reject rather than misparse.
    if "," in str(REPO):
        raise SystemExit("The prototype directory cannot contain commas.")
    backend = "none,id=prototype"
    responsive = latency == "responsive"
    usb_buffer = 4096 if responsive else 16384
    if audio == "coreaudio":
        geometry = "timer-period=1000,out.buffer-length=5000,out.buffer-count=2" if responsive else "out.buffer-length=20000,out.buffer-count=4"
        backend = "coreaudio,id=prototype,out.frequency=48000,out.channels=2,out.format=s16," + geometry
    if record_audio:
        recording = Path(record_audio).expanduser().resolve()
        if recording.exists() or not recording.parent.is_dir() or "," in str(recording):
            raise SystemExit("Choose a new WAV path in an existing directory, without commas.")
        backend = f"wav,id=prototype,path={recording},out.frequency=44100,out.channels=2,out.format=s16"
    options = [qemu, "-name", "XDJ-RX3 Mac Prototype", "-machine", "virt,highmem=off", "-cpu", "host" if native_guest else "cortex-a15",
            "-accel", "hvf" if native_guest else "tcg,thread=multi,tb-size=512", "-smp", "4", "-m", "2048",
            "-kernel", str(build / kernel), "-initrd", str(build / initrd),
            "-append", "console=ttyAMA0 loglevel=4 video=Virtual-1:1280x800-32@60 rx3_latency=" + latency,
            "-device", "virtio-gpu-pci", "-device", "qemu-xhci,id=usb,p2=8,p3=8",
            "-device", "usb-tablet", "-device", "usb-kbd",
            "-device", "virtio-serial-pci,id=control",
            "-chardev", f"socket,id=midi,path={build / 'midi.sock'},server=on,wait=off",
            "-device", "virtserialport,bus=control.0,nr=1,chardev=midi,name=flx4",
            "-audiodev", backend, "-device", f"usb-audio,audiodev=prototype,multi=off,buffer={usb_buffer}",
            "-fsdev", f"local,id=source,path={build / 'share'},security_model=none,readonly=on",
            "-device", "virtio-9p-pci,fsdev=source,mount_tag=source",
            "-display", "none" if headless else
            "cocoa,zoom-to-fit=on,zoom-interpolation=" + ("on" if display_quality == "smooth" else "off") +
            ",full-screen=" + ("off" if windowed else "on"),
            "-qmp", f"unix:{build / 'qmp.sock'},server=on,wait=off",
            "-serial", "stdio", "-monitor", "none", "-no-reboot", "-nic", "none"]
    if usb_directory:
        directory = Path(usb_directory).expanduser().resolve()
        if not (directory / "PIONEER/rekordbox/export.pdb").is_file():
            raise SystemExit("Choose a rekordbox export root containing PIONEER/rekordbox/export.pdb.")
        if "," in str(directory):
            raise SystemExit("The USB directory cannot contain commas.")
        options += ["-fsdev", f"local,id=usbfiles,path={directory},security_model=none,readonly=on",
                    "-device", "virtio-9p-pci,fsdev=usbfiles,mount_tag=usb1",
                    "-drive", f"if=none,id=media,file={build / 'media-id.img'},format=raw,readonly=on",
                    "-device", "usb-storage,bus=usb.0,drive=media"]
    return options


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--headless", action="store_true")
    architecture = parser.add_mutually_exclusive_group()
    architecture.add_argument("--native-guest", dest="native_guest", action="store_true",
                              help="Use hardware-accelerated ARM64 Linux (default on Apple silicon)")
    architecture.add_argument("--arm32-guest", dest="native_guest", action="store_false",
                              help="Use the slower ARM32 system-emulation fallback")
    parser.set_defaults(native_guest=platform.machine() == "arm64")
    parser.add_argument("--no-midi", action="store_true")
    parser.add_argument("--windowed", action="store_true", help="Use a window instead of fullscreen")
    parser.add_argument("--display-quality", choices=["smooth", "pixel"], default="smooth",
                        help="Smooth scales the firmware UI with interpolation; pixel retains nearest-neighbor scaling")
    parser.add_argument("--no-usb", action="store_true", help="Skip automatic USB library detection")
    parser.add_argument("--usb-directory", help="Read-only rekordbox export folder or mounted USB root")
    parser.add_argument("--audio", choices=["muted", "coreaudio"], default="muted",
                        help="Optional stereo master on the Mac's default output; cue output is unavailable")
    parser.add_argument("--latency", choices=["responsive", "stable"], default="responsive",
                        help="Responsive uses smaller audio buffers; stable retains the previous playback settings")
    parser.add_argument("--record-audio", help="Capture stereo master to a new WAV file instead of speakers")
    args = parser.parse_args()
    if args.native_guest and platform.machine() != "arm64":
        parser.error("The accelerated guest requires Apple silicon; use --arm32-guest on Intel Macs.")
    if args.no_usb and args.usb_directory:
        parser.error("--no-usb and --usb-directory cannot be used together")
    usb_directory = args.usb_directory or (None if args.no_usb else detect_usb())
    options = command(args.headless, usb_directory, args.audio, args.record_audio, args.windowed, args.native_guest, args.latency, args.display_quality)
    lock = (REPO / "work/macos/launch.lock").open("w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise SystemExit("The prototype is already running. Stop it before launching again.")
    print(("Starting accelerated ARM64 guest with ARM32 firmware. " if args.native_guest else
           "Starting ARM32 system-emulation fallback. ") +
          ("Recording stereo master to WAV." if args.record_audio else
           "Stereo master uses the Mac default output." if args.audio == "coreaudio" else
           "Audio is muted; use --audio coreaudio for stereo master."), flush=True)
    if usb_directory:
        print(f"USB library (host read-only): {Path(usb_directory).resolve()}", flush=True)
    print(f"Audio latency profile: {args.latency}", flush=True)
    if not args.headless:
        print(f"Display scaling: {args.display_quality} (firmware renders at 1280 x 800)", flush=True)
    (REPO / "work/macos/share/usb-label").write_text(
        Path(usb_directory).resolve().name if usb_directory else "RX3 Mac USB")
    bridge = None
    log = None
    if not args.no_midi:
        native = REPO / "work/macos/midi-host"
        if not native.is_file():
            raise SystemExit("Build macos/midi_host.c first, or launch with --no-midi.")
        log = (REPO / "work/macos/midi-host.log").open("a")
        bridge = subprocess.Popen([str(native), str(REPO / "work/macos/midi.sock")], stdout=log, stderr=log)
    try:
        environment = os.environ.copy()
        if args.audio == "coreaudio" and not args.record_audio:
            adapter = REPO / "work/macos/coreaudio-adapter.dylib"
            if not adapter.is_file():
                raise SystemExit("Build macos/coreaudio_adapter.c before launching audio.")
            environment["DYLD_INSERT_LIBRARIES"] = str(adapter)
        subprocess.run(options, check=True, env=environment)
    except KeyboardInterrupt:
        pass
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"QEMU exited with status {error.returncode}; see the messages above.") from None
    finally:
        if bridge is not None:
            bridge.terminate()
            bridge.wait(timeout=5)
        if log is not None:
            log.close()
        lock.close()


if __name__ == "__main__":
    main()
