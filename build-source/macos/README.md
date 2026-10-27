# Experimental macOS RX3 prototype

This runs the XDJ-RX3 v1.20 ARM32 Linux player in QEMU on macOS. Apple silicon
uses a hardware-accelerated ARM64 Linux guest with ARM32 user-space translation.
The rbpi4b shims supply the firmware's display and controller interfaces;
native guest helpers handle ALSA and evdev operations that the user translator
cannot perform. This is a Linux appliance, not a recompilation of the firmware.

The local build lives in `work/macos/`. Firmware, extracted files, keys and
binaries remain ignored by Git. Do not redistribute them with the source.

## Verified on this Mac

| Feature | Observed result |
| --- | --- |
| RX3 display | Original firmware interface renders at 1280 × 800 |
| Display performance | Approximately 58–59 frames/second with loaded-track playback |
| Fullscreen | QEMU reports fullscreen and zoom-to-fit enabled; smooth scaling is the launcher default |
| Physical DDJ-FLX4 | CoreMIDI detects it; real notes and CCs reach the guest |
| Mouse / trackpad | Visible cursor; clicking USB1 opens the library |
| Keyboard | SOURCE, playlist, load and play keys verified in the accelerated guest |
| CFX | Physical left knob activates Filter in the live engine; both channels pass activation regression checks |
| USB library | `Sam_Scott` imports 2,569 tracks and 13 playlists on a clean boot |
| Playlist browser | Exported playlist names and track lists appear |
| Track loading | “I Adore You (Extended Mix)” loads on deck 1 |
| Waveforms | Detailed waveform, overview, beat markers and phrase colors appear |
| Playback engine | Nonzero master PCM reaches the emulated USB audio device |
| Audio timing | Native audio clock reports zero underrun recoveries in sustained testing |

The operator confirmed smooth audible playback with the previous stable audio
profile. The responsive profile has negotiated a 1,027-frame native ring and
240-frame CoreAudio callbacks on the FLX4, with zero helper recoveries observed
during the initial idle-engine run; loaded-track scratching still needs operator
validation. Round-trip latency,
every controller mapping, physical LED behavior and separate headphone output have not
been validated. Audio uses stereo CoreAudio on the Mac's default output. The
CoreAudio adapter places stereo master into channels 1/2 of an interleaved
four-channel FLX4 output, keeping channels 3/4 silent.

## Launch the existing build

The **RX3 Prototype** Dock shortcut opens the same fullscreen player. Clicking it
while this prototype is running brings its QEMU window forward. On a new launch,
Terminal opens for guest diagnostics, and FLX4 and the exported USB are detected
automatically. The installed app is in `~/Applications/RX3 Prototype.app`; it
references this checkout, so reinstall it if the checkout moves.

To build and install the shortcut again:

```sh
python3 macos/build_dock_launcher.py
python3 macos/install_dock_launcher.py
```

The installer preserves the existing Dock items and saves the original Dock
preferences in `work/macos/dock-before-rx3-*.plist` before adding the shortcut.

Double-click **Launch RX3 Prototype.command** in this directory. It opens a
fullscreen QEMU view and a Terminal with guest diagnostics. Use `--windowed`
for a regular window. Wait for the RX3 interface and USB counts to populate.
The double-click launcher uses stereo master output
on the Mac's default sound device. The Python CLI is muted unless `--audio
coreaudio` is supplied.

For stereo master output on the Mac's default sound device:

```sh
cd /Users/sami.md/Documents/ChatGPT/AE/rbpi4b-macos
python3 macos/launch.py --audio coreaudio
```

Apple silicon automatically uses the accelerated guest (four virtual CPUs and
2 GiB RAM). `--native-guest` explicitly selects it. `--arm32-guest` selects the
older, substantially slower system-emulation fallback, which is also the
default on Intel Macs. Both images are produced by the Apple silicon build.
Docker does not run during playback.

To read a rekordbox-exported USB or folder:

```sh
python3 macos/launch.py --usb-directory "/Volumes/YOUR_USB" --audio coreaudio
```

Select the **export root**, containing `PIONEER/rekordbox/export.pdb` and the
audio/analysis files. A directory of unexported audio alone is insufficient.
Keep the USB attached while the prototype runs. QEMU shares this directory
read-only; the guest presents it to the firmware at `/media/usb1/sda1`.
The firmware requires database writes even for browsing. A guest tmpfs overlay
receives those writes; the original host folder remains read-only. Changes made
in the player, including new cues and settings, are discarded when the VM stops.
Only one export folder is currently supported. A small emulated USB disk with
an empty FAT partition satisfies the firmware's media-format probe; audio and
database contents come from the folder share. The launcher automatically chooses
a mounted USB containing `PIONEER/rekordbox/export.pdb`. If more than one is
mounted, select one with `--usb-directory`. Use `--no-usb` to skip detection.
The SOURCE screen's capacity/free-space values describe the guest overlay,
rather than the physical USB's capacity.

To browse the USB with the FLX4:

1. Hold SHIFT and press the browse knob to open SOURCE.
2. Select your USB and press the browse knob to open it.
3. Open PLAYLIST on the screen (or press `8` on the keyboard).
4. Browse to a playlist, press the knob, choose a track, then press LOAD 1 or
   LOAD 2. Use the corresponding PLAY button.

Wait for the source's song and playlist counts to populate after startup.

Click inside the QEMU window to use keyboard controls:

| Action | Key |
| --- | --- |
| SOURCE / BROWSE / PLAYLIST | 5 / 6 / 8 |
| Select a row / open it | Up or Down / Enter |
| Back | Backspace |
| Load deck 1 / deck 2 | 1 / 2 |
| Play deck 1 / deck 2 | Space / N |
| Cue deck 1 / deck 2 | Z / M |
| Sync deck 1 / deck 2 | X / comma |

Connect a DDJ-FLX4 to the Mac by USB for MIDI input. The native CoreMIDI bridge
automatically finds an input source whose display name contains `FLX4` or
`FLX-4`, including when it is connected after launch. SHIFT + browse-knob push
opens SOURCE; the browse knob, LOAD and deck buttons use the upstream FLX4 map.
The 1/2 and 2x buttons resize the active loop on their respective deck.
4 BEAT / EXIT starts a fresh four-beat loop or exits an active loop, preserving
the current pad mode. SHIFT + 1/2 opens playlists; use the browse knob to select
and push to enter. Backspace returns to the previous browser level.
See [upstream mapping details](../docs/15-flx4-midi.md), particularly pad modes:
the four mode buttons follow RX3 positions, which differ from the FLX4 labels.

The accelerated guest uses a bidirectional virtio port: input reaches the FLX4
mapper, and the firmware's LED state returns through CoreMIDI to the FLX4 output.
The bridge automatically reconnects both endpoints. LED-off messages retain
Note On with velocity zero, and the FLX4 keepalive is sent every 200 ms.
The older guest fallback remains input-only. This bypasses unsupported ALSA
sequencer ioctls. Physical browse and channel-fader messages have been observed during
playback. For audible master, raise the playing deck's channel fader, put the
crossfader where that deck is audible, and set the FLX4's master level.

Mouse/trackpad clicks use the firmware touch controls, with a visible cursor.
A native evdev helper normalizes the virtual tablet's coordinates and serializes
the ARM64 kernel's 24-byte input records into the ARM32 firmware's 16-byte format.
The keyboard uses the same record conversion on a separate FIFO. Keyboard and
pointer helpers read only QEMU's virtual devices inside the guest.

The emulated sound card has two channels, so headphone pairs are dropped. This
does not implement independent FLX4 headphone cue routing. The complete control
map and physical LEDs still require validation. The live guest-to-CoreMIDI test
received 3,293 LED notes (623 on, 2,670 off) and 45 FLX4 keepalives; no physical
FLX4 MIDI endpoints were available during that test. This remains a development prototype.

## Audio and display timing

The original firmware renders a **1280 × 800** interface. Fullscreen now uses
Cocoa's zoom interpolation to smooth its enlargement on the Mac display.
This is presentation scaling, not higher-resolution firmware rendering; merely
increasing the virtual monitor's resolution cannot create finer text or artwork.
`--display-quality pixel` restores the previous nearest-neighbor scaling.

The Dock launcher now defaults to the **responsive** audio profile: four native
ALSA periods (about 23 ms of ring capacity), at most two queued PCM packets,
a 4 KiB virtual USB FIFO, and a 5 ms CoreAudio buffer request with two buffers.
QEMU's audio timer runs every 1 ms. The jog's no-motion timeout is 40 ms instead
of 120 ms. These are buffer capacities and requested intervals, not a measured
controller-to-speaker latency.

If responsive mode crackles on a busy Mac, use the previous settings with:

```sh
python3 macos/launch.py --audio coreaudio --latency stable
```

The firmware generates 44.1 kHz stereo audio in 64-frame blocks. QEMU's USB
sound device uses 48 kHz. The native guest audio helper owns ALSA, resamples with
`plughw:0,0`, and uses a half-ring start threshold. Stable mode retains its
approximately 93 ms ring. A bounded Unix datagram queue blocks the producer at the actual sound
device's rate. The translated player does not independently sleep against a
wall-clock estimate, which previously drained the audio buffer over time.
The transport has a timeout so losing the helper does not hang the firmware.
This ring size is buffering capacity, not a measured end-to-end latency.

`AUDIO_SOCKET` and `MIDI_RAW_DEV` enable the optional native transports;
`KBD_RAW_DEV` selects the normalized keyboard event stream. They are unset on a
regular Pi. The Pi's existing hardware paths remain the defaults. `AUDIO_PERIODS`
can override the master ring depth for the ARM32 guest without changing Pi
defaults. `FB_VSYNC_HZ=60` supplies display pacing; `FB_VSYNC_RATE=1` reports
measured frame rate. The Mac guest disables unnecessary framebuffer rotation
and replaces the touch menu's 0.1 ms polling with 2 ms polling.

The FLX4's CFX knobs select Filter on their own channel when the live mixer
reports no active Color FX. This avoids startup key pulses leaving both inputs
Off, while retaining a different effect if one has been explicitly selected.
Center is neutral, left is low-pass and right is high-pass. For a read-only
diagnostic inside the accelerated guest, run `/source/inspect-cfx PLAYER_PID`;
it reports the actual engine type and knob value (Filter is type 5). After the
fix, real left-knob input changed the engine from type 0 to type 5 and updated
its Color value; audio reported no underrun recoveries while Filter was active.

Stop it by closing QEMU or double-clicking **Stop RX3 Prototype.command**.
An instance lock prevents a second launcher from replacing a running VM's
sockets. Docker is only needed for builds, not launches. The VM has no external
network adapter; localhost is enabled for its database service.

## Rebuild

Requirements: macOS, Python 3, Apple command-line tools, Docker Desktop running,
and QEMU (`brew install qemu`). The prototype was brought up on Apple silicon;
Intel macOS has not been tested.

```sh
bash macos/build.sh
```

The script downloads the official RX3 v1.20 update and AlphaTheta's published
RX3 GPL archives, plus pinned DirectFB/Flux sources and Debian's ARM installer
initramfs for both guest architectures. It finds the matching published key locally, verifies the stock player
hash, applies the upstream patches, and builds a guest with matching Debian
kernel modules. Compilation/extraction containers have networking disabled.
Docker image construction needs network access when dependencies are missing.

`downloads.json` pins the SHA-256 of each input used for this build. The Debian
initramfs digest was checked against Debian's SHA256SUMS; other digests pin the
bytes retrieved from the linked publishers. A publisher replacing an artifact
causes the build to stop. Inspect and deliberately update the manifest instead
of bypassing that check. `bash macos/build.sh --offline` prevents build-input
downloads; Docker may still need network access to build its images.

Kernel modules stay in a tar archive until guest assembly because macOS's usual
case-insensitive filesystem cannot preserve all Linux module names. The
`highmem=off` QEMU machine setting is required by the fallback's 32-bit kernel.
The native build keeps each ARM64 kernel and its generated module indexes
together, and installs a static ARM32 interpreter through `binfmt_misc` so it
also works inside the firmware chroot.

## Diagnostics

```sh
work/macos/midi-host --list
python3 macos/qmp.py status
python3 macos/qmp.py screenshot work/macos/screen.png
python3 macos/qmp.py quit
```

Host bridge connection status is logged in `work/macos/midi-host.log`. Inside the guest's
Terminal shell, inspect `/tmp/rbp.log`, `/tmp/knobshim.log`,
`/tmp/macos-midi.dump`, `/tmp/native-audio.log`, `/tmp/native-pointer.log`,
`/tmp/native-keyboard.log`, `/tmp/pointsrc.log`, and `/tmp/edb_daemon_log`.
The fallback additionally uses `/tmp/midi-bridge.log`.
Guest files and settings are discarded when the VM stops.

`midi-host --list` lists both input and output endpoints. If neither FLX4 endpoint
appears, reconnect the controller through its PC/Mac USB-C port. The host log
reports when the guest feedback stream becomes active and the FLX4 output connects.

`test_midi_host.c` creates a temporary, clearly named virtual MIDI source and
sends a SOURCE press/release. It never opens a physical MIDI output destination.
`test_midi_output.c` checks fragmented MIDI, velocity-zero LED off, running status,
SysEx and realtime handling. With the accelerated guest running,
`work/macos/test-midi-feedback` captures its output using a temporary virtual
CoreMIDI destination and checks LED on/off plus keepalives. It skips capture when
a physical FLX4 output is already present.
`--record-audio /absolute/new-file.wav` captures stereo master to a new WAV file
instead of playing it through speakers.

Regression checks cover the production audio packing/routing, the FLX4 mapping,
evdev hotplug and the native keyboard FIFO, pointer/cursor behavior, raw MIDI
running status and realtime interleaving, 16-byte input records and coordinate
boundaries, vsync pacing, and stereo placement into interleaved/split CoreAudio
buffers. `macos/build_native_guest.sh` rebuilds just the native guest helpers
and image after the firmware runtime has been assembled.

## Sources

- [rbpi4b](https://github.com/salesforcedoc/rbpi4b)
- [Official RX3 v1.20 update](https://downloads.support.alphatheta.com/firmwares/all-in-one-dj-systems/XDJ-RX3/XDJ-RX3_v120.zip)
- [AlphaTheta/Pioneer open-source distribution](https://www.pioneerdj.com/en/support/open-source-code-distribution/gnu-open-source-license/)
- [QEMU audio options](https://www.qemu.org/docs/master/system/invocation.html)
