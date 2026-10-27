#ifndef RX3_MIDI_OUTPUT_H
#define RX3_MIDI_OUTPUT_H
#include <stddef.h>

/* Frame complete MIDI messages across arbitrary socket read boundaries. */
struct rx3_midi_output {
    unsigned char bytes[256], status;
    size_t used, needed;
    int sysex, overflow;
};
typedef void (*rx3_midi_emit)(const unsigned char *, size_t, void *);

static inline void rx3_midi_output_feed(struct rx3_midi_output *p, unsigned char b,
                                        rx3_midi_emit emit, void *context)
{
    if (b >= 0xf8) { emit(&b, 1, context); return; }
    if (p->sysex) {
        if (b == 0xf7) {
            if (!p->overflow && p->used < sizeof p->bytes) {
                p->bytes[p->used++] = b;
                emit(p->bytes, p->used, context);
            }
            p->sysex = p->overflow = 0;
            p->used = 0;
            return;
        }
        if (!(b & 0x80)) {
            if (p->used < sizeof p->bytes) p->bytes[p->used++] = b;
            else p->overflow = 1;
            return;
        }
        p->sysex = p->overflow = 0; /* interrupted SysEx: resync at status */
    }
    if (b & 0x80) {
        p->used = 0;
        p->status = b < 0xf0 ? b : 0;
        if (b == 0xf0) {
            p->sysex = 1; p->bytes[0] = b; p->used = 1;
            return;
        }
        p->needed = b < 0xf0 ? ((b & 0xe0) == 0xc0 ? 2 : 3) :
                    b == 0xf2 ? 3 : (b == 0xf1 || b == 0xf3) ? 2 : 1;
        p->bytes[p->used++] = b;
    } else {
        if (!p->used) {
            if (!p->status) return;
            p->bytes[p->used++] = p->status;
        }
        p->bytes[p->used++] = b;
    }
    if (p->used == p->needed) {
        emit(p->bytes, p->used, context);
        p->used = 0;
    }
}
#endif
