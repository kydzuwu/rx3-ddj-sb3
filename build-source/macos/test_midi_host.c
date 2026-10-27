/* A temporary, clearly named virtual MIDI source. Sends to the prototype only;
 * no MIDI output destination or physical controller is opened. */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    unsigned channel = 6, note = 66;
    if (argc == 3) {
        char *end;
        channel = (unsigned)strtoul(argv[1], &end, 10);
        if (*end || channel > 15) return 2;
        note = (unsigned)strtoul(argv[2], &end, 10);
        if (*end || note > 127) return 2;
    } else if (argc != 1) {
        fprintf(stderr, "Usage: test-midi-host [channel_0_to_15 note_0_to_127]\n");
        return 2;
    }
    MIDIClientRef client;
    MIDIEndpointRef source;
    if (MIDIClientCreate(CFSTR("RX3 Prototype Test"), NULL, NULL, &client) != noErr ||
        MIDISourceCreate(client, CFSTR("DDJ-FLX4 Prototype Test"), &source) != noErr)
        return 1;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 3.0, false);
    const Byte press[] = {0x90 | channel, note, 127}; /* default SHIFT+BROWSE */
    const Byte release[] = {0x90 | channel, note, 0};
    Byte storage[128];
    MIDIPacketList *list = (MIDIPacketList *)storage;
    MIDIPacket *packet = MIDIPacketListInit(list);
    if (!MIDIPacketListAdd(list, sizeof(storage), packet, 0, sizeof(press), press)) return 1;
    if (MIDIReceived(source, list) != noErr) return 1;
    usleep(150000);
    packet = MIDIPacketListInit(list);
    if (!MIDIPacketListAdd(list, sizeof(storage), packet, 0, sizeof(release), release)) return 1;
    if (MIDIReceived(source, list) != noErr) return 1;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false);
    MIDIEndpointDispose(source);
    MIDIClientDispose(client);
    printf("Sent channel %u note %u press/release through a temporary virtual FLX4 source.\n", channel, note);
    return 0;
}
