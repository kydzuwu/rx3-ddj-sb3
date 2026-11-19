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

## Set recorder (step 1 of "DJ set -> Ableton project", 2026-11-04, awaiting test with music)
Goal: the audio comes only from the RX3; Ableton's tempo follows the player's tempo master
(getSyncMaster); the other deck stays as played, offset when not beatmatched.
- Start with `RX3_RECORD=1 ./"Rebuild and Launch SB3.command"`. Without RX3_RECORD nothing changes:
  no virtio port, so the hook is never installed.
- Guest `rbp_stems.c` (in knobshim, started next to install_meter_hook): hooks MixerChannel::update
  (0x9e890), copies channel+0x20 (assumed: to master, fader + channel Beat FX) and +0x1c (assumed: before
  fader) to a per-deck ring; a thread writes to the port (nr=2 next to MIDI) and polls DjEngineIF getters
  at ~50 Hz. Buffer meanings come from the disassembly of updateFilter (0x9e3fc) and are NOT confirmed by ear.
- Mac `stem_recorder.c` -> Helpers/stem-recorder: ~/Music/RX3 Sets/<date>/deck1.wav, deck2.wav,
  deck1_prefader.wav, deck2_prefader.wav (float32, 44100, the engine's fixed rate), state.csv.
  Same frame counter for all files; dropped blocks become silence. About 1.4 MB/s while the app runs.
- state.csv values are raw: units of playing_time/bpm/grid_offset still to be read from a real recording.
- Not captured yet: master, SB3 control moves, loaded track name. rekordbox's full beat grid looks reachable
  via common::ReaderOwner::getBeatPos.
- Lead for the crossfader: setCrossFaderAssign (0x57008) exists and we never call it.
- Guest build: guest-build/build_guest.sh (replaces /tmp/sb3tools, which is gone, so the backups named
  above under /tmp are gone too). Backups: guest-build/backups.
- Launching /Applications/RX3 Prototype.app (old copy) overwrites share/shim-env.sh with the one without
  MASTER_CUE_START / CFX_FILTER_ONLY; the SB3 build restores it on its next launch.
- Test result 2026-11-04 (session 2026-11-04_153552): confirmed from the audio: decks separate (no bleed either way);
  +0x20 follows the channel fader, +0x1c does not; crossfader at 00 did not change deck 2's level in the file, so it
  looks to be applied later; position = ms and tracks audio frames; bpm_raw = BPM x 100 (not checked against screen);
  tempo_master changed by itself to deck 2 when deck 1 stopped. grid_offset_raw is 0 for all 3 tracks, so use
  getBeatPos instead. Still untested: filter, Beat FX, pitch fader, backward scratch, SYNC, LOAD from the SB3.
- Test 3, same session: filter is in deck*.wav but NOT in *_prefader.wav (t=380-382 s: high band x1.79, low band x1.00, fader
  at unity), so +0x20 = colour FX + fader and +0x1c = before colour FX. Backward motion and cue/pad jumps show in playing_time.
  Not exercised: no message at all on ch 5/6 (FX1) or CC 0 (pitch fader) in the log, so Beat FX in the file and BPM following pitch are unknown.
- Beat grid: common::BeatPosition (Player+0xc8 -> ReaderOwner, +4, +0x69c), vector<myBeat> at +0x34/+0x38, 8 bytes each:
  u16 beat in bar, u16 BPM x 100, u32 time (layout from Player::getQuantizedBpm 0x68534; field meanings and time unit not yet
  checked against audio). +0x28 is the "grid offset", 0 so far. rbp_stems.c sends it once per loaded track; grid.csv.
- Grid test (session 2026-11-04_154632, same 126 BPM track on both decks): grid.csv has 460 beats, bars 1-4, time = ms, BPM x 100 matches
  state.csv. Kick onsets at 0-10 ms from logged beats for track 0-24 s on both decks. Later onset peaks are weak and deck 1 reads
  70 ms in places; undecided. Comparison script ad hoc; grid_check_deck1.wav in the session folder = deck 1 + click on logged beats.
- Grid confirmed by ear 2026-11-04: user reports grid_check_deck1.wav clicks land on the kicks (track 0:16-0:46), so the 70 ms onset readings were
  the measurement, not the grid. Next: step 2, generate an Ableton Set from deck audio + state.csv + grid.csv.
- Step 2: set_to_ableton.py <session> -> <session>/RX3 Set Project/ (RX3 Set.als, Samples/Deck N.wav trimmed copies). Template: Live's DefaultLiveSet.als;
  clip XML from Live's own library. Fixed tempo = first master's BPM, warp markers on a straight line (rate 1, Re-Pitch), master's bar 1 on a bar.
  Live is at /Applications/Ableton Live 12 Suite.app (12.4.15). Not done: tempo changes, master changes, cue jumps as clip cuts, automation, master file.
- Step 2 status: Set loads in Live (tracks, names, clip positions OK) but clips show Sample Offline; Log.txt: `The file "Deck 1.wav" could not be opened`. Testing
  float vs PCM and relative vs plain path with ~/Music/RX3 Sets/_probe Project/probe.als. Do not `open` Sets from a script while Live has unsaved changes: it prompts the user.
- Sample Offline solved: RelativePathType 3 fails without Live's project info; float32 WAV with RelativePathType 0, empty RelativePath and the full Path loads (probe.als proved it).
- Step 2 confirmed in Live 2026-11-04: rebuilt Set plays both decks with kicks on the grid (user test, one 126 BPM track on both decks, no tempo change).
  Next, step 3: tempo follows the master deck (ignore very short master switches), clip cuts at jumps/pauses/scratches. Needs a recording with the pitch fader moved.
  Also open: portable sample links, master file, automation, GitHub repo does not have recorder or generator yet.

## REC button and step 3 (2026-11-04, awaiting test on the player)
- REC = eighth column of the touch menu band (menu_zone MZ_REC_BUTTON, pointsrc menu_fire); label becomes STOP REC. Shared flag rbp_stems_get/set in
  rbp_transport.c (both shims). rbp_stems.c sends START (4) / STOP (5) and copies audio only while on. RX3_RECORD is gone: the port and helper are always there.
- stem_recorder.c: one folder per press; on stop it runs Contents/Resources/set_to_ableton.py with /usr/bin/python3 (needs Apple's Python, Live in /Applications).
- Firmware's own REC (MasterInnards::onEv_Rec, DjEngineIF::prepareRecording) writes a master WAV to USB; our USB is read-only, so it is not used.
- set_to_ableton.py step 3: audio always at rate 1, tempo map (MASTER_HOLD_S debounce, bars 1 of master on Ableton bars, bump over gaps), clips cut at jumps and stops, scratches as own clips.
  test_set_to_ableton.py checks it on synthetic data. Pitch fader still not tested on hardware. test_menu.c not rerun with 8 columns.
- The firmware's own record dot and timer (left of INFO) are the main REC control: pointsrc rec_dot_tap swallows a tap there (x 1070-1172, y 8-38, deck screen only) and toggles the recorder;
  rbp_stems.c replaces MixerEngine::getRecordingStatus (0x57854) and getRecElapsedSec (0x578d8) so that display shows our recorder. The band's REC column does the same. Effect of status=1 on USB-removal paths untested.
- First REC recording (2026-11-04_161759_2, 169.7 s) built automatically. Clips: loop passes merged (merge_loops). Toggles within 1 s ignored (a click arrived as a burst, cause not found); recordings under 1 s leave no folder.
  Tempo bumps (110-171 BPM here) come from keeping bar 1 on an Ableton bar across gaps; audio is unaffected. Open: pitch fader and FX1 still sent no MIDI in any recording.
- Step 3 confirmed in Live 2026-11-04 by the user on session 2026-11-04_161759_2 (clips, scratches, loop clip, tempo line). Bar-accurate tempo bumps accepted for now.
  Not exercised by that recording: pitch fader (no MIDI), FX1 (no MIDI).

## Step 4 automation (2026-11-04, in progress)
- rbp_stems.c now polls MixerEngine getChannelFader, getDigitalTrim, getEqualizer (band 0-2), getSoundColorFxColor, getCrossFader; state.csv has fader,trim,eq0,eq1,eq2,color,crossfader. Ranges and band order still to read from a recording.
- Plan for the Set: bypassed Utility (Gain 0-56.2 linear), EQ Three (GainLo/Mid/Hi 0.0003-1.995), Auto Filter (Cutoff 20-135) on each deck, so lanes show the moves without applying them twice. Device XML: first FilterEQ3, AutoFilter, StereoGain in Core Library/Lessons/Sets/Live 11 Intro Empty.als; every AutomationTarget, ModulationTarget and Pointee Id must be renumbered from NextPointeeId.
- Knob test (session 2026-11-04_163629): controls read 0..1, centre 0.504. eq0 = HI (CC 7), eq1 = MID (CC 11), eq2 = LOW (CC 15), matched against the MIDI order. SB3 crossfader and TEMPO slider sent no MIDI in that session.
  Generator adds bypassed EQ Three, Auto Filter LP and HP, Utility Fader and Trim, with 7 lanes per deck. Curves in eq_gain/trim_gain/*_cutoff are approximations, not measured. Filter direction (below centre = low-pass) is assumed: audio check was inconclusive. Device XML is 12.4 schema inside a 12.1 template: needs a load check in Live.
- Live crashed (SIGSEGV, same stack, 3 reports 16:39-16:40) opening a Set with 12402-schema devices in the 12117 DefaultLiveSet. Template is now Core Library/Templates/8-Track Template.als, same schema as the devices; generator leaves lanes out if they differ. Cause inferred from the version mismatch, confirmed only if the next load works.
- Crash isolated with step-by-step Sets: only those containing the lesson Set's EQ Three crash Live (it has KeyMidi mappings and reversed ranges; the schema mismatch was not the cause). EQ Three and Utility now come from Devices/Audio Effects presets with neutral settings; Auto Filter stays the lesson copy, which loads. Lanes on top are still untested in Live.
- Step 4 confirmed in Live 2026-11-04 on session 2026-11-04_163629: Set opens, lanes show the moves (user test). Not tested: turning the devices on, how close their sound is to the player's, filter direction, recording with two tracks plus lanes.
- Clean audio (2026-11-04): rbp_stems.c now sends MixerRouteMngr::getPlayerDataPointer's block (read before MixerChannel::update) instead of +0x1c; files deck1_clean.wav, deck2_clean.wav replace *_prefader. Measured on the old prefader: it held EQ, not filter or fader. Set from a clean recording: tracks 1-2 clean with devices ON, tracks 3-4 as played, muted. Device curves are still guesses: fit eq_gain, trim_gain, cutoffs and fader from deck vs clean on a recording.
- Clean test (session 2026-11-04_165401): clean confirmed (as played silent at fader 0, clean not). As played = clean x 0.25 flat at neutral, so clean clips use SampleVolume 0.25. Fader gain ~ v^1.75 from deck 2. Filter right of centre: high band +7 dB at 0.9, bass unchanged, not a high-pass; left side unmeasured; filter devices stay off on clean tracks. EQ curves remain guesses (too few samples, knobs moved fast). Pitch fader (CC 0/32) and crossfader did reach MIDI in this session.
- Master track: rbp_stems.c hooks MasterOutChannel::update (0x59490), sends channel+0x20 as type 6 -> master.wav; the Set gets a muted Master track with one clip. Timed from deck 1's block start, assuming decks update first: check against deck1 + deck2 on a recording.
- In that session the file shows Filter LP/HP Off (user's screenshot showed them lit, so maybe a screenshot/user state).
- Track names: rbp_stems.c calls UiHid_GetTitleString/ArtistString (0x125c30, 0x125c60; deck 1-based, 16-bit text assumed from the halved size) when a new grid is sent, type 7 -> titles.csv as hex; set_to_ableton.py names clips from it. Whether these return text outside the PC link is unverified.
- Groups: clean Sets get "Clean + FX" (tracks 1-2) and "As Played" (tracks 3-4, muted at the group), GroupTrack copied from Core Library/Templates/Podcast & Radio.als with new target Ids, children routed to AudioOut/GroupTrack. Master stays ungrouped and muted so unmuting As Played does not double it. Not yet opened in Live.
- Session 2026-11-04_171122: Master = deck1 + deck2 exactly (lag 0, correlation 1.0, level 1.0) in 11 windows, Set opens with groups. UiHid title getters returned all zeros, so names did not work. Now capturing raw 568-byte getUiPlayTitleLinePointer entries to title_lines.csv; decode next, then point read_titles at it.
- Track names: title entry = u16 length at byte 32, UTF-16LE text at byte 34 (0x0326ef78 + 568 * deck), logged to title_lines.csv at each track load; clips named from it (title only: the entry has no artist). Timing against a track change is checked by the user only.
- SB3 FILTER played Dub Echo (user, 2026-11-04). SoundColorFxManager::setType (0xbc708) only stores in state 0 or 5, so CFX_FILTER_ONLY's set on knob movement could be dropped. flx4_tick now re-asks about 4x a second. state.csv color_type logs the engine's type (5 = Filter) per deck. Not yet confirmed by ear. Earlier filter measurements (highs up, bass same) were almost certainly Dub Echo.
- Correction: COLORFX_FILTER was 5, which is Space. Types are 0 Off, 1 Filter, 2 Noise, 3 Sweep, 4 Dub Echo, 5 Space, 6 Crush (operate table 0xbc0c0, constructor 0xbb970). Fixed to 1. Every recording before this used Space on the FILTER knob, including the "highs up, bass same" measurement. Awaiting confirmation by ear.
- FILTER knob confirmed a real filter by ear, 2026-11-04 (COLORFX_FILTER 1). Next: slow one-knob test, fit filter and EQ curves, turn Ableton's filter on for clean Sets.
- Filter measured on session 2026-11-04_173922 deck 2 (type 1), by Goertzel on deck vs clean: LP/HP cutoff tables in set_to_ableton.py, about 12 dB/oct with a resonant peak. Filter devices now ON for clean recordings whose log shows type 1. LOW: -26 dB floor, FreqLo 100, Slope 24. HI, MID, and every EQ above centre remain unmeasured, and the clean tracks' sound has not been compared in Live. TRIM read 0.36 in that session and the crossfader 0.83.

## SB3 in rekordbox 5 as a DDJ-SB2 (2026-11-14, awaiting test in rekordbox)
- rekordbox 5.8.5 (and 7) ship DDJ-SB2.midi.csv but no SB3 mapping. sb3_as_sb2.c creates a virtual CoreMIDI source+destination "DDJ-SB2"
  and translates with sb2_map.h (tests: test_sb2_map.c). Run "SB3 as SB2 for rekordbox.command" with the RX3 app closed. MIDI only: no licence handling.
- Same on both: transport, jog, tempo, mixer, FX knobs/buttons, mode buttons 1B/1E/20/22 (+SHIFT 69/6B/6D). Remapped: deck IN/OUT/AUTO LOOP (+SHIFT) to the SB2's
  loop notes on ch 7/8, SHIFT+VINYL -> slip 4E, SHIFT+SAMPLER 6E -> bank 6F, MASTER LEVEL B6 08 -> 09, HP level 0D -> 07, HP mix 0C -> 05; pads per mode block
  (SB3 8 notes per block, SB2 pads 5-8 at +0x40 / +8 / 1B..18); SB3 SHIFT pads with no SB2 meaning are dropped. Lights mapped back.
- Verified here: virtual DDJ-SB2 appears in CoreMIDI and the SB3 connects. Not verified: that rekordbox picks the virtual port as a DDJ-SB2, pad block-to-mode order, LEDs.
- rekordbox did not list the virtual DDJ-SB2 (translator was not running at the time; rekordbox 5 also checks IOUSB idVendor, so it may never list a virtual port).
  Second route, likely better: make_rekordbox_mapping.py rewrites rekordbox's DDJ-SB2.midi.csv into rekordbox/DDJ-SB3.midi.csv (167 rows, every SB2 input has an SB3
  control, no SB3 message used twice) for MIDI setting > DDJ-SB3 > IMPORT. "Wake SB3 lights.command" (sb3-as-sb2 --wake) sends the LED init rekordbox does not send.
- Confirmed by the user 2026-11-14: rekordbox 5 accepts the virtual "DDJ-SB2" (sb3-as-sb2) and the jog wheels work through its built-in SB2 mapping. rekordbox
  ignores jog functions in an imported user mapping, which is why the DDJ-SB3.midi.csv route alone was not enough. Clear any imported DDJ-SB3 mapping when using it
  (else every press arrives twice). sb3-as-sb2 has to be running before rekordbox starts.
- Starts at login (user's request 2026-11-14): LaunchAgent ~/Library/LaunchAgents/com.kydz.sb3-as-sb2.plist runs a signed copy at
  ~/Library/Application Support/SB3asSB2/sb3-as-sb2 (outside Documents, so launchd needs no folder permission), KeepAlive, log ~/Library/Logs/sb3-as-sb2.log.
  After rebuilding sb3_as_sb2.c, copy and re-sign that binary, then launchctl kickstart -k gui/$UID/com.kydz.sb3-as-sb2.
  Remove: launchctl bootout gui/$UID/com.kydz.sb3-as-sb2 && rm the plist. The .command launcher now just says it is already running.
- 2026-11-14 SYNC never stayed on: rekordbox turned it on then off at once, because rekordbox also had an imported user mapping for the real
  DDJ-SB3 (~/Library/Application Support/Pioneer/rekordbox/DDJ-SB3.midi.csv, our generated one), so each SB3 press arrived twice. Lights
  path is fine (rekordbox -> virtual SB2 -> SB3 checked by hand). Fix: MIDI setting > DDJ-SB3 > DEFAULT. sb3-as-sb2 always logs rekordbox's first
  40 packets and a count every 10 s; with `launchctl setenv SB2_LOG_ALL 1` + kickstart it logs every non-meter light and SB3 button/CC.
- 2026-11-16 SB3 dead after a reboot: the agent started before the SB3 was enumerated and its poll loop used usleep, so CoreMIDI never refreshed
  its endpoint list (needs a run loop). Loop now runs CFRunLoopRunInMode(0.5 s). Install a new binary with cp to a temp name + mv (cp over the
  running file got it killed with OS_REASON_CODESIGNING; launchd restarted it).
- 2026-11-19 A plugged-in DDJ-400 showed up in rekordbox as DDJ-SB2: the virtual port existed even with no SB3. sb3-as-sb2 now creates the
  virtual DDJ-SB2 only while an SB3 is present and removes it when the SB3 goes (log: "Virtual ... created/removed"). rekordbox must be
  restarted after plugging in the SB3. Checked: with only the DDJ-400 plugged in, CoreMIDI lists no DDJ-SB2. Old source: sb3_as_sb2.c.bak-always-virtual.
