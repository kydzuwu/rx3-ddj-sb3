#!/usr/bin/env python3
"""Turns rekordbox 5's own DDJ-SB2 mapping into a DDJ-SB3 mapping, using sb2_map.h.

Usage: make_rekordbox_mapping.py [rekordbox DDJ-SB2.midi.csv] [output .midi.csv]
Then in rekordbox: Preferences > Controller > MIDI (or the MIDI button), pick DDJ-SB3, IMPORT.
Each SB2 row is rewritten with the SB3 control that sends that SB2 message, and its light with
the SB3 light. Where two SB3 controls give the same SB2 message (IN and the loop pad), the row
is written once for each. Rows with no SB3 control are left out and listed.
"""
import os, subprocess, sys, tempfile
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = sys.argv[1] if len(sys.argv) > 1 else "/Applications/rekordbox 5/rekordbox.app/Contents/Resources/DDJ-SB2.midi.csv"
TARGET = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "rekordbox", "DDJ-SB3.midi.csv")


def translation_table():
    """{('IN'|'OUT', sb2 status, sb2 note): [(sb3 status, sb3 note), ...]}"""
    with tempfile.TemporaryDirectory() as tmp:
        tool = os.path.join(tmp, "sb2_table")
        subprocess.run(["clang", "-O2", "-I", HERE, "-o", tool, os.path.join(HERE, "sb2_table.c")], check=True)
        lines = subprocess.run([tool], check=True, capture_output=True, text=True).stdout.splitlines()
    table = defaultdict(list)
    for line in lines:
        way, a_status, a_note, b_status, b_note = line.split()
        if way == "IN":     # SB3 a -> SB2 b: look up by the SB2 message
            table[("IN", int(b_status, 16), int(b_note, 16))].append((int(a_status, 16), int(a_note, 16)))
        else:               # SB2 a -> SB3 b
            table[("OUT", int(a_status, 16), int(a_note, 16))].append((int(b_status, 16), int(b_note, 16)))
    # A message the translator only passes through is not an SB3 control when sb2_map.h sends that
    # SB2 message from another SB3 control (MASTER LEVEL B6 09, HP B6 07, slip 4E ...). Pad notes
    # are always explicit, so they stay.
    for key, sources in table.items():
        if key[0] != "IN" or len(sources) < 2:
            continue
        kept = [src for src in sources if not (src == key[1:] and (src[0] & 15) not in (7, 8))]
        if kept:
            table[key] = kept
    return table


def is_hex(text):
    return len(text) == 4 and all(c in "0123456789ABCDEFabcdef" for c in text)


def messages(base, decks):
    """The (status, note) a row means, one per deck column; a deck column may be a channel or full hex."""
    out = []
    for deck in decks:
        if not deck:
            out.append(None)
        elif is_hex(deck):
            out.append((int(deck[:2], 16), int(deck[2:], 16)))
        elif base:
            out.append(((int(base[0], 16) << 4) | int(deck), int(base[2:], 16)))
        else:
            out.append(None)
    return out


def klass(status):
    ch = status & 15
    return "deck" if ch <= 1 else "pad" if ch in (7, 8) else "other%d" % ch


def main():
    table = translation_table()
    rows = open(SOURCE, newline="", encoding="utf-8").read().replace("\r\n", "\n").split("\n")
    out_rows, missing, written = [], [], 0
    for row in rows:
        c = row.split(",")
        if row.startswith("@file"):
            out_rows.append("@file,1,DDJ-SB3")
            continue
        if len(c) < 14 or row.startswith("#") or not (c[3] or any(is_hex(x) for x in c[4:8])) or c[3].upper().startswith("FFF"):
            out_rows.append(row)
            continue
        c += [""] * (15 - len(c))
        name, inp, decks, outp, odecks = c[0], c[3].upper(), c[4:8], c[8].upper(), c[9:13]
        two_deck = any(decks[:2]) and not inp == ""
        if not any(decks):                      # global: input is the full message
            sources = table.get(("IN", int(inp[:2], 16), int(inp[2:], 16)), [])
            outs = table.get(("OUT", int(outp[:2], 16), int(outp[2:], 16)), []) if is_hex(outp) else []
            if not sources:
                missing.append(f"{name} ({c[1]}) {inp}")
                continue
            for source in sources:
                light = next((o for o in outs if klass(o[0]) == klass(source[0])), outs[0] if outs else None)
                n = list(c)
                n[3] = "%02X%02X" % source
                n[8] = "%02X%02X" % light if light else ""
                out_rows.append(",".join(n[:max(len(row.split(",")), 14)]))
                written += 1
            continue
        wanted = messages(inp, decks[:2])
        found = [table.get(("IN",) + m, []) if m else [] for m in wanted]
        if not all(found[i] for i in range(2) if wanted[i]):
            missing.append(f"{name} ({c[1]}) {inp or decks[0]}")
            continue
        lights = [table.get(("OUT",) + m, []) if m else [] for m in messages(outp, odecks[:2])]
        for i, first in enumerate(found[0] if wanted[0] else found[1]):
            pair = [found[d][i] if wanted[d] and i < len(found[d]) else None for d in range(2)]
            n = list(c)
            if is_hex(decks[0]) or is_hex(decks[1]):    # full hex per deck (LOAD)
                n[3] = ""
                n[4:8] = ["%02X%02X" % p if p else "" for p in pair] + ["", ""]
            else:
                kinds = {p[0] & 0xF0 for p in pair if p}
                notes = {p[1] for p in pair if p}
                if len(kinds) != 1 or len(notes) != 1:
                    missing.append(f"{name} ({c[1]}): decks differ")
                    continue
                n[3] = "%X0%02X" % (kinds.pop() >> 4, notes.pop())
                n[4:8] = [str(p[0] & 15) if p else "" for p in pair] + ["", ""]
            # light: same kind of control as the input (a deck button lights the deck button)
            chosen = []
            for d in range(2):
                options = lights[d] if d < len(lights) else []
                k = klass(pair[d][0]) if pair[d] else None
                chosen.append(next((o for o in options if klass(o[0]) == k), options[0] if options else None))
            if any(chosen) and len({o[1] for o in chosen if o}) == 1 and len({o[0] & 0xF0 for o in chosen if o}) == 1:
                o = next(o for o in chosen if o)
                n[8] = "%X0%02X" % (o[0] >> 4, o[1])
                n[9:13] = [str(x[0] & 15) if x else "" for x in chosen] + ["", ""]
            else:
                n[8] = ""
                n[9:13] = [""] * 4
            out_rows.append(",".join(n[:max(len(row.split(",")), 14)]))
            written += 1
    os.makedirs(os.path.dirname(TARGET), exist_ok=True)
    with open(TARGET, "w", newline="", encoding="utf-8") as f:
        f.write("\r\n".join(out_rows))
    print(f"wrote {written} rows to {TARGET}")
    if missing:
        print(f"left out (no SB3 control sends it): {len(missing)}")
        for m in missing:
            print("  ", m)


if __name__ == "__main__":
    main()
