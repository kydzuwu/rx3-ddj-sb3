#ifndef RX3_MIDI_RAW_H
#define RX3_MIDI_RAW_H
#include <string.h>
#include <sound/asequencer.h>

struct midi_raw_parser { unsigned char status, data[2], length, needed; };

/* Running status survives realtime messages. System common/SysEx reset it;
 * unsupported channel messages are consumed without corrupting the next one. */
static inline int midi_raw_feed(struct midi_raw_parser *p, unsigned char byte,
                                struct snd_seq_event *event)
{
    if (byte >= 0xf8) return 0;
    if (byte & 0x80) {
        p->status = byte < 0xf0 ? byte : 0;
        p->needed = (byte & 0xe0) == 0xc0 ? 1 : 2;
        p->length = 0;
        return 0;
    }
    if (!p->status) return 0;
    p->data[p->length++] = byte;
    if (p->length != p->needed) return 0;
    p->length = 0;
    memset(event, 0, sizeof *event);
    if ((p->status & 0xf0) == 0x80 || (p->status & 0xf0) == 0x90) {
        event->type = (p->status & 0xf0) == 0x80 ? SNDRV_SEQ_EVENT_NOTEOFF : SNDRV_SEQ_EVENT_NOTEON;
        event->data.note.channel = p->status & 15;
        event->data.note.note = p->data[0];
        event->data.note.velocity = p->data[1];
    } else if ((p->status & 0xf0) == 0xb0) {
        event->type = SNDRV_SEQ_EVENT_CONTROLLER;
        event->data.control.channel = p->status & 15;
        event->data.control.param = p->data[0];
        event->data.control.value = p->data[1];
    } else return 0;
    return 1;
}
#endif
