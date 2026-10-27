#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <string.h>

static void receive(const MIDIPacketList *list, void *context, void *source) {
    (void)context; (void)source;
    const MIDIPacket *packet = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) {
        for (UInt16 j = 0; j + 2 < packet->length; j += 3) {
            const Byte *b = packet->data + j;
            if ((b[0] == 0xb6 && (b[1] == 0x40 || b[1] == 0x64)) ||
                (b[0] == 0x96 && (b[1] == 0x41 || b[1] == 0x42))) {
                printf("browse %02X %02X %02X\n", b[0], b[1], b[2]);
                fflush(stdout);
            }
        }
        packet = MIDIPacketNext(packet);
    }
}

int main(void) {
    MIDIClientRef client;
    MIDIPortRef port;
    if (MIDIClientCreate(CFSTR("RX3 SB3 Diagnostic"), NULL, NULL, &client) != noErr)
        return 1;
    if (MIDIInputPortCreate(client, CFSTR("SB3 Monitor"), receive, NULL, &port) != noErr)
        return 1;
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
        MIDIEndpointRef endpoint = MIDIGetSource(i);
        CFStringRef name = NULL;
        if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) != noErr || !name)
            continue;
        char label[256] = {0};
        CFStringGetCString(name, label, sizeof label, kCFStringEncodingUTF8);
        CFRelease(name);
        if (strstr(label, "SB3")) {
            printf("Monitoring %s\n", label);
            fflush(stdout);
            if (MIDIPortConnectSource(port, endpoint, NULL) != noErr) return 1;
            for (;;) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
        }
    }
    puts("SB3 source not found");
    return 2;
}
