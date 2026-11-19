#!/usr/bin/env python3
"""Synthetic set: pitch change, cue jump, a scratch, a short and a real master switch."""
import csv, gzip, math, os, shutil, struct, tempfile
import xml.etree.ElementTree as ET
import set_to_ableton as s

RATE = 44100
STEP = 882


def grid_rows(writer, deck, load, bpm, first_ms, beats):
    for i in range(beats):
        writer.writerow([load, deck, i, i % 4 + 1, int(bpm * 100), round(first_ms + i * 60000 / bpm)])


d = tempfile.mkdtemp()
seconds = 120
with open(d + "/grid.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow("load_frame,deck,index,beat_in_bar,bpm_x100,time".split(","))
    grid_rows(w, 1, 0, 120, 100, 400)
    grid_rows(w, 2, 0, 100, 300, 400)

with open(d + "/state.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow("frame,deck,playing_time_raw,bpm_raw,grid_offset_raw,is_playing,tempo_x100_raw,total_time_raw,tempo_master,dropped_blocks".split(","))
    p1 = p2 = 0.0
    for frame in range(0, seconds * RATE, STEP):
        t = frame / RATE
        dt = STEP / RATE * 1000
        # Deck 1: 120 BPM, +4 % from 40 s, cue jump at 60 s, scratches 80-82 s.
        if 80 <= t < 82:
            p1 += dt * 3 * math.sin((t - 80) * 12)
        else:
            p1 += dt * (1.04 if t >= 40 else 1.0)
        if abs(t - 60) < dt / 2000:
            p1 += 8000
        # Deck 2: stopped until 20 s, then 100 BPM.
        if t >= 20:
            p2 += dt
        master = 1 if 70 <= t or 30 <= t < 31 else 0   # 1 s flick at 30 s, real switch at 70 s
        w.writerow([frame, 1, int(p1), hex(12000), 0, 1, 0, 300000, master, 0])
        w.writerow([frame, 2, int(p2), hex(10000), 0, int(t >= 20), 0, 300000, master, 0])

for name in ("deck1", "deck2"):
    with open(f"{d}/{name}.wav", "wb") as f:
        n = seconds * RATE * 8
        f.write(b"RIFF" + struct.pack("<I", 36 + n) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 2, RATE, RATE * 8, 8, 32) + b"data" + struct.pack("<I", n))
        f.truncate(44 + n)

# A one-beat loop: the position jumps back every 500 ms, ten times, then plays on.
rows = []
pos = 0.0
for frame in range(0, 60 * RATE, STEP):
    t = frame / RATE
    pos += STEP / 44.1
    if 20 <= t < 25 and int(t * 2) != int((t - STEP / RATE) * 2):
        pos -= 500
    rows.append((frame, int(pos), True, 12000, 0))
pieces = s.merge_loops(rows, s.split_states(rows))
assert [p[0] for p in pieces] == ["play", "loop", "play"], pieces

# Older recordings have no mixer columns; give this one a fader move and a filter move.
rows = list(csv.reader(open(d + "/state.csv", newline="")))
rows[0] += "fader,trim,eq0,eq1,eq2,color,crossfader,color_type".split(",")
for row in rows[1:]:
    t = int(row[0]) / RATE
    fader = 1.0 if t < 50 else max(0.0, 1.0 - (t - 50) / 5)
    color = 0.5 if t < 60 else min(1.0, 0.5 + (t - 60) / 10)
    row += [fader, 0.5, 0.5, 0.5, 0.5, color, 0.5, 1]
with open(d + "/state.csv", "w", newline="") as f:
    csv.writer(f).writerows(rows)

r = s.build(d)
bpms = [t[1] for t in r["tempos"]]
assert [x[1] for x in r["periods"]] == [0, 1], r["periods"]        # flick ignored, real switch kept
assert abs(bpms[0] - 120) < 0.2, bpms[:3]
assert any(abs(b - 124.8) < 0.3 for b in bpms), bpms               # follows the pitch change
assert any(abs(b - 100) < 0.3 for b in bpms), bpms                 # follows the new master
assert r["errors"] and r["errors"][len(r["errors"]) // 2] < 5, r["errors"][-5:]
assert r["clips"][0]["move"] == 1, r["clips"]                      # the scratch has its own clip
assert r["clips"][0]["play"] >= 3, r["clips"]                      # cut at the jump and around the scratch

root = ET.fromstring(gzip.open(r["als"]).read())
assert root.get("MinorVersion") == "12.0_12402" and root.get("SchemaChangeCount") == "1"  # same as the devices
assert len(root.find("LiveSet/Tracks").findall("AudioTrack")) == 2
ids = [e.get("Id") for e in root.iter() if e.tag.endswith("Target") and "Id" in e.attrib]
assert len(ids) == len(set(ids)), "duplicate automation target ids"
assert int(root.find("LiveSet/NextPointeeId").get("Value")) > max(int(i) for i in ids)
assert r["lane_points"] > 20, r["lane_points"]
targets = set(ids)
for track in root.find("LiveSet/Tracks").findall("AudioTrack")[:2]:
    devices = track.find("DeviceChain/DeviceChain/Devices")
    assert [d.tag for d in devices] == ["FilterEQ3", "AutoFilter", "AutoFilter", "StereoGain", "StereoGain"]
    assert all(d.find("On/Manual").get("Value") == "false" for d in devices)  # moves are in the audio
    assert not list(devices[0].iter("KeyMidi")), "EQ Three with MIDI mappings crashes Live"
    assert devices[0].find("LowOn/Manual").get("Value") == "true" and devices[0].find("HighOn/Manual").get("Value") == "true"
    envelopes = track.findall("AutomationEnvelopes/Envelopes/AutomationEnvelope")
    assert len(envelopes) == 7
    for envelope in envelopes:
        assert envelope.find("EnvelopeTarget/PointeeId").get("Value") in targets
        times = [float(e.get("Time")) for e in envelope.iter("FloatEvent")]
        assert times == sorted(times), times[:6]
assert s.lowpass_cutoff(0.5) == 135 and s.highpass_cutoff(0.5) == 20 and s.eq_gain(0.5) == 1
note = lambda hz: 69 + 12 * math.log2(hz / 440)
assert abs(s.lowpass_cutoff(0.3) - note(4000)) < 0.01 and abs(s.highpass_cutoff(1.0) - note(8000)) < 0.01
assert s.lowpass_cutoff(0.1) < s.lowpass_cutoff(0.3) < s.lowpass_cutoff(0.45)
assert s.highpass_cutoff(0.65) < s.highpass_cutoff(0.95)
assert s.eq_gain(0) == 0.05
tempos = r["tempos"]
beats = [t[0] for t in tempos]
checked = 0
for clip in root.iter("AudioClip"):
    assert float(clip.get("Time")) >= 0
    marks = [(float(m.get("SecTime")), float(m.get("BeatTime"))) for m in clip.find("WarpMarkers")]
    assert len(marks) >= 2
    for (s0, b0), (s1, b1) in zip(marks, marks[1:]):
        assert s1 > s0 and b1 > b0
        i = max(0, min(len(tempos) - 1, __import__("bisect").bisect_right(beats, (b0 + b1) / 2) - 1))
        rate = (b1 - b0) * 60 / tempos[i][1] / (s1 - s0)   # clip seconds per real second
        assert abs(rate - 1) < 0.002, (rate, s0, s1, tempos[i])
        checked += 1
assert checked > 10
print("Set generator checks passed:", r["clips"], "tempo changes", len(tempos) - 1, "median error %.1f ms" % r["errors"][len(r["errors"]) // 2], "worst %.1f" % r["errors"][-1])

# A clean recording: effects on, as-played audio below on muted tracks.
for name in ("deck1", "deck2"):
    shutil.copy(f"{d}/{name}.wav", f"{d}/{name}_clean.wav")
r = s.build(d)
root = ET.fromstring(gzip.open(r["als"]).read())
tracks = root.find("LiveSet/Tracks").findall("AudioTrack")
assert [t.find("Name/EffectiveName").get("Value") for t in tracks] == ["Deck 1", "Deck 2", "Deck 1 As Played", "Deck 2 As Played"]
groups = root.find("LiveSet/Tracks").findall("GroupTrack")
assert [g.find("Name/EffectiveName").get("Value") for g in groups] == ["Clean + FX", "As Played"]
assert [g.find("DeviceChain/Mixer/Speaker/Manual").get("Value") for g in groups] == ["true", "false"]
assert [t.find("TrackGroupId").get("Value") for t in tracks] == [groups[0].get("Id")] * 2 + [groups[1].get("Id")] * 2
assert [t.tag for t in root.find("LiveSet/Tracks")][:6] == ["GroupTrack", "AudioTrack", "AudioTrack", "GroupTrack", "AudioTrack", "AudioTrack"]
assert all(t.find("DeviceChain/AudioOutputRouting/Target").get("Value") == "AudioOut/GroupTrack" for t in tracks)
assert all(len(g.find("DeviceChain/DeviceChain/Devices")) == 0 for g in groups)
every = [t.get("Id") for t in root.find("LiveSet/Tracks")]
assert len(every) == len(set(every)), every
for t in tracks[:2]:
    assert [x.find("On/Manual").get("Value") for x in t.find("DeviceChain/DeviceChain/Devices")] == ["true", "true", "true", "true", "true"]
    assert {c.find("SampleVolume").get("Value") for c in t.iter("AudioClip")} == {"0.25"}
assert all(len(t.find("DeviceChain/DeviceChain/Devices")) == 0 for t in tracks[2:])
paths = [c.find("SampleRef/FileRef/Path").get("Value") for t in tracks for c in t.iter("AudioClip")]
assert any(p.endswith("Deck 1 Clean.wav") for p in paths) and any(p.endswith("Deck 1.wav") for p in paths)
ids = [e.get("Id") for e in root.iter() if e.tag.endswith("Target") and "Id" in e.attrib]
assert len(ids) == len(set(ids)), [i for i in ids if ids.count(i) > 1][:5]
assert int(root.find("LiveSet/NextPointeeId").get("Value")) > max(int(i) for i in ids)
print("Clean Set and group checks passed")

# Track names: clips take the name of what was loaded, including characters XML must escape.
def line(text):
    raw = text.encode("utf-16-le")
    return (bytes(32) + (len(raw) // 2).to_bytes(2, "little") + raw + bytes(512)).hex()[:1024]
assert s.title_from_line(line("REKA")) == "REKA"
assert s.title_at([(500, "A"), (900, "B")], 0) == "A" and s.title_at([(500, "A"), (900, "B")], 950) == "B"
with open(f"{d}/title_lines.csv", "w") as f:
    f.write("load_frame,deck,title_hex,artist_hex\n")
    f.write("0,1," + line("Em Gái & <Mưa>") + ",\n")
    f.write("0,2," + line("Second") + ",\n")
r = s.build(d)
root = ET.fromstring(gzip.open(r["als"]).read())
names = {c.find("Name").get("Value") for c in root.iter("AudioClip")}
assert "Em Gái & <Mưa>" in names and "Second" in names, names
assert "Em Gái & <Mưa> Scratch" in names, names
print("Track name checks passed")

# A recording from before the type fix played Space on that knob: its filter stays off.
rows = list(csv.reader(open(d + "/state.csv", newline="")))
kept = rows[0].index("color_type")
original = [list(row) for row in rows]
for row in rows[1:]:
    row[kept] = 5
with open(d + "/state.csv", "w", newline="") as f:
    csv.writer(f).writerows(rows)
r = s.build(d)
t = ET.fromstring(gzip.open(r["als"]).read()).find("LiveSet/Tracks").findall("AudioTrack")[0]
assert [x.find("On/Manual").get("Value") for x in t.find("DeviceChain/DeviceChain/Devices")] == ["true", "false", "false", "true", "true"]
with open(d + "/state.csv", "w", newline="") as f:
    csv.writer(f).writerows(original)

# And with the master output.
shutil.copy(f"{d}/deck1.wav", f"{d}/master.wav")
r = s.build(d)
root = ET.fromstring(gzip.open(r["als"]).read())
tracks = root.find("LiveSet/Tracks").findall("AudioTrack")
assert [t.find("Name/EffectiveName").get("Value") for t in tracks][-1] == "Master" and len(tracks) == 5
assert tracks[-1].find("DeviceChain/Mixer/Speaker/Manual").get("Value") == "false"
assert tracks[-1].find("TrackGroupId").get("Value") == "-1"
clips = list(tracks[-1].iter("AudioClip"))
assert len(clips) == 1 and clips[0].find("SampleRef/FileRef/Path").get("Value").endswith("Master.wav")
all_ids = [c.get("Id") for t in tracks for c in t.iter("AudioClip")]
assert len(all_ids) == len(set(all_ids))
print("Master track checks passed")
