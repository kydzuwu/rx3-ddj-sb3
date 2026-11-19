# RX3 Prototype for DDJ-SB3

Runs the Pioneer DJ XDJ-RX3 player software on an Apple silicon Mac (QEMU + Hypervisor.framework)
and lets a **Pioneer DDJ-SB3** control it. The player expects a DDJ-FLX4; `midi-host` translates
the SB3's MIDI to FLX4 MIDI and back (LEDs, meters).

Unofficial project, not affiliated with Pioneer DJ or AlphaTheta. **No Pioneer/AlphaTheta files are
included.** On first launch the app downloads the official XDJ-RX3 update and builds the player on
your Mac inside Docker.

## Requirements
- Mac with Apple silicon (M1 or later), macOS 26 or later
- [Docker Desktop](https://www.docker.com/products/docker-desktop/) (only for the first-run build, ~20-40 min)
- DDJ-SB3 connected by USB, headphones in the SB3

## Install
1. Download `RX3-Prototype-SB3.zip` from [Releases](../../releases) and unzip it.
2. Move `RX3 Prototype.app` to Applications. The app is not notarized: the first time,
   right-click it -> Open (or run `xattr -dr com.apple.quarantine "/Applications/RX3 Prototype.app"`).
3. Start Docker Desktop, open the app and let the first-run setup finish.

Audio: master plays on the Mac's output (system default, or built-in speakers if the default is the
SB3); the headphone cue mix goes to the SB3's headphone jack.

## SB3 controls (differences from the printed labels)
| SB3 | Player |
|---|---|
| IN / OUT | LOOP IN / OUT; on a stopped deck: previous / next memory cue |
| SHIFT + IN / OUT | CUE/LOOP CALL < / > |
| AUTO LOOP | 4 BEAT loop / exit (LED blinks while a loop is set) |
| SHIFT + AUTO LOOP | PLAYLIST |
| KEY LOCK / SHIFT + KEY LOCK | Master tempo / tempo range |
| FX1 | Beat FX on this deck (press again = off; LED blinks while on) |
| SHIFT + FX1 | Beat FX on master |
| FX2 / FX3 | Beat < / > |
| SHIFT + FX2 / FX3 | Previous / next Beat FX type |
| FILTER knobs | Sound Color FX Filter |
| Browse push | Deck screen: SOURCE (USB) menu; elsewhere: select |
| SHIFT + browse push | Back; at the top level back to the deck screen |
| SHIFT + LOAD L / R | Track filter / shortcut |

## Record a DJ set into Ableton Live
Tap the record dot (left of INFO on the deck screen) to start, tap again to stop. Each recording goes to
`~/Music/RX3 Sets/<date>/`, and on stop an Ableton Live 12 Set is built next to it (`RX3 Set Project/`):
- tracks on the player's beat grid, tempo following the master deck; scratches and loops as their own clips
- "Clean + FX" group: each deck without mixer processing, with EQ, filter, fader and trim as Live devices and automation
- "As Played" group (muted): each deck exactly as heard; a muted Master track
- clips named after the loaded track

Needs Ableton Live 12 in `/Applications` and Apple's `/usr/bin/python3`. Generator: `host/set_to_ableton.py`.

## DDJ-SB3 in rekordbox 5
rekordbox 5 has no SB3 mapping. `host/sb3_as_sb2.c` shows rekordbox a virtual **DDJ-SB2** (only while an SB3
is plugged in) and translates both ways, so rekordbox's own SB2 mapping drives the SB3, jog wheels and lights included.
Build and run with `host/SB3 as SB2 for rekordbox.command` (quit the RX3 app first; start it before rekordbox).
Keep the DDJ-SB3 and DDJ-SB2 entries in rekordbox's MIDI setting on DEFAULT. MIDI translation only.

## Building from source
- `host/`: Mac-side code. `midi_host.c` + `sb3_map.h` (MIDI translation), `coreaudio_adapter.c`
  (audio routing, injected into QEMU), `AppLauncher.swift` (the app's launcher), tests.
  `stem_recorder.c` + `set_to_ableton.py` (set recording -> Ableton), `sb3_as_sb2.c` + `sb2_map.h` (rekordbox),
  `guest-build/build_guest.sh` (rebuilds the player-side shims in Docker).
  `Rebuild and Launch SB3.command` builds midi-host, the recorder and the adapter into an existing
  `RX3 Prototype.app` placed next to it (take it from the release), signs and starts it.
- `build-source/`: the guest (player-side) project, shims in `build-source/scripts/shims`
  (`map_flx4.c` is the FLX4 map). The app copies this into the first-run Docker build.
- `app-resources/`: first-run setup script, shim environment, Info.plist, third-party licenses.
- `host/CLAUDE.md`: detailed developer notes.

Launcher: `xcrun swiftc -O -target arm64-apple-macos26.0 -framework AppKit -framework CoreAudio AppLauncher.swift -o "RX3 Prototype.app/Contents/MacOS/RX3Prototype"`

## Known issues
- SB3 headphone output and crossfader are still being tested.
- A DDJ-FLX4 is the player's native controller and should work directly, but has not been retested.

## License
Original code MIT (see `build-source/LICENSE`). QEMU and bundled libraries keep their own licenses
(`app-resources/licenses`). DDJ-SB3 MIDI reference: the "DDJ-SB3 MIDI Message List" from AlphaTheta/Pioneer DJ.
