#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "midi_output.h"

static unsigned char received[32][256];
static size_t lengths[32], count;
static void collect(const unsigned char *bytes, size_t length, void *unused)
{
    (void)unused;
    assert(count < 32 && length <= 256);
    memcpy(received[count], bytes, length);
    lengths[count++] = length;
}
static void feed(struct rx3_midi_output *p, const unsigned char *bytes, size_t size)
{
    for (size_t i = 0; i < size; ++i) rx3_midi_output_feed(p, bytes[i], collect, NULL);
}
int main(void)
{
    struct rx3_midi_output p = {0};
    const unsigned char head[] = {0x90, 11};
    feed(&p, head, sizeof head);
    assert(count == 0); /* Socket read ending mid-message must not send it. */
    const unsigned char tail[] = {127, 11, 0, 0xb1, 2, 40, 0xf0, 0, 0x40};
    feed(&p, tail, sizeof tail);
    assert(count == 3);
    assert(lengths[0] == 3 && received[0][2] == 127);
    assert(lengths[1] == 3 && received[1][0] == 0x90 && received[1][2] == 0);
    assert(received[2][0] == 0xb1 && received[2][2] == 40);
    const unsigned char sysex_end[] = {0xf8, 5, 0xf7};
    feed(&p, sysex_end, sizeof sysex_end);
    assert(count == 5 && lengths[3] == 1 && received[3][0] == 0xf8);
    const unsigned char expected[] = {0xf0, 0, 0x40, 5, 0xf7};
    assert(lengths[4] == sizeof expected && !memcmp(received[4], expected, sizeof expected));
    const unsigned char interrupt[] = {0xf0, 1, 0x91, 12, 0};
    feed(&p, interrupt, sizeof interrupt);
    assert(count == 6 && received[5][0] == 0x91);
    rx3_midi_output_feed(&p, 0xf0, collect, NULL);
    for (unsigned i = 0; i < 1024; ++i) rx3_midi_output_feed(&p, 1, collect, NULL);
    rx3_midi_output_feed(&p, 0xf7, collect, NULL);
    assert(count == 6); /* Reject oversized SysEx, then recover cleanly. */
    const unsigned char recovery[] = {0x90, 11, 127, 0xc0, 5, 0xf2, 1, 2};
    feed(&p, recovery, sizeof recovery);
    assert(count == 9 && lengths[6] == 3 && lengths[7] == 2 && lengths[8] == 3);
    puts("MIDI output: fragmented notes, velocity-zero off, running status, CC, SysEx, realtime and overflow recovery passed");
    return 0;
}
