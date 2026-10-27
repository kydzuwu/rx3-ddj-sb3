#include <assert.h>
#include <stdio.h>
#include "sb3_map.h"
#include "stereo_map.h"

static void check(int status, int data1, int data2, int expected_status,
                  int expected_data1, int expected_data2) {
    unsigned char out[SB3_OUT_MAX][3] = {{0}};
    assert(sb3_translate(status, data1, data2, out) == 1);
    assert(out[0][0] == expected_status);
    assert(out[0][1] == expected_data1);
    assert(out[0][2] == expected_data2);
}

static void check_multi(int status, int data1, int data2, int count,
                        const unsigned char expected[][3]) {
    unsigned char out[SB3_OUT_MAX][3] = {{0}};
    assert(sb3_translate(status, data1, data2, out) == count);
    for (int i = 0; i < count; ++i)
        for (int j = 0; j < 3; ++j) assert(out[i][j] == expected[i][j]);
}

int main(void) {
    unsigned char out[SB3_OUT_MAX][3];
    check(0x90, 11, 127, 0x90, 11, 127);  /* play deck 1 */
    check(0x91, 20, 127, 0x91, 77, 127);  /* loop deck 2 */
    check(0x90, 18, 0, 0x90, 16, 0);      /* IN release -> loop in */
    check(0x90, 19, 127, 0x90, 17, 127); /* OUT -> loop out */
    check(0x90, 98, 127, 0x90, 83, 127); /* shift OUT -> 2X */
    check(0x90, 97, 127, 0x90, 81, 127); /* shift IN -> 1/2X */
    check(0x90, 80, 127, 0x90, 62, 127); /* shift auto loop -> playlist */
    check(0xb6, 31, 64, 0xb6, 31, 64);  /* crossfader */
    check(0xb6, 13, 64, 0xb6, 13, 64);  /* headphones level */
    check(0xb0, 19, 127, 0xb0, 19, 127);/* channel fader */
    check(0x90, 88, 127, 0x90, 88, 127);/* sync */
    check(0x96, 91, 127, 0x96, 99, 127); /* master cue */
    check(0x96, 88, 127, 0x96, 104, 127);/* shift load L */
    check(0x96, 89, 0, 0x96, 122, 0);    /* shift load R release */
    check(0x98, 4, 127, 0x99, 4, 127);   /* deck 2 hot cue */
    check(0x98, 8, 127, 0x9a, 0, 127);   /* deck 2 delete hot cue */
    check(0xb1, 31, 65, 0xb1, 41, 65);  /* shifted jog */
    check(0xb6, 64, 127, 0xb6, 64, 127);/* browse */
    check(0x80, 11, 64, 0x90, 11, 0);   /* note off */
    {   /* FX1 deck 1: leg B off, leg A on (= CH1), then FX ON */
        const unsigned char d1[][3] = {{0x95, 17, 0}, {0x94, 16, 127}, {0x94, 71, 127}};
        const unsigned char d2[][3] = {{0x94, 16, 0}, {0x95, 17, 127}, {0x95, 71, 127}};
        const unsigned char rel[][3] = {{0x95, 71, 0}};
        const unsigned char master[][3] = {{0x94, 16, 127}, {0x95, 17, 127}};
        check_multi(0x94, 71, 127, 3, d1);
        check_multi(0x95, 71, 127, 3, d2);
        check_multi(0x95, 71, 0, 1, rel);
        check_multi(0x94, 99, 127, 2, master);
        assert(!sb3_translate(0x94, 99, 0, out));
    }
    check(0x95, 72, 127, 0x94, 74, 127); /* FX2 -> BEAT < (always ch4) */
    check(0x94, 73, 127, 0x94, 75, 127); /* FX3 -> BEAT > */
    check(0x95, 100, 127, 0x94, 100, 127);/* SHIFT+FX2 -> FX type back */
    check(0x94, 101, 127, 0x94, 99, 127); /* SHIFT+FX3 -> FX type forward */
    check(0xb5, 2, 64, 0xb5, 2, 64);     /* FX knob -> depth */
    assert(!sb3_translate(0xb4, 4, 64, out)); /* duplicate knob CCs dropped */
    check(0x90, 26, 127, 0x90, 26, 127); /* KEY LOCK -> master tempo */
    check(0x91, 96, 127, 0x91, 96, 127); /* SHIFT+KEY LOCK -> tempo range */
    assert(!sb3_translate(0x90, 23, 127, out)); /* unmapped deck note */
    assert(!sb3_translate(0xf0, 0, 0, out));
    {   /* 6-ch guest voice: master 0/1, cue 2/3 -> SB3 ch 1-4; padding dropped */
        const float six[6] = {1, 2, 3, 4, 5, 6};
        float sb3[4], mac[2];
        rx3_map_channels(sb3, 4, 0, six, 6, 1);
        assert(sb3[0] == 1 && sb3[1] == 2 && sb3[2] == 3 && sb3[3] == 4);
        rx3_map_channels(mac, 2, 0, six, 6, 1);
        assert(mac[0] == 1 && mac[1] == 2);
        const float two[2] = {7, 8};   /* stereo voice: master copied to phones */
        rx3_map_channels(sb3, 4, 0, two, 2, 1);
        assert(sb3[0] == 7 && sb3[1] == 8 && sb3[2] == 7 && sb3[3] == 8);
        float mixed[4] = {0, 0, 0, 0};  /* master voice 1/2, cue voice 3/4 */
        rx3_map_range(mixed, 4, 0, two, 2, 1, 0, 2);
        rx3_map_range(mixed, 4, 0, (const float[]){5, 6}, 2, 1, 2, 4);
        assert(mixed[0] == 7 && mixed[1] == 8 && mixed[2] == 5 && mixed[3] == 6);
        float mac2[2] = {0, 0};          /* stereo Mac output: cue voice writes nothing */
        rx3_map_range(mac2, 2, 0, two, 2, 1, 2, 4);
        assert(mac2[0] == 0 && mac2[1] == 0);
    }
    puts("SB3 translation checks passed");
}
