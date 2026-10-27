/* DDJ-SB3 -> DDJ-FLX4 guest MIDI compatibility map.
 * Values come from AlphaTheta's DDJ-SB3 MIDI list v1.01 and the existing
 * map_flx4.c. Unmapped SB3 controls are deliberately dropped. */
#ifndef SB3_MAP_H
#define SB3_MAP_H

#define SB3_OUT_MAX 3

/* Returns how many guest messages were written to out (0 = dropped). */
static int sb3_translate(unsigned char in_status, unsigned char in_data1,
                         unsigned char in_data2, unsigned char out[SB3_OUT_MAX][3]) {
    unsigned char kind = in_status & 0xf0, ch = in_status & 0x0f;
    unsigned char note = in_data1;
    if (kind == 0x80) kind = 0x90, in_data2 = 0;
    if (kind == 0x90) {
        if (ch <= 1) {
            switch (note) {
            case 18: note = 16; break;  /* IN (printed on the button) -> LOOP IN */
            case 19: note = 17; break;  /* OUT -> LOOP OUT */
            case 20: note = 77; break;  /* AUTO LOOP -> 4 BEAT/EXIT */
            case 97: note = 81; break;  /* SHIFT+IN (1/2X) -> CUE/LOOP CALL < */
            case 98: note = 83; break;  /* SHIFT+OUT (2X) -> CUE/LOOP CALL > */
            case 80: note = 62; break;  /* SHIFT+AUTO LOOP -> PLAYLIST */
            case 11: case 12: case 54: case 63: case 88: case 84:
            case 27: case 30: case 32: case 34: case 92:
            case 71: case 72: /* SHIFT+PLAY, SHIFT+CUE */
            case 26: case 96: break;  /* KEY LOCK -> master tempo, SHIFT -> tempo range */
            default: return 0;
            }
        } else if (ch == 6) {
            switch (note) {
            case 91: note = 99; break;  /* MASTER CUE */
            case 88: note = 104; break; /* SHIFT+LOAD L -> TRACK FILTER */
            case 89: note = 122; break; /* SHIFT+LOAD R -> SHORTCUT */
            case 65: case 66: case 70: case 71: break;
            default: return 0;
            }
        } else if (ch == 4 || ch == 5) {
            /* SB3 FX1-3 per deck (FX 1 = ch 5, FX 2 = ch 6) -> the FLX4 Beat FX
             * section. The guest's FX SELECT / BEAT handlers only listen on
             * CH_FXA (ch 4). FX CH SELECT is a lever with one leg per channel
             * (ch4 note 16, ch5 note 17); the leg going off is sent first so
             * the guest never sees both on (= MASTER) on the way. */
            int n = 0;
            switch (note) {
            case 71: /* FX1: Beat FX on this deck's channel, then toggle on/off */
                if (in_data2) {
                    out[n][0] = ch == 4 ? 0x95 : 0x94; out[n][1] = ch == 4 ? 17 : 16; out[n++][2] = 0;
                    out[n][0] = 0x90 | ch; out[n][1] = ch == 4 ? 16 : 17; out[n++][2] = 0x7f;
                }
                out[n][0] = 0x90 | ch; out[n][1] = 71; out[n++][2] = in_data2;
                return n;
            case 99: /* SHIFT+FX1: Beat FX target MASTER (both legs) */
                if (!in_data2) return 0;
                out[0][0] = 0x94; out[0][1] = 16; out[0][2] = 0x7f;
                out[1][0] = 0x95; out[1][1] = 17; out[1][2] = 0x7f;
                return 2;
            case 72:  note = 74; break; /* FX2: BEAT < */
            case 73:  note = 75; break; /* FX3: BEAT > */
            case 100: note = 100; break; /* SHIFT+FX2: previous Beat FX type */
            case 101: note = 99; break;  /* SHIFT+FX3: next Beat FX type */
            default: return 0;
            }
            out[0][0] = 0x94; out[0][1] = note; out[0][2] = in_data2;
            return 1;
        } else if (ch == 7 || ch == 8) {
            unsigned char deck = ch == 7 ? 0 : 1;
            if (note < 8 || (note >= 16 && note < 24) ||
                (note >= 32 && note < 40) || (note >= 48 && note < 56)) {
                ch = deck ? 9 : 7;
            } else if (note >= 8 && note < 16) {
                ch = deck ? 10 : 8;
                note -= 8; /* SHIFT+HOT CUE -> FLX4 delete layer */
            } else return 0;
        } else return 0;
        out[0][0] = 0x90 | ch; out[0][1] = note; out[0][2] = in_data2;
        return 1;
    }
    if (kind == 0xb0) {
        if (ch <= 1) {
            /* Pitch, jog, trim, EQ and channel fader are wire-compatible. */
            switch (note) {
            case 0: case 32: case 33: case 34: case 35:
            case 4: case 7: case 11: case 15: case 19:
            case 36: case 39: case 43: case 47: case 51: /* 14-bit LSBs */
                break;
            case 31: note = 41; break; /* SHIFT+platter */
            default: return 0;
            }
        } else if (ch == 6) {
            /* Filter, master level, headphone level, crossfader, browse. */
            switch (note) {
            case 23: case 24: case 8: case 40: case 13:
            case 31: case 64: case 100:
            case 12: case 44: /* HEADPHONES MIX */
            case 55: case 56: case 45: case 63: break; /* 14-bit LSBs */
            default: return 0;
            }
        } else if (ch == 4 || ch == 5) {
            if (note != 2) return 0; /* FX knob sends CC 2/4/6 at once: pass 2 -> Beat FX depth */
        } else return 0;
        out[0][0] = 0xb0 | ch; out[0][1] = note; out[0][2] = in_data2;
        return 1;
    }
    return 0;
}
#endif
