/* ARM32 guest MIDI input bridge. Raw bytes arrive over a virtio serial port.
 * Creates a named ALSA sequencer source for the existing FLX4 mapping. */
#include <errno.h>
#include <fcntl.h>
#include <sound/asequencer.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int sequencer, client, port;

static int setup(void) {
    sequencer = open("/dev/snd/seq", O_RDWR);
    if (sequencer < 0 || ioctl(sequencer, SNDRV_SEQ_IOCTL_CLIENT_ID, &client) < 0)
        return -1;
    struct snd_seq_client_info info = {0};
    info.client = client;
    info.type = USER_CLIENT;
    snprintf(info.name, sizeof(info.name), "DDJ-FLX4 macOS bridge");
    if (ioctl(sequencer, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &info) < 0) return -1;
    struct snd_seq_port_info surface = {0};
    surface.addr.client = client;
    snprintf(surface.name, sizeof(surface.name), "DDJ-FLX4 MIDI 1");
    surface.capability = SNDRV_SEQ_PORT_CAP_READ | SNDRV_SEQ_PORT_CAP_SUBS_READ;
    surface.type = SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC | SNDRV_SEQ_PORT_TYPE_APPLICATION;
    if (ioctl(sequencer, SNDRV_SEQ_IOCTL_CREATE_PORT, &surface) < 0) return -1;
    port = surface.addr.port;
    return 0;
}

static void dispatch(unsigned char status, unsigned char *data) {
    struct snd_seq_event event = {0};
    event.queue = SNDRV_SEQ_QUEUE_DIRECT;
    event.source.client = client;
    event.source.port = port;
    event.dest.client = SNDRV_SEQ_ADDRESS_SUBSCRIBERS;
    event.dest.port = 0;
    if ((status & 0xf0) == 0x80 || (status & 0xf0) == 0x90) {
        event.type = (status & 0xf0) == 0x80 ? SNDRV_SEQ_EVENT_NOTEOFF : SNDRV_SEQ_EVENT_NOTEON;
        event.data.note.channel = status & 15;
        event.data.note.note = data[0];
        event.data.note.velocity = data[1];
    } else if ((status & 0xf0) == 0xb0) {
        event.type = SNDRV_SEQ_EVENT_CONTROLLER;
        event.data.control.channel = status & 15;
        event.data.control.param = data[0];
        event.data.control.value = data[1];
    } else {
        return;
    }
    if (write(sequencer, &event, sizeof(event)) != sizeof(event))
        perror("FLX4 event dispatch");
}

int main(int argc, char **argv) {
    const char *device = argc > 1 ? argv[1] : "/dev/vport0p1";
    int input;
    while ((input = open(device, O_RDONLY)) < 0) sleep(1);
    if (setup() < 0) { perror("FLX4 ALSA setup"); return 1; }
    fprintf(stderr, "FLX4 macOS bridge ready at %d:%d\n", client, port);
    unsigned char status = 0, data[2], byte;
    int length = 0, needed = 0;
    for (;;) {
        ssize_t n = read(input, &byte, 1);
        if (n <= 0) { if (n < 0 && errno != EINTR) perror("FLX4 input"); sleep(1); continue; }
        if (byte >= 0xf8) continue; /* realtime bytes may interrupt any message */
        if (byte & 0x80) {
            status = byte < 0xf0 ? byte : 0;
            needed = (byte & 0xe0) == 0xc0 ? 1 : 2;
            length = 0;
        } else if (status) {
            data[length++] = byte;
            if (length == needed) { dispatch(status, data); length = 0; }
        }
    }
}
