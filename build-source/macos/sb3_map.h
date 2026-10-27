/* DDJ-SB3 -> DDJ-FLX4 guest MIDI compatibility map.
 * Values come from AlphaTheta's DDJ-SB3 MIDI list v1.01 and the existing
 * map_flx4.c. Unmapped SB3 controls are deliberately dropped. */
#ifndef SB3_MAP_H
#define SB3_MAP_H

static int sb3_translate(unsigned char in_status, unsigned char in_data1,
                         unsigned char in_data2, unsigned char out[3]) {
    unsigned char kind = in_status & 0xf0, ch = in_status & 0x0f;
    unsigned char note = in_data1;
    if (kind == 0x80) kind = 0x90, in_data2 = 0;
    if (kind == 0x90) {
        if (ch <= 1) {
            switch (note) {
            case 18: note = 81; break;  /* LOOP 1/2X -> CUE/LOOP CALL < */
            case 19: note = 83; break;  /* LOOP 2X -> CUE/LOOP CALL > */
            case 20: note = 77; break;  /* AUTO LOOP -> 4 BEAT/EXIT */
            case 11: case 12: case 54: case 63: case 88: case 84:
            case 27: case 30: case 32: case 34: break;
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
            if (note != 71) return 0;  /* first FX ON -> Beat FX ON */
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
        out[0] = 0x90 | ch; out[1] = note; out[2] = in_data2;
        return 1;
    }
    if (kind == 0xb0) {
        if (ch <= 1) {
            /* Pitch, jog, trim, EQ and channel fader are wire-compatible. */
            switch (note) {
            case 0: case 32: case 33: case 34: case 35:
            case 4: case 7: case 11: case 15: case 19:
                break;
            case 31: note = 41; break; /* SHIFT+platter */
            default: return 0;
            }
        } else if (ch == 6) {
            /* Filter, master level, headphone level, crossfader, browse. */
            switch (note) {
            case 23: case 24: case 8: case 40: case 13:
            case 31: case 64: case 100: break;
            default: return 0;
            }
        } else if (ch == 4 || ch == 5) {
            if (note != 2) return 0; /* first FX level -> Beat FX depth */
        } else return 0;
        out[0] = 0xb0 | ch; out[1] = note; out[2] = in_data2;
        return 1;
    }
    return 0;
}
#endif
