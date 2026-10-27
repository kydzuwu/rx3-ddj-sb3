#include <assert.h>
#include <stdio.h>
#include "../scripts/shims/midi_raw.h"

int main(void)
{
    struct midi_raw_parser p = {0};
    struct snd_seq_event e;
    unsigned char input[] = {0x96, 66, 0xf8, 127, 66, 0, 0xf0, 1, 2, 0xf7,
                             0xb1, 31, 64, 31, 0, 0xc0, 4, 0x81, 60, 2};
    unsigned events = 0;
    for (unsigned i = 0; i < sizeof input; ++i) {
        if (!midi_raw_feed(&p, input[i], &e)) continue;
        switch (events++) {
        case 0: assert(e.type == SNDRV_SEQ_EVENT_NOTEON && e.data.note.channel == 6 &&
                       e.data.note.note == 66 && e.data.note.velocity == 127); break;
        case 1: assert(e.type == SNDRV_SEQ_EVENT_NOTEON && e.data.note.velocity == 0); break;
        case 2: assert(e.type == SNDRV_SEQ_EVENT_CONTROLLER && e.data.control.channel == 1 &&
                       e.data.control.param == 31 && e.data.control.value == 64); break;
        case 3: assert(e.type == SNDRV_SEQ_EVENT_CONTROLLER && e.data.control.value == 0); break;
        case 4: assert(e.type == SNDRV_SEQ_EVENT_NOTEOFF && e.data.note.channel == 1 &&
                       e.data.note.note == 60 && e.data.note.velocity == 2); break;
        default: assert(0);
        }
    }
    assert(events == 5);
    puts("Raw MIDI: notes, CCs, running status, realtime and SysEx boundaries passed");
    return 0;
}
