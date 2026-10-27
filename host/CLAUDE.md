# RX3 Prototype - DDJ-SB3 support (handoff)

App: `RX3 Prototype.app` runs XDJ-RX3 firmware in QEMU. The guest expects a DDJ-FLX4;
`midi-host` (midi_host.c + sb3_map.h) translates DDJ-SB3 MIDI <-> FLX4 MIDI.
Source of truth for both sides:
- SB3: ../ddj-sb3-midi.pdf (no pdftotext here; extract with PDFKit, e.g. /tmp/sb3tools/pdftext.swift)
- FLX4 as the guest reads it: `RX3 Prototype.app/Contents/Resources/build-source/scripts/shims/map_flx4.c`
  (N_* notes, CC_* numbers, LED table incl. meter = CC 2 on ch 0/1, values 0x26-0x7F)

Build + launch: `./"Rebuild and Launch SB3.command"` (builds midi-host and coreaudio-adapter.dylib,
copies into the bundle, signs qemu WITH hvf-entitlements.plist - never `codesign --deep`, it strips the
hypervisor entitlement and the app silently quits). Logs: ~/Library/Logs/RX3 Prototype/ (midi-host.log, console.log).
The script launches with `open --env RX3_MIDI_DEBUG=1`, so midi-host.log has every SB3->guest and guest->SB3 message.
Tests: `test_sb3_map.c` (run by the script); phantom filter: build a file with `#define main host_main` + `#include "midi_host.c"`.
Originals of edited files: *.bak.

## Done (untested on hardware)
- SB3 buttons are printed IN / OUT (1/2X, 2X are the shift labels): 18 -> LOOP IN (16), 19 -> LOOP OUT (17),
  SHIFT 97 -> CALL < (81), 98 -> CALL > (83); LEDs 16/17 -> 18/19. SHIFT+AUTO LOOP -> PLAYLIST (62)
- Note-off LED messages now converted to note-on vel 0; SYNC long (92) LED allowed; 4BEAT LED -> AUTO LOOP LED
- 14-bit LSBs passed (deck 36/39/43/47/51; ch6 55/56/45/63); SHIFT+PLAY/CUE (71/72); HP MIX CC 12/44
- stereo_map.h: stereo voice -> master copied to 3/4; multichannel voice mapped straight

- SB3 LEDs stay dark until init SysEx `F0 00 20 7F 03 01 F7` (from Mixxx DDJ-SB3 script; not in the PDF).
  midi-host sends it + `9B 09 7F` (request knob/fader positions) when the SB3 output connects.
  Raw PDF LED messages without init: nothing lit (tested). Meter = Bn 02 on ch 0/1 only.
- FX FADE / PAD SCRATCH pads (ch 7/8 notes 16-47) make the SB3 emit fake filter, fader, jog/backspin
  messages (captured). midi-host drops that deck's fader/jog/filter/xfader while pads held + 1.5 s, then re-requests positions.
- Captured on hardware: crossfader B6 1F (MSB only, full range), faders, SYNC, jog, loops all arrive correctly.

## Open
- Verify in the app after rebuild: LEDs (init SysEx), meters, crossfader effect, headphones
- Real headphone cue mix (2026-10-27), awaiting hardware test (MASTER CUE / CH CUE must change phones).
  First try (one 6-ch usb-audio card, multi=on) gave NOISE: QEMU's mixeng is mono/stereo only
  (audio_template.h `nchannels == 2` tables), and mixing-engine=off skips resampling (SB3 = 44.1 only).
  Working design: two stereo cards.
  guest rbp -> audioshim (4 ch: 0/1 master, 2/3 phones from AUDIO_MAP) -> native-audio splits into
  plughw:<port 3 card> (master) and plughw:<port 4 card> (phones); guest-init finds the cards by
  "usb-...-3," / "-4," in /proc/asound/cards, falls back to 2 ch without the phones card.
  AppLauncher: audiodev prototype (buffer-length 5000) + usb-audio port=3; audiodev phones
  (buffer-length 6000) + usb-audio port=4. Adapter tags the voice whose BufferFrameSize request is
  >= 240 frames as phones (then forces the first requested size for both), master -> SB3 ch 1/2,
  phones -> 3/4 (master -> 3/4 only while no phones voice). HAL gives each IOProc its own zeroed
  buffer and mixes them (checked with /tmp/sb3tools/scripts/twoproc.c).
  Adapter also: reports DeviceIsRunning = 0 to QEMU (else the second voice never starts), stops
  before destroy, retries AudioDeviceStart ('what' right after relaunch).
  console.log: "master voice ... 4 device channels", "headphone cue voice ...",
  "RBPI4B_MACOS: audio master card 0, phones card 1, 4 channel(s)".
  Changed: audioshim.c, native_audio.c, guest-init.sh (bundle build-source), AppLauncher.swift
  (rebuilt with xcrun swiftc -target arm64-apple-macos26.0 -> Contents/MacOS/RX3Prototype).
  Guest binaries built in Docker image rbpi4b-macos-tools:dev; work area /tmp/sb3tools/gb
  (build.sh, na.sh, repack.sh), initrd patched by /tmp/sb3tools/scripts/patch_initrd.py (newc.py)
  starting from ../initrd-native.gz.orig.
  Backups: ../initrd-native.gz.orig, ../RX3Prototype-launcher.bak48000, AppLauncher.swift.bak,
  stereo_map.h.bak, ~/Library/Application Support/RX3Prototype/rbpi4b-pi4.tgz.orig (share/ copy replaced).
- Crossfader: if MIDI arrives but no effect, check guest crossfader assign (map_flx4 has no assign handling)
- SB3 audio start can fail with CoreAudio 'what' right after a rebuild; quit + relaunch fixed it (2026-10-27).
  sb3_output_usable now retries 10x every 300 ms (logs "started on attempt N" when a retry was needed).
- Launcher binary rebuilt from AppLauncher.swift 2026-10-27 (44100, master + phones cards).
- Confirmed in app 2026-10-27: user reports everything works; HP level (B6 0D/2D), crossfader, trim, jog, cue all arrive.

## FX / filter / master cue (2026-10-27, awaiting user test)
- SB3 FX section (ch 5/6 = status 94/95) -> FLX4 Beat FX, sb3_translate now returns up to
  SB3_OUT_MAX (3) guest messages: FX1 (71) = CH SELECT this deck (off leg first: deck1
  95 11 00 + 94 10 7F, deck2 94 10 00 + 95 11 7F) then FX ON 71; SHIFT+FX1 (99) = target MASTER
  (both legs); FX2 (72) = BEAT < (94 4A); FX3 (73) = BEAT > (94 4B); SHIFT+FX2 (100) = FX type
  back (94 64); SHIFT+FX3 (101) = FX type forward (94 63). Guest FX SELECT/BEAT only listen on ch4.
  FX knob sends CC 2/4/6 (+LSBs) together; only CC 2 -> depth.
- Guest (knobshim, rebuilt in Docker with /tmp/sb3tools/gb/build2.sh + repack2.sh from
  rbpi4b-pi4.tgz.new -> .new2, installed to share/): env MASTER_CUE_START=0 (rbp_vu.c: start off,
  and hold it off ~60 s because rbp turns master cue on by itself after the mixer appears, until
  the operator presses MASTER CUE: g_master_cue_user in rbp_bridge.c); CFX_FILTER_ONLY=1
  (map_flx4.c: CFX knob movement replaces any non-Filter colour FX type with Filter).
  The flags are in shim-env.sh. The launcher copies Contents/Resources/runtime/share/* over
  ~/Library/Application Support/RX3Prototype/share/ when sizes differ, so edit the BUNDLE copy
  (runtime/share/shim-env.sh; original at ../runtime-shim-env.sh.orig). Guest source backups: *.bak in scripts/shims.
- Guest FX LED: map_flx4 .fx_ch = 4, BFX ON/OFF = 71 -> guest sends 94 47 (blinks itself when rbp
  LedStat 48 = 2, effect on). midi-host turns that into SB3 FX1 (94/95 47) blinking on the deck(s)
  of the last FX1 / SHIFT+FX1 press (fx_deck_mask), via the blink_leds() thread.
- AUTO LOOP LED (SB3 9n 14): blinked by blink_leds() while LOOP IN (16) toggles = loop active (guest sends no 77 LED).
- IN/OUT on a stopped deck (PLAY LED lit and steady = playing) -> CALL < / > (97/98 -> 81/83) = step memory cues.
- KEY LOCK (SB3 26) -> guest 26 = K_MT (master tempo); SHIFT+KEY LOCK (96) -> guest 96 = K_TEMPO_RANGE.
  map_flx4 .n_keylock = 26, so guest sends 9n 1A with the MT state; midi-host blinks it while on.

## Master on the Mac output (2026-10-27, awaiting user test)
- coreaudio_adapter.c: QEMU gets the SB3 as default for both voices; the master voice (BufferFrameSize
  < 240) is moved to the Mac output (system default, or built-in if the default is the SB3):
  buffer size, stream format (never changes the Mac rate) and the IOProc go to that device; Start/Stop/
  Destroy are mapped by IOProc id; the callback passes the SB3 id because QEMU drops other devices.
  SB3 then only carries the cue mix on 3/4 (1/2 silent). console.log: "master voice -> Mac output: ...".
- AppLauncher.swift macOutputRate(): same device choice, master out.frequency = its nominal rate and
  buffer-length = 200 frames (<= 5000 us) so 48 kHz does not cross the 240-frame phones threshold.
  Rebuilt with -framework CoreAudio. Backups: coreaudio_adapter.c.bak3, AppLauncher.swift.bak3,
  previous launcher binary /tmp/sb3tools/RX3Prototype.prev.
- FX1 LED fix: the guest's BFX LED is steady lit when Beat FX is OFF and blinks when ON, so
  midi-host counts Beat FX on only while that LED toggles (fx_prev_ms, like the loop LED).
- Browse push (ch6 65): map_flx4 flx4_note() reads rbp's browse mode (UIBROWSE_GLOBAL):
  mode 1 (deck screen) -> K_BROWSE (open browser), else K_SELECTOR. SHIFT+push (66) -> K_BACK
  (was K_SOURCE). Backup /tmp/sb3tools/map_flx4.c.before-browse.
- 2026-10-27 later: push on the deck screen -> K_SOURCE (USB chooser, user's request), not K_BROWSE;
  static source_open makes the next push SELECT (SOURCE may keep mode PLAY); SHIFT+push with
  source_open -> K_BACK. SHIFT+push in the browser: BACK; if nothing changed (top level) the release
  sends K_BROWSE to close the browser. Backup /tmp/sb3tools/map_flx4.c.before-source.
- CH CUE LED (9n 54): guest's LED 84 is swallowed, blink_leds() blinks it while on (cue_led).
- Adapter logs "<voice> peak X" every ~5 s with RX3_MIDI_DEBUG (to check silent phones).
- OPEN: SB3 headphones silent (unverified whether the cue voice carries signal; peaks were 0 with
  nothing playing). Crossfader: B6 1F reaches the guest (K_XFADER), user reports no effect - not
  yet diagnosed (guest log /tmp/knobshim.log is not reachable from the host).
