#!/usr/bin/env python3
"""Build an Ableton Live Set from one RX3 set recording (stem_recorder.c).

Usage: set_to_ableton.py "<session folder in ~/Music/RX3 Sets>"
Writes "<session>/RX3 Set Project/": RX3 Set.als and Samples/Deck 1.wav, Deck 2.wav (the
Set links to their full path, so moving the folder needs Live's Search).

All audio stays at the speed it was played. Ableton's beat grid is bent to fit instead:
a tempo map puts every bar 1 of the tempo master deck on an Ableton bar, and warp markers
follow the same map, so every clip plays at rate 1. The other deck stays as played. A scratch or
a cue jump on the master costs a short tempo bump there, not any change to the sound.

Each deck track also carries the player's fader, trim, EQ and filter moves as automation on
Live's own devices. With a clean recording (deck1_clean.wav) the deck tracks play it with those
devices ON, in a "Clean + FX" group, and the as-played audio follows in a muted "As Played" group. An
older recording has only the as-played audio, so its devices stay off: the moves are in the sound.
Clips are named after the track played, when the recording has title_lines.csv. A recording with
master.wav also gets a muted Master track. Crossfader moves are not drawn yet.
"""
import array, bisect, copy, csv, glob, gzip, math, os, struct, sys
import xml.etree.ElementTree as ET
from xml.sax.saxutils import quoteattr

RATE = 44100
# The template and the devices must come from one Live schema: Live upgrades a Set by its
# version number, and a device newer than its Set crashed Live on load (2026-11-04).
TEMPLATE_GLOB = "/Applications/Ableton Live 1*.app/Contents/App-Resources/Core Library/Templates/8-Track Template.als"
FALLBACK_TEMPLATE_GLOB = "/Applications/Ableton Live 1*.app/Contents/App-Resources/Builtin/Templates/DefaultLiveSet.als"
EQ_FLOOR = 0.05            # about -26 dB
EQ_LOW_HZ = 100            # measured: LOW at 0 is -23 dB at 60 Hz, -11 at 120, -4.5 at 250
# Knob position to cutoff, from the slow test (session 2026-11-04_173922, deck 2).
LOWPASS_HZ = ((0.0, 90.0), (0.1, 200.0), (0.2, 2000.0), (0.3, 4000.0), (0.4, 8000.0), (0.5, 20000.0))
HIGHPASS_HZ = ((0.5, 20.0), (0.6, 60.0), (0.7, 1000.0), (0.8, 2500.0), (0.9, 4000.0), (1.0, 8000.0))
FILTER_RESONANCE = "0.5"   # the player's filter peaks several dB at its cutoff; not fitted exactly
FILTER_TYPE = 1            # the player's Sound Color FX type for Filter
# Where each device comes from, and the settings that make it neutral when switched on. An EQ
# Three copied from a lesson Set, which carried MIDI mappings and reversed ranges, crashed Live
# on load (2026-11-04); this preset one is clean. Auto Filter's presets are a newer device, so
# that one stays the lesson Set's copy, which loaded fine, with its modulation zeroed.
LIBRARY_GLOB = "/Applications/Ableton Live 1*.app/Contents/App-Resources/Core Library/"
DEVICE_SOURCES = {
    "FilterEQ3": ("Devices/Audio Effects/EQ Three/Hi.adv", {
        "GainLo": "1", "GainMid": "1", "GainHi": "1", "FreqLo": str(EQ_LOW_HZ), "FreqHi": "2500",
        "LowOn": "true", "MidOn": "true", "HighOn": "true", "Slope": "0"}),
    "AutoFilter": ("Lessons/Sets/Live 11 Intro Empty.als", {
        "Resonance": FILTER_RESONANCE, "Drive": "0", "Morph": "0", "LfoAmount": "0", "ModHub": "0"}),
    "StereoGain": ("Devices/Audio Effects/Utility/Wide Stereo.adv", {
        "Gain": "1", "StereoWidth": "1", "Balance": "0"}),
}
CLEAN_GAIN = 0.25          # measured: as played = clean x 0.25 (-12 dB) with every knob neutral
FADER_POWER = 1.75         # measured: fader 0.5 -10.5 dB, 0.3 -16 dB, 0.1 -37 dB
AUTOMATION_STEP = 0.004    # smallest control change worth an automation point
MARKER_EVERY = 16          # one warp marker every 16 of the deck's own beats
WARP_REPITCH = 3           # at rate 1 this plays the audio untouched
PLAY_SPEED = (0.75, 1.25)  # track speed counted as normal play (covers the pitch fader)
JUMP_MS = 250              # a position step this far from expected is a cue/hot cue jump
SHORT_STATE_ROWS = 8       # about 160 ms: shorter play/idle runs are noise
MOVE_BRIDGE_ROWS = 15      # about 300 ms
MIN_MOVE_S = 0.5           # shorter jog moves stay inside the play clip (nudges)
LOOP_PASS_S = 8.0          # play pieces this short, cut by jumps, are passes of one loop
LOOP_MIN_PASSES = 3
MASTER_HOLD_S = 4.0        # a tempo master switch must last this long to count
GRID_TOLERANCE_MS = 4.0    # the tempo map may differ from the master's beats by this much
TAIL_RMS = 0.003           # about -50 dB: audio still sounding after a deck stops


def read_state(path):
    decks = {0: [], 1: []}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if not r.get("dropped_blocks"):
                continue               # cut-off last line of a recording still running
            decks[int(r["deck"]) - 1].append((int(r["frame"]), int(r["playing_time_raw"]),
                                               r["is_playing"] == "1", int(r["bpm_raw"], 16),
                                               int(r["tempo_master"])))
    return decks


def read_grids(path):
    grids = {0: {}, 1: {}}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            grids[int(r["deck"]) - 1].setdefault(int(r["load_frame"]), []).append(
                (int(r["time"]), int(r["beat_in_bar"])))
    return grids


def title_from_line(hex_line):
    """The player's title entry: u16 length at byte 32, then that many UTF-16 characters."""
    line = bytes.fromhex(hex_line)
    if len(line) < 34:
        return ""
    length = int.from_bytes(line[32:34], "little")
    return line[34:34 + length * 2].decode("utf-16-le", "replace").strip()


def read_titles(path):
    """{deck: [(load frame, title)]}, empty for older recordings."""
    titles = {0: [], 1: []}
    if os.path.exists(path):
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                title = title_from_line(r["title_hex"])
                if title:
                    titles[int(r["deck"]) - 1].append((int(r["load_frame"]), title))
        for deck in titles:
            titles[deck].sort()
    return titles


def title_at(titles, frame):
    # The first title is logged a moment after recording starts, for the track already loaded.
    name = titles[0][1] if titles else None
    for load, text in titles:
        if load <= frame:
            name = text
    return name


def split_states(rows):
    """Cut one deck's log into (state, first_row, last_row), state = play, move or idle.
    A position jump always starts a new piece."""
    n = len(rows)
    steps = [None] * n         # speed of the step into each row, None = jump or no time passed
    jump = [False] * n
    for i in range(1, n):
        df, dp = rows[i][0] - rows[i - 1][0], rows[i][1] - rows[i - 1][1]
        if df <= 0:
            continue
        if abs(dp - df / 44.1) > JUMP_MS:
            jump[i] = True
        else:
            steps[i] = dp / (df / 44.1)
    states = ["idle"] * n
    for i in range(n):
        near = sorted(s for s in steps[max(0, i - 2):i + 3] if s is not None)
        if not near:
            continue
        speed = near[len(near) // 2]
        if PLAY_SPEED[0] <= speed <= PLAY_SPEED[1]:
            states[i] = "play"
        elif abs(speed) >= 0.05:
            states[i] = "move"
    # A scratch swings through normal speed and through zero: bridge those gaps.
    last_move = None
    for i in range(n):
        if jump[i]:
            last_move = None
        if states[i] == "move":
            if last_move is not None and i - last_move <= MOVE_BRIDGE_ROWS:
                for k in range(last_move + 1, i):
                    states[k] = "move"
            last_move = i
    pieces = []
    for i in range(n):
        if pieces and states[i] == pieces[-1][0] and not jump[i]:
            pieces[-1][2] = i
        else:
            pieces.append([states[i], i, i])
    # Short runs are noise: they join the piece before them unless a jump separates them.
    merged = []
    for piece in pieces:
        short = piece[2] - piece[1] + 1 < SHORT_STATE_ROWS
        if piece[0] == "move":
            short = (rows[piece[2]][0] - rows[piece[1]][0]) / RATE < MIN_MOVE_S
        if merged and short and not jump[piece[1]]:
            merged[-1][2] = piece[2]
        elif merged and merged[-1][0] == piece[0] and not jump[piece[1]]:
            merged[-1][2] = piece[2]
        else:
            merged.append(list(piece))
    return [tuple(p) for p in merged]


def merge_loops(rows, pieces):
    """A loop jumps back on every pass. Runs of short play pieces become one loop clip."""
    out = []
    i = 0
    while i < len(pieces):
        j = i
        while (j < len(pieces) and pieces[j][0] == "play" and
               (rows[pieces[j][2]][0] - rows[pieces[j][1]][0]) / RATE < LOOP_PASS_S):
            j += 1
        if j - i >= LOOP_MIN_PASSES:
            out.append(("loop", pieces[i][1], pieces[j - 1][2]))
            i = j
        else:
            out.append(pieces[i])
            i += 1
    return out


def steady_beats(rows, pieces, grids):
    """Beats passed in play pieces: (frame, beat_in_bar, piece index, load, beat index)."""
    loads = sorted(grids)
    out = []
    for number, (state, first, last) in enumerate(pieces):
        if state != "play":
            continue
        for k in range(first, last):
            a, b = rows[k], rows[k + 1]
            df, dp = b[0] - a[0], b[1] - a[1]
            if df <= 0 or dp <= 0:
                continue
            i = bisect.bisect_right(loads, b[0]) - 1
            if i < 0:
                continue
            grid = grids[loads[i]]
            j = bisect.bisect_left(grid, (a[1], 0))
            while j < len(grid) and grid[j][0] < b[1]:
                out.append((a[0] + (grid[j][0] - a[1]) / dp * df, grid[j][1], number, loads[i], j))
                j += 1
    return out


def master_periods(decks, states_of):
    """[(first frame, deck)]: the tempo master, ignoring switches shorter than MASTER_HOLD_S
    and switches to a deck that is not in normal play."""
    rows = decks[0]
    def playing(deck, frame):
        data = decks[deck]
        i = bisect.bisect_right([r[0] for r in data], frame) - 1
        if i < 0:
            return False
        return any(s == "play" and a <= i <= b for s, a, b in states_of[deck])
    current, periods, candidate, since = None, [], None, 0
    for row in rows:
        value = row[4]
        if value not in (0, 1):
            continue
        if current is None:
            if playing(value, row[0]):
                current = value
                periods.append((row[0], value))
            continue
        if value == current:
            candidate = None
            continue
        if value != candidate:
            candidate, since = value, row[0]
        if (row[0] - since) / RATE >= MASTER_HOLD_S and playing(value, row[0]):
            current, candidate = value, None
            periods.append((since, value))
    return periods


def build_tempo_map(beats_by_deck, periods, fallback_bpm):
    """Breakpoints [(frame, beat)]: the master's bars 1 land on Ableton bars. Piecewise
    linear, simplified to within GRID_TOLERANCE_MS, so a steady deck gives one tempo."""
    anchors = []
    for i, (start, deck) in enumerate(periods):
        end = periods[i + 1][0] if i + 1 < len(periods) else float("inf")
        anchors += [b + (deck,) for b in beats_by_deck[deck] if start <= b[0] < end and b[1] == 1]
    if not anchors:
        return [(0.0, 0.0), (RATE * 60.0 / fallback_bpm, 1.0)]
    points = []                # (frame, beat)
    tempo = fallback_bpm
    previous = None
    for anchor in anchors:
        frame = anchor[0]
        if previous is None:
            points.append((frame, 0.0))
            previous = anchor
            continue
        seconds = (frame - previous[0]) / RATE
        same_run = anchor[5] == previous[5] and anchor[2] == previous[2] and anchor[3] == previous[3]
        if same_run:
            beats = float(anchor[4] - previous[4])
            if beats <= 0:
                continue
        else:
            if seconds < 2 * 60.0 / tempo:
                continue       # too close to fit in the grid: the next bar 1 anchors instead
            beats = 4.0 * max(1, round(seconds * tempo / 240.0))
        points.append((frame, points[-1][1] + beats))
        tempo = beats / seconds * 60.0
        previous = anchor
    if len(points) == 1:
        points.append((points[0][0] + RATE * 60.0 / fallback_bpm, 1.0))
    # Keep only the corners. Walk greedily: extend a straight piece while every point stays close.
    kept = [points[0]]
    start = 0
    i = 1
    while i < len(points):
        j = i + 1
        while j < len(points):
            f0, b0 = points[start]
            f1, b1 = points[j]
            slope = (b1 - b0) / (f1 - f0)
            ok = True
            for k in range(start + 1, j):
                off = abs(b0 + (points[k][0] - f0) * slope - points[k][1]) / slope / 44.1
                if off > GRID_TOLERANCE_MS:
                    ok = False
                    break
            if not ok:
                break
            j += 1
        kept.append(points[j - 1])
        start = j - 1
        i = j
    return kept


class TempoMap:
    def __init__(self, points):
        self.frames = [p[0] for p in points]
        self.beats = [p[1] for p in points]
        self.offset = 0.0

    def beat(self, frame):
        i = bisect.bisect_right(self.frames, frame) - 1
        i = max(0, min(i, len(self.frames) - 2))
        slope = (self.beats[i + 1] - self.beats[i]) / (self.frames[i + 1] - self.frames[i])
        return self.beats[i] + (frame - self.frames[i]) * slope + self.offset

    def start_on_a_bar(self, first_frame):
        """Shift by whole bars so nothing lands before the Set's first beat."""
        while self.beat(first_frame) < 0:
            self.offset += 4.0

    def tempos(self):
        """[(beat, bpm)]"""
        out = []
        for i in range(len(self.frames) - 1):
            seconds = (self.frames[i + 1] - self.frames[i]) / RATE
            out.append((self.beats[i] + self.offset, (self.beats[i + 1] - self.beats[i]) / seconds * 60.0))
        return out


def sounding_until(path, frame, limit):
    """Where a stopped deck goes quiet, so an effect tail stays in the clip."""
    block = RATE // 10
    with open(path, "rb") as f:
        while frame + block < limit:
            f.seek(44 + frame * 8)
            v = array.array("f", f.read(block * 8))
            if not v or (sum(x * x for x in v[::8]) / len(v[::8])) ** 0.5 < TAIL_RMS:
                break
            frame += block
    return min(frame, limit)


def clip_xml(clip_id, name, color, path, frames, start, end, markers, gain=1.0):
    mtime = int(os.path.getmtime(path))
    size = os.path.getsize(path)
    marks = "".join('<WarpMarker Id="%d" SecTime="%r" BeatTime="%r" />' % (i, s, b)
                    for i, (s, b) in enumerate(markers))
    return ET.fromstring(f'''<AudioClip Id="{clip_id}" Time="{start!r}">
<LomId Value="0" /><LomIdView Value="0" />
<CurrentStart Value="{start!r}" /><CurrentEnd Value="{end!r}" />
<Loop><LoopStart Value="{start!r}" /><LoopEnd Value="{end!r}" /><StartRelative Value="0" /><LoopOn Value="false" />
<OutMarker Value="{end!r}" /><HiddenLoopStart Value="{start!r}" /><HiddenLoopEnd Value="{end!r}" /></Loop>
<Name Value={quoteattr(name)} /><Annotation Value="" /><Color Value="{color}" /><LaunchMode Value="0" /><LaunchQuantisation Value="0" />
<TimeSignature><TimeSignatures><RemoteableTimeSignature Id="0"><Numerator Value="4" /><Denominator Value="4" /><Time Value="0" /></RemoteableTimeSignature></TimeSignatures></TimeSignature>
<Envelopes><Envelopes /></Envelopes>
<ScrollerTimePreserver><LeftTime Value="0" /><RightTime Value="0" /></ScrollerTimePreserver>
<TimeSelection><AnchorTime Value="0" /><OtherTime Value="0" /></TimeSelection>
<Legato Value="false" /><Ram Value="false" /><GrooveSettings><GrooveId Value="-1" /></GrooveSettings>
<Disabled Value="false" /><VelocityAmount Value="0" />
<FollowAction><FollowTime Value="4" /><IsLinked Value="true" /><LoopIterations Value="1" /><FollowActionA Value="4" /><FollowActionB Value="0" /><FollowChanceA Value="100" /><FollowChanceB Value="0" /><JumpIndexA Value="0" /><JumpIndexB Value="0" /><FollowActionEnabled Value="false" /></FollowAction>
<Grid><FixedNumerator Value="1" /><FixedDenominator Value="16" /><GridIntervalPixel Value="20" /><Ntoles Value="2" /><SnapToGrid Value="true" /><Fixed Value="false" /></Grid>
<FreezeStart Value="0" /><FreezeEnd Value="0" /><IsWarped Value="true" /><TakeId Value="0" /><IsInKey Value="false" />
<ScaleInformation><Root Value="0" /><Name Value="0" /></ScaleInformation>
<AutomationEnvelopesListWrapper LomId="0" />
<SampleRef><FileRef><RelativePathType Value="0" /><RelativePath Value="" /><Path Value="{path}" /><Type Value="2" /><LivePackName Value="" /><LivePackId Value="" /><OriginalFileSize Value="{size}" /><OriginalCrc Value="0" /><SourceHint Value="" /></FileRef>
<LastModDate Value="{mtime}" /><SourceContext /><SampleUsageHint Value="0" /><DefaultDuration Value="{frames}" /><DefaultSampleRate Value="{RATE}" /><SamplesToAutoWarp Value="0" /></SampleRef>
<Onsets><UserOnsets /><HasUserOnsets Value="false" /></Onsets>
<WarpMode Value="{WARP_REPITCH}" /><GranularityTones Value="30" /><GranularityTexture Value="65" /><FluctuationTexture Value="25" /><TransientResolution Value="6" /><TransientLoopMode Value="2" /><TransientEnvelope Value="100" /><ComplexProFormants Value="100" /><ComplexProEnvelope Value="128" />
<Sync Value="true" /><HiQ Value="true" /><Fade Value="true" />
<Fades><FadeInLength Value="0" /><FadeOutLength Value="0" /><ClipFadesAreInitialized Value="true" /><CrossfadeInState Value="0" /><FadeInCurveSkew Value="0" /><FadeInCurveSlope Value="0" /><FadeOutCurveSkew Value="0" /><FadeOutCurveSlope Value="0" /><IsDefaultFadeIn Value="false" /><IsDefaultFadeOut Value="false" /></Fades>
<PitchCoarse Value="0" /><PitchFine Value="0" /><SampleVolume Value="{gain!r}" />
<WarpMarkers>{marks}</WarpMarkers><SavedWarpMarkersForStretched /><MarkersGenerated Value="true" /><IsSongTempoLeader Value="false" />
</AudioClip>''')


def read_controls(path):
    """{deck: [(frame, {control: value})]} from the mixer columns, [] for older recordings."""
    controls = {0: [], 1: []}
    names = ("fader", "trim", "eq0", "eq1", "eq2", "color")
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r.get("crossfader"):
                controls[int(r["deck"]) - 1].append((int(r["frame"]), {n: float(r[n]) for n in names}))
    return controls


def read_filter_is_real(path):
    """{deck: was the knob's effect the Filter?} Older recordings do not log the type."""
    counts = {0: [0, 0], 1: [0, 0]}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r.get("color_type"):
                counts[int(r["deck"]) - 1][0] += r["color_type"] == str(FILTER_TYPE)
                counts[int(r["deck"]) - 1][1] += 1
    return {d: total > 0 and hits >= total * 0.9 for d, (hits, total) in counts.items()}


def eq_gain(v):
    """Centre (0.5) is 0 dB. Below it, measured on LOW: knob 0 is about -26 dB, not a kill, and
    0.2 about -8 dB. Above centre is assumed (+6 dB at the top): not measured yet."""
    return max(EQ_FLOOR, v * 2.0) if v <= 0.5 else min(1.99526238, 1.0 + (v - 0.5) * 2.0 * 0.99526238)


def trim_gain(v):
    """Centre (0.5) is 0 dB; up to +9 dB, as on the unit's TRIM."""
    return v * 2.0 if v <= 0.5 else 1.0 + (v - 0.5) * 2.0 * 1.818


def _cutoff(table, v):
    """Measured (knob, Hz) points, joined in octaves; Auto Filter's Cutoff is a MIDI note number."""
    for (v0, hz0), (v1, hz1) in zip(table, table[1:]):
        if v <= v1:
            t = (v - v0) / (v1 - v0)
            hz = hz0 * (hz1 / hz0) ** max(0.0, t)
            return max(20.0, min(135.0, 69.0 + 12.0 * math.log2(hz / 440.0)))
    return 135.0


def lowpass_cutoff(v):
    """Filter knob below centre, measured from where the response peaks."""
    return 135.0 if v >= 0.5 else _cutoff(LOWPASS_HZ, v)


def highpass_cutoff(v):
    """Filter knob above centre, measured likewise."""
    return 20.0 if v <= 0.5 else _cutoff(HIGHPASS_HZ, v)


class Devices:
    """Live's own devices, copied from a Set in its library. Every target needs a new Id."""
    def __init__(self, root, live_set):
        self.next_id = int(live_set.find("NextPointeeId").get("Value"))
        self.live_set = live_set
        self.sources = {}
        for kind, (relative, _) in DEVICE_SOURCES.items():
            found = sorted(glob.glob(LIBRARY_GLOB + relative))
            source = ET.fromstring(gzip.open(found[-1]).read()) if found else None
            if source is None or source.find(".//" + kind) is None or any(
                    source.get(k) != root.get(k) for k in ("MinorVersion", "SchemaChangeCount")):
                print(f"{kind} is missing or a different version in Live's library: leaving the automation out")
                self.sources = None
                break
            self.sources[kind] = source

    def add(self, track, kind, name, slot):
        device = copy.deepcopy(self.sources[kind].find(".//" + kind))
        for parameter, value in DEVICE_SOURCES[kind][1].items():
            setting = device.find(parameter + "/Manual")
            if setting is not None:
                setting.set("Value", value)
        for element in device.iter():
            if "Id" in element.attrib and (element.tag.endswith("Target") or element.tag == "Pointee"):
                element.set("Id", str(self.next_id))
                self.next_id += 1
        device.set("Id", str(slot))
        device.find("On/Manual").set("Value", "true" if self.active else "false")
        device.find("UserName").set("Value", name)
        for child in list(device.find("LastPresetRef")):
            device.find("LastPresetRef").remove(child)
        ET.SubElement(device.find("LastPresetRef"), "Value")
        track.find("DeviceChain/DeviceChain/Devices").append(device)
        return device

    def done(self):
        self.live_set.find("NextPointeeId").set("Value", str(self.next_id))


def add_group(root, devices, tracks, name, children, color, muted):
    """A Live group track above its children, copied from a template of Live's. False if none."""
    found = sorted(glob.glob(LIBRARY_GLOB + "Templates/Podcast & Radio.als"))
    source = ET.fromstring(gzip.open(found[-1]).read()) if found else None
    if source is None or any(source.get(k) != root.get(k) for k in ("MinorVersion", "SchemaChangeCount")):
        print("no matching group track in Live's library: leaving the groups out")
        return False
    group = copy.deepcopy(source.find("LiveSet/Tracks/GroupTrack"))
    for element in group.iter():
        if "Id" in element.attrib and (element.tag.endswith("Target") or element.tag == "Pointee"):
            element.set("Id", str(devices.next_id))
            devices.next_id += 1
    chain = group.find("DeviceChain/DeviceChain/Devices")
    for device in list(chain):
        chain.remove(device)
    group_id = max(int(t.get("Id")) for t in tracks) + 1
    group.set("Id", str(group_id))
    for tag in ("EffectiveName", "UserName"):
        group.find("Name/" + tag).set("Value", name)
    group.find("Color").set("Value", str(color))
    group.find("TrackUnfolded").set("Value", "true")
    group.find("DeviceChain/Mixer/Speaker/Manual").set("Value", "false" if muted else "true")
    # A group holds one clip slot per scene, like every other track of this Set.
    slots = group.find("DeviceChain/FreezeSequencer/ClipSlotList")
    wanted = len(root.find("LiveSet/Scenes"))
    if slots is not None and len(slots):
        while len(slots) > wanted:
            slots.remove(slots[-1])
        while len(slots) < wanted:
            extra = copy.deepcopy(slots[-1])
            extra.set("Id", str(len(slots)))
            slots.append(extra)
    slots = group.find("Slots")
    if slots is not None and len(slots):
        while len(slots) > wanted:
            slots.remove(slots[-1])
        while len(slots) < wanted:
            extra = copy.deepcopy(slots[-1])
            extra.set("Id", str(len(slots)))
            slots.append(extra)
    for child in children:
        child.find("TrackGroupId").set("Value", str(group_id))
        routing = child.find("DeviceChain/AudioOutputRouting")
        routing.find("Target").set("Value", "AudioOut/GroupTrack")
        routing.find("UpperDisplayString").set("Value", "Group")
    tracks.insert(list(tracks).index(children[0]), group)
    return True


def add_lane(track, device, parameter, points, to_value, tempo_map, first, last):
    """points: [(frame, knob value)]. Holds a value until it moves, so there are no long ramps."""
    if not points:
        return 0
    param = device.find(parameter)
    start = to_value(points[0][1])
    param.find("Manual").set("Value", repr(start))
    envelopes = track.find("AutomationEnvelopes/Envelopes")
    envelope = ET.SubElement(envelopes, "AutomationEnvelope")
    envelope.set("Id", str(len(envelopes) - 1))
    ET.SubElement(ET.SubElement(envelope, "EnvelopeTarget"), "PointeeId").set(
        "Value", param.find("AutomationTarget").get("Id"))
    automation = ET.SubElement(envelope, "Automation")
    events = ET.SubElement(automation, "Events")
    view = ET.SubElement(automation, "AutomationTransformViewState")
    ET.SubElement(view, "IsTransformPending").set("Value", "false")
    ET.SubElement(view, "TimeAndValueTransforms")
    def event(at, value):
        e = ET.SubElement(events, "FloatEvent")
        e.set("Id", str(len(events) - 1))
        e.set("Time", repr(at))
        e.set("Value", repr(value))
    event(-63072000.0, start)
    held = points[0][1]
    previous = points[0]
    count = 0
    for frame, value in points[1:]:
        if first <= frame <= last and abs(value - held) >= AUTOMATION_STEP:
            before = tempo_map.beat(previous[0])
            if abs(previous[1] - held) < AUTOMATION_STEP and previous[0] >= first:
                event(before, to_value(previous[1]))
            event(tempo_map.beat(frame), to_value(value))
            held = value
            count += 1
        previous = (frame, value)
    return count


def add_automation(tracks, devices, controls, tempo_map, first, last, active, real_filter):
    """Deck moves on Live's devices, on only when the audio is clean. Returns the points drawn."""
    total = 0
    for deck, track in enumerate(tracks):
        rows = controls[deck]
        if not rows or devices.sources is None:
            continue
        name = f"Deck {deck + 1}"
        devices.active = active
        series = lambda key: [(f, c[key]) for f, c in rows]
        eq = devices.add(track, "FilterEQ3", name + " EQ", 0)
        for parameter, key in (("GainHi", "eq0"), ("GainMid", "eq1"), ("GainLo", "eq2")):
            total += add_lane(track, eq, parameter, series(key), eq_gain, tempo_map, first, last)
        # Only where the knob really was a filter: recordings before the type fix played Space.
        devices.active = active and real_filter[deck]
        low = devices.add(track, "AutoFilter", name + " Filter LP", 1)
        low.find("FilterType/Manual").set("Value", "0")
        total += add_lane(track, low, "Cutoff", series("color"), lowpass_cutoff, tempo_map, first, last)
        high = devices.add(track, "AutoFilter", name + " Filter HP", 2)
        high.find("FilterType/Manual").set("Value", "1")
        total += add_lane(track, high, "Cutoff", series("color"), highpass_cutoff, tempo_map, first, last)
        devices.active = active
        fader = devices.add(track, "StereoGain", name + " Fader", 3)
        total += add_lane(track, fader, "Gain", series("fader"), lambda v: v ** FADER_POWER, tempo_map, first, last)
        trim = devices.add(track, "StereoGain", name + " Trim", 4)
        total += add_lane(track, trim, "Gain", series("trim"), trim_gain, tempo_map, first, last)
    return total


def write_trimmed(source, dest, first, last):
    frames = last - first
    if os.path.exists(dest) and os.path.getsize(dest) == 44 + frames * 8:
        return                 # Live may have this copy open
    with open(source, "rb") as f, open(dest, "wb") as w:
        bytes_ = frames * 8
        w.write(b"RIFF" + struct.pack("<I", 36 + bytes_) + b"WAVEfmt " +
                struct.pack("<IHHIIHH", 16, 3, 2, RATE, RATE * 8, 8, 32) + b"data" + struct.pack("<I", bytes_))
        f.seek(44 + first * 8)
        left = bytes_
        while left:
            chunk = f.read(min(left, 1 << 20))
            if not chunk:
                raise SystemExit("recording shorter than its log: " + source)
            w.write(chunk)
            left -= len(chunk)


def set_tempo(live_set, tempos):
    """tempos: [(beat, bpm)]. A step in Live's automation is two events at almost one time."""
    main = live_set.find("MainTrack")
    tempo = next(main.iter("Tempo"))
    tempo.find("Manual").set("Value", repr(tempos[0][1]))
    target = tempo.find("AutomationTarget").get("Id")
    for envelope in main.iter("AutomationEnvelope"):
        if envelope.find("EnvelopeTarget/PointeeId").get("Value") != target:
            continue
        events = envelope.find("Automation/Events")
        first = events.find("FloatEvent")
        first.set("Value", repr(tempos[0][1]))
        next_id = int(first.get("Id")) + 1
        for i in range(1, len(tempos)):
            beat, bpm = tempos[i]
            for at, value in ((beat - 1e-6, tempos[i - 1][1]), (beat, bpm)):
                event = ET.SubElement(events, "FloatEvent")
                event.set("Id", str(next_id))
                event.set("Time", repr(at))
                event.set("Value", repr(value))
                next_id += 1


def build(session):
    decks = read_state(os.path.join(session, "state.csv"))
    grids = read_grids(os.path.join(session, "grid.csv"))
    played = [r for rows in decks.values() for r in rows if r[2]]
    if not played:
        raise SystemExit("no deck played in this recording")
    length_frames = [(os.path.getsize(os.path.join(session, f"deck{d + 1}.wav")) - 44) // 8 for d in (0, 1)]
    first = min(r[0] for r in played)
    last = min(max(r[0] for r in played), *length_frames)

    titles = read_titles(os.path.join(session, "title_lines.csv"))
    states = {d: merge_loops(decks[d], split_states(decks[d])) for d in (0, 1)}
    beats = {d: steady_beats(decks[d], states[d], grids[d]) for d in (0, 1)}
    periods = master_periods(decks, states)
    if not periods:
        deck = next(d for d in (0, 1) if any(s == "play" for s, _, _ in states[d]))
        periods = [(first, deck)]
    bpm_raw = next((r[3] for r in decks[periods[0][1]] if r[2] and r[3] != 0xffffffff), 12000)
    tempo_map = TempoMap(build_tempo_map(beats, periods, bpm_raw / 100.0))
    tempo_map.start_on_a_bar(first)

    project = os.path.join(session, "RX3 Set Project")
    os.makedirs(os.path.join(project, "Samples"), exist_ok=True)
    templates = sorted(glob.glob(TEMPLATE_GLOB)) or sorted(glob.glob(FALLBACK_TEMPLATE_GLOB))
    if not templates:
        raise SystemExit("Ableton Live not found in /Applications: the recording is kept, run this again later")
    with gzip.open(templates[-1]) as f:
        root = ET.fromstring(f.read())
    live_set = root.find("LiveSet")
    tracks = live_set.find("Tracks")
    clean = all(os.path.exists(os.path.join(session, f"deck{d + 1}_clean.wav")) for d in (0, 1))
    master = os.path.join(session, "master.wav")
    has_master = os.path.exists(master)
    deck_slots = 4 if clean else 2
    for track in tracks.findall("MidiTrack") + tracks.findall("AudioTrack")[deck_slots + (1 if has_master else 0):]:
        tracks.remove(track)
    for track in tracks.findall("AudioTrack"):
        devices_now = track.find("DeviceChain/DeviceChain/Devices")
        for device in list(devices_now):
            devices_now.remove(device)
    tempos = tempo_map.tempos()
    set_tempo(live_set, tempos)

    all_tracks = tracks.findall("AudioTrack")
    deck_tracks = all_tracks[:2]
    # (track, deck, track name, file in the session, file in the project, muted)
    layout = [(all_tracks[d], d, f"Deck {d + 1}", f"deck{d + 1}{'_clean' if clean else ''}.wav",
               f"Deck {d + 1}{' Clean' if clean else ''}.wav", False) for d in (0, 1)]
    if clean:
        layout += [(all_tracks[d + 2], d, f"Deck {d + 1} As Played", f"deck{d + 1}.wav",
                    f"Deck {d + 1}.wav", True) for d in (0, 1)]
    clip_counts = []
    next_clip = 0
    for track, deck, name, source, sample, muted in layout:
        for tag in ("EffectiveName", "UserName"):
            track.find("Name/" + tag).set("Value", name)
        track.find("DeviceChain/Mixer/Speaker/Manual").set("Value", "false" if muted else "true")
        path = os.path.join(project, "Samples", sample)
        write_trimmed(os.path.join(session, source), path, first, last)
        events = track.find("DeviceChain/MainSequencer/Sample/ArrangerAutomation/Events")
        rows = decks[deck]
        count = {"play": 0, "move": 0, "loop": 0}
        pieces = states[deck]
        for number, (state, a, b) in enumerate(pieces):
            if state == "idle":
                continue
            f0 = max(first, rows[a][0])
            f1 = min(last, rows[b][0])
            following = next((rows[p[1]][0] for p in pieces[number + 1:] if p[0] != "idle"), last)
            if number + 1 < len(pieces) and pieces[number + 1][0] == "idle":
                f1 = min(following, sounding_until(os.path.join(session, f"deck{deck + 1}.wav"), f1, min(following, last)))
            if f1 - f0 < RATE // 20:
                continue
            count[state] += 1
            # Rate 1: a marker at each corner of the tempo map, plus this deck's own beats.
            points = {f0, f1}
            points.update(f for f in tempo_map.frames if f0 < f < f1)
            if state == "play":
                own = [x[0] for x in beats[deck] if x[2] == number and f0 < x[0] < f1]
                points.update(own[::MARKER_EVERY])
            markers = [((f - first) / RATE, tempo_map.beat(f)) for f in sorted(points)]
            label = (title_at(titles[deck], f0) or name) + {"move": " Scratch", "loop": " Loop"}.get(state, "")
            events.append(clip_xml(next_clip, label, 14 if state == "move" else 5 if state == "loop" else (16 if deck == 0 else 26), path,
                                   last - first, tempo_map.beat(f0), tempo_map.beat(f1), markers,
                                   CLEAN_GAIN if clean and not muted else 1.0))
            next_clip += 1
        if not muted:
            clip_counts.append(count)

    if has_master:
        track = all_tracks[deck_slots]
        for tag in ("EffectiveName", "UserName"):
            track.find("Name/" + tag).set("Value", "Master")
        track.find("DeviceChain/Mixer/Speaker/Manual").set("Value", "false")
        path = os.path.join(project, "Samples", "Master.wav")
        write_trimmed(master, path, first, last)
        points = {first, last}
        points.update(f for f in tempo_map.frames if first < f < last)
        markers = [((f - first) / RATE, tempo_map.beat(f)) for f in sorted(points)]
        track.find("DeviceChain/MainSequencer/Sample/ArrangerAutomation/Events").append(
            clip_xml(next_clip, "Master", 0, path, last - first, tempo_map.beat(first), tempo_map.beat(last), markers))
        next_clip += 1

    devices = Devices(root, live_set)
    lane_points = add_automation(deck_tracks, devices, read_controls(os.path.join(session, "state.csv")),
                                 tempo_map, first, last, clean,
                                 read_filter_is_real(os.path.join(session, "state.csv")))
    if clean:
        if add_group(root, devices, tracks, "Clean + FX", all_tracks[:2], 26, False):
            # Muted as a group, so a click brings both back; the Master stays out of it.
            if add_group(root, devices, tracks, "As Played", all_tracks[2:4], 14, True):
                for track in all_tracks[2:4]:
                    track.find("DeviceChain/Mixer/Speaker/Manual").set("Value", "true")
    devices.done()

    data = b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root)
    als = os.path.join(project, "RX3 Set.als")
    with gzip.open(als, "wb") as f:
        f.write(data)

    # How close the master's beats are to Ableton's grid.
    errors = []
    for i, (start, deck) in enumerate(periods):
        end = periods[i + 1][0] if i + 1 < len(periods) else float("inf")
        for frame, *_ in beats[deck]:
            if start <= frame < end and first <= frame <= last:
                value = tempo_map.beat(frame)
                i_tempo = max(0, bisect.bisect_right(tempo_map.frames, frame) - 1)
                bpm = tempos[min(i_tempo, len(tempos) - 1)][1]
                errors.append(abs(value - round(value)) * 60000.0 / bpm)
    errors.sort()
    summary = {
        "als": als, "seconds": (last - first) / RATE, "tempos": tempos, "periods": periods,
        "clips": clip_counts, "errors": errors, "lane_points": lane_points,
    }
    return summary


def main():
    result = build(os.path.abspath(sys.argv[1]))
    print("Set:", result["als"])
    print(f"{result['seconds']:.1f} s; tempo changes {len(result['tempos']) - 1}; master periods "
          f"{[(round(f / RATE, 1), d + 1) for f, d in result['periods']]}")
    bpms = [t[1] for t in result["tempos"]]
    print(f"tempo range {min(bpms):.2f} to {max(bpms):.2f} BPM")
    print("clips:", result["clips"], "automation points:", result["lane_points"])
    e = result["errors"]
    if e:
        print(f"master beats vs Ableton grid: median {e[len(e) // 2]:.1f} ms, worst {e[-1]:.1f} ms, "
              f"over 20 ms: {sum(x > 20 for x in e)} of {len(e)}")


if __name__ == "__main__":
    main()
