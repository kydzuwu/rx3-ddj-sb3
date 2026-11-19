/* Prints every SB3 <-> SB2 translation in sb2_map.h, for make_rekordbox_mapping.py. */
#include <stdio.h>
#include "sb2_map.h"
int main(void) {
    unsigned char out[SB2_OUT_MAX][3];
    for (int k = 0x90; k <= 0xB0; k += 0x20)
        for (int ch = 0; ch < 16; ++ch)
            for (int d1 = 0; d1 < 128; ++d1) {
                int n = sb3_to_sb2((unsigned char)(k | ch), (unsigned char)d1, 127, out);
                for (int i = 0; i < n; ++i) printf("IN %02X %02X %02X %02X\n", k | ch, d1, out[i][0], out[i][1]);
                n = sb2_to_sb3((unsigned char)(k | ch), (unsigned char)d1, 127, out);
                for (int i = 0; i < n; ++i) printf("OUT %02X %02X %02X %02X\n", k | ch, d1, out[i][0], out[i][1]);
            }
    return 0;
}
