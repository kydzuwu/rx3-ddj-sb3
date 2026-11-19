#include <assert.h>
#include <stdio.h>
#include "sb2_map.h"

static void to_sb2(int s, int d1, int d2, int es, int ed1, int ed2) {
    unsigned char out[SB2_OUT_MAX][3];
    assert(sb3_to_sb2(s, d1, d2, out) == 1);
    assert(out[0][0] == es && out[0][1] == ed1 && out[0][2] == ed2);
}

static void dropped(int s, int d1) {
    unsigned char out[SB2_OUT_MAX][3];
    assert(sb3_to_sb2(s, d1, 127, out) == 0);
}

int main(void) {
    unsigned char out[SB2_OUT_MAX][3];
    to_sb2(0x90, 0x0B, 127, 0x90, 0x0B, 127);   /* play: same on both */
    to_sb2(0xB1, 0x22, 65, 0xB1, 0x22, 65);     /* jog: same */
    to_sb2(0xB6, 0x1F, 64, 0xB6, 0x1F, 64);     /* crossfader: same */
    to_sb2(0x80, 0x0C, 64, 0x90, 0x0C, 0);      /* note off -> note on 0 */
    to_sb2(0x90, 0x12, 127, 0x97, 0x20, 127);   /* IN -> loop in, pad channel */
    to_sb2(0x91, 0x13, 127, 0x98, 0x21, 127);   /* deck 2 OUT -> loop out */
    to_sb2(0x90, 0x14, 127, 0x97, 0x12, 127);   /* AUTO LOOP -> 4-beat loop */
    to_sb2(0x90, 0x40, 127, 0x90, 0x4E, 127);   /* SHIFT+VINYL -> slip */
    to_sb2(0xB6, 0x08, 99, 0xB6, 0x09, 99);     /* master level */
    to_sb2(0xB6, 0x0D, 99, 0xB6, 0x07, 99);     /* headphones level */
    to_sb2(0xB6, 0x0C, 99, 0xB6, 0x05, 99);     /* headphones mix */
    to_sb2(0x97, 0x00, 127, 0x97, 0x00, 127);   /* hot cue 1 */
    to_sb2(0x97, 0x05, 127, 0x97, 0x41, 127);   /* hot cue 6 */
    to_sb2(0x98, 0x0D, 127, 0x98, 0x49, 127);   /* SHIFT hot cue 6 -> delete */
    to_sb2(0x97, 0x14, 127, 0x97, 0x1B, 127);   /* loop pad 5 -> 1/2 */
    to_sb2(0x97, 0x24, 127, 0x97, 0x2B, 127);   /* manual pad 5 -> double */
    to_sb2(0x97, 0x56, 127, 0x97, 0x5A, 127);   /* pad FX1 pad 7 */
    dropped(0x97, 0x1C);                        /* SHIFT in loop mode: nothing on the SB2 */
    dropped(0x97, 0x2C);                        /* ...and must not land on loop move */
    dropped(0x97, 0x35);                        /* sampler pads 5-8 */
    dropped(0xF0, 0x00);
    /* lights back */
    assert(sb2_to_sb3(0x97, 0x41, 127, out) == 2 && out[0][1] == 0x05 && out[1][1] == 0x45);
    assert(sb2_to_sb3(0x97, 0x20, 127, out) == 2 && out[1][0] == 0x90 && out[1][1] == 0x12);
    assert(sb2_to_sb3(0x98, 0x12, 127, out) == 2 && out[1][0] == 0x91 && out[1][1] == 0x14);
    assert(sb2_to_sb3(0x97, 0x1B, 127, out) == 1 && out[0][1] == 0x14);
    assert(sb2_to_sb3(0x90, 0x0B, 127, out) == 1 && out[0][1] == 0x0B);
    /* every SB3 pad note that is not dropped maps back to itself */
    for (int block = 0; block < 0x80; block += 0x10)
        for (int k = 0; k < 16; ++k) {
            int note = block + k, sb2 = sb2_pad_note((unsigned char)note);
            if (sb2 < 0 || (note & 8) || block == 0x40) continue;
            int n = sb2_to_sb3(0x97, (unsigned char)sb2, 1, out), found = 0;
            for (int i = 0; i < n; ++i) found |= out[i][1] == note;
            if (!found) { printf("pad %02X -> %02X has no light back\n", note, sb2); return 1; }
        }
    puts("SB3 <-> SB2 checks passed");
    return 0;
}
