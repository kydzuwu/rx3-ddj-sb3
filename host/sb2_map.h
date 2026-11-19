/* DDJ-SB3 <-> DDJ-SB2 MIDI, so rekordbox 5 can drive an SB3 with its own DDJ-SB2 mapping
 * (rekordbox.app/Contents/Resources/DDJ-SB2.midi.csv). Sources: that file and AlphaTheta's
 * DDJ-SB3 MIDI list (../ddj-sb3-midi.pdf).
 *
 * Most of the two controllers is the same message: transport, jogs, tempo, mixer, FX and the
 * four pad-mode buttons (1B / 1E / 20 / 22, with SHIFT 69 / 6B / 6D). What differs:
 *  - the SB3 has 8 pads per mode, sent as a block of 8 notes per mode (00, 10, 20, ... 70, SHIFT
 *    +8); the SB2 layout rekordbox expects puts pads 5-8 elsewhere, and has no SHIFT layer in most
 *    modes, so those SB3 notes are dropped rather than landing on another function;
 *  - the SB3's deck IN / OUT / AUTO LOOP buttons, which the SB2 had as loop pads;
 *  - master level and headphone knobs use other CC numbers.
 * Which SB3 pad block goes with which mode button is read off the PDF's order: unverified. */
#ifndef SB2_MAP_H
#define SB2_MAP_H

#define SB2_OUT_MAX 3

static int sb2_put(unsigned char out[][3], int n, unsigned char s, unsigned char d1, unsigned char d2) {
    out[n][0] = s; out[n][1] = d1; out[n][2] = d2;
    return n + 1;
}

/* SB3 pad note (one 8-note block per mode) -> SB2 note, or -1 to drop. */
static int sb2_pad_note(unsigned char note) {
    int block = note & 0x70, shift = note & 0x08, k = note & 0x07;
    static const unsigned char manual[8] = {0x20, 0x21, 0x22, 0x23, 0x2B, 0x28, 0x29, 0x2A};
    switch (block) {
    case 0x00: case 0x40:   /* HOT CUE (and SHIFT+HOT CUE, which rekordbox also treats as HOT CUE) */
        return (k < 4 ? k : 0x40 + k - 4) + (shift ? 8 : 0);
    case 0x10:              /* auto loop: 1, 2, 4, 8 on top, 1/2 .. 1/16 below */
        return shift ? -1 : k < 4 ? 0x10 + k : 0x1B - (k - 4);
    case 0x20:              /* manual loop */
        return shift ? -1 : manual[k];
    case 0x30:              /* sampler: the SB2 has four slots */
        return k < 4 ? 0x30 + k + (shift ? 8 : 0) : -1;
    case 0x50: case 0x60:   /* pad FX 1 / 2 */
        return shift ? -1 : (k < 4 ? block + k : block + 8 + k - 4);
    case 0x70:              /* bank */
        return shift || k >= 4 ? -1 : 0x70 + k;
    }
    return -1;
}

/* SB3 -> rekordbox. Returns how many messages were written to out (0 = dropped). */
static int sb3_to_sb2(unsigned char status, unsigned char d1, unsigned char d2,
                      unsigned char out[SB2_OUT_MAX][3]) {
    unsigned char kind = status & 0xf0, ch = status & 0x0f;
    if (kind == 0x80) kind = 0x90, d2 = 0;
    if (kind == 0x90 && ch <= 1) {
        unsigned char loop = 0x97 + ch;     /* the SB2's loop controls live on the pad channel */
        switch (d1) {
        case 0x12: return sb2_put(out, 0, loop, 0x20, d2);        /* IN -> loop in */
        case 0x13: return sb2_put(out, 0, loop, 0x21, d2);        /* OUT -> loop out */
        case 0x61: return sb2_put(out, 0, loop, 0x23, d2);        /* SHIFT+IN (1/2X) -> loop half */
        case 0x62: return sb2_put(out, 0, loop, 0x2B, d2);        /* SHIFT+OUT (2X) -> loop double */
        case 0x14: return sb2_put(out, 0, loop, 0x12, d2);        /* AUTO LOOP -> 4-beat loop */
        case 0x50: return sb2_put(out, 0, loop, 0x22, d2);        /* SHIFT+AUTO LOOP -> reloop/exit */
        case 0x40: return sb2_put(out, 0, 0x90 + ch, 0x4E, d2);   /* SHIFT+VINYL -> slip */
        case 0x6E: return sb2_put(out, 0, 0x90 + ch, 0x6F, d2);   /* SHIFT+SAMPLER -> bank */
        }
    }
    if (kind == 0x90 && (ch == 7 || ch == 8)) {
        int note = sb2_pad_note(d1);
        return note < 0 ? 0 : sb2_put(out, 0, 0x90 | ch, (unsigned char)note, d2);
    }
    if (kind == 0xb0 && ch == 6) {
        switch (d1) {
        case 0x08: return sb2_put(out, 0, status, 0x09, d2);      /* MASTER LEVEL */
        case 0x28: return sb2_put(out, 0, status, 0x29, d2);
        case 0x0D: return sb2_put(out, 0, status, 0x07, d2);      /* HEADPHONES LEVEL */
        case 0x2D: return sb2_put(out, 0, status, 0x27, d2);
        case 0x0C: return sb2_put(out, 0, status, 0x05, d2);      /* HEADPHONES MIX */
        case 0x2C: return sb2_put(out, 0, status, 0x25, d2);
        }
    }
    if (kind < 0x80 || kind == 0xf0) return 0;
    return sb2_put(out, 0, kind | ch, d1, d2);
}

/* rekordbox's SB2 lights -> SB3 lights. A pad light goes to the SB3 note of the same pad. */
static int sb2_to_sb3(unsigned char status, unsigned char d1, unsigned char d2,
                      unsigned char out[SB2_OUT_MAX][3]) {
    unsigned char kind = status & 0xf0, ch = status & 0x0f;
    if (kind == 0x80) kind = 0x90, d2 = 0;
    if (kind == 0x90 && (ch == 7 || ch == 8)) {
        unsigned char s = 0x90 | ch, deck = 0x90 + ch - 7;
        int n = 0;
        if (d1 <= 0x03) return sb2_put(out, 0, s, d1, d2);                       /* hot cue 1-4 */
        if (d1 >= 0x40 && d1 <= 0x43) {                                         /* hot cue 5-8 */
            n = sb2_put(out, n, s, (unsigned char)(0x04 + d1 - 0x40), d2);
            return sb2_put(out, n, s, (unsigned char)(0x44 + d1 - 0x40), d2);
        }
        if (d1 >= 0x10 && d1 <= 0x13) {                                         /* loop 1-8 */
            n = sb2_put(out, n, s, d1, d2);
            return d1 == 0x12 ? sb2_put(out, n, deck, 0x14, d2) : n;            /* + AUTO LOOP */
        }
        if (d1 >= 0x18 && d1 <= 0x1B) return sb2_put(out, 0, s, (unsigned char)(0x14 + 0x1B - d1), d2);
        switch (d1) {                                                           /* manual loop */
        case 0x20: n = sb2_put(out, 0, s, 0x20, d2); return sb2_put(out, n, deck, 0x12, d2);
        case 0x21: n = sb2_put(out, 0, s, 0x21, d2); return sb2_put(out, n, deck, 0x13, d2);
        case 0x22: case 0x23: return sb2_put(out, 0, s, d1, d2);
        case 0x2B: return sb2_put(out, 0, s, 0x24, d2);
        case 0x28: return sb2_put(out, 0, s, 0x25, d2);
        case 0x29: return sb2_put(out, 0, s, 0x26, d2);
        case 0x2A: return sb2_put(out, 0, s, 0x27, d2);
        }
        if (d1 >= 0x30 && d1 <= 0x33) return sb2_put(out, 0, s, d1, d2);       /* sampler */
        if ((d1 >= 0x50 && d1 <= 0x53) || (d1 >= 0x60 && d1 <= 0x63) || (d1 >= 0x70 && d1 <= 0x73))
            return sb2_put(out, 0, s, d1, d2);
        if ((d1 >= 0x58 && d1 <= 0x5B) || (d1 >= 0x68 && d1 <= 0x6B))
            return sb2_put(out, 0, s, (unsigned char)(d1 - 4), d2);           /* pad FX 5-8 */
        return 0;
    }
    if (kind == 0x90 && ch <= 1 && d1 == 0x6F) return sb2_put(out, 0, status, 0x6E, d2);
    if (kind < 0x80 || kind == 0xf0) return 0;
    return sb2_put(out, 0, kind | ch, d1, d2);
}
#endif
