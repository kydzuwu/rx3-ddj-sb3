/* Print every MIDI message from the DDJ-SB3. Build:
 * clang monitor_all.c -framework CoreMIDI -framework CoreFoundation -o monitor_all */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <string.h>
static void receive(const MIDIPacketList *list, void *c, void *s) {
    (void)c; (void)s;
    const MIDIPacket *p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) {
        for (UInt16 j = 0; j < p->length; ++j) {
            if (p->data[j] >= 0xf8) continue;
            printf("%s%02X", (p->data[j] & 0x80) && j ? "\n" : " ", p->data[j]);
        }
        printf("\n"); fflush(stdout);
        p = MIDIPacketNext(p);
    }
}
int main(void) {
    MIDIClientRef client; MIDIPortRef port;
    if (MIDIClientCreate(CFSTR("SB3 Monitor All"), NULL, NULL, &client)) return 1;
    if (MIDIInputPortCreate(client, CFSTR("in"), receive, NULL, &port)) return 1;
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
        MIDIEndpointRef e = MIDIGetSource(i); CFStringRef n = NULL; char l[256] = {0};
        if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &n) || !n) continue;
        CFStringGetCString(n, l, sizeof l, kCFStringEncodingUTF8); CFRelease(n);
        if (strstr(l, "SB3") || strstr(l, "SB-3")) {
            printf("Monitoring %s - press SYNC on each deck, Ctrl+C to stop\n", l); fflush(stdout);
            MIDIPortConnectSource(port, e, NULL);
            for (;;) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
        }
    }
    puts("SB3 source not found"); return 2;
}
