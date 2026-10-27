/* Observe the real guest LED stream through a temporary CoreMIDI destination.
 * Never sends controller input or writes to a physical hardware endpoint. */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "midi_output.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned notes, off, on, keepalive;
static struct rx3_midi_output parser;
static void count_message(const unsigned char *bytes, size_t length, void *unused)
{
    (void)unused;
    const Byte expected[] = {0xf0,0,0x40,5,0,0,4,5,0,0x50,2,0xf7};
    if (length == 3 && (bytes[0] & 0xf0) == 0x90) {
        ++notes;
        if (bytes[2]) ++on; else ++off;
    }
    if (length == sizeof expected && !memcmp(bytes, expected, sizeof expected)) ++keepalive;
}
static void observe(const MIDIPacketList *list, void *unused, void *source)
{
    (void)unused; (void)source;
    const MIDIPacket *p = &list->packet[0];
    pthread_mutex_lock(&lock);
    for (UInt32 i = 0; i < list->numPackets; ++i) {
        for (UInt16 k = 0; k < p->length; ++k)
            rx3_midi_output_feed(&parser, p->data[k], count_message, NULL);
        p = MIDIPacketNext(p);
    }
    pthread_mutex_unlock(&lock);
}
int main(void)
{
    MIDIClientRef client;
    MIDIEndpointRef endpoint;
    if (MIDIClientCreate(CFSTR("RX3 Feedback Validation"), NULL, NULL, &client) != noErr) return 1;
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i) {
        CFStringRef name = NULL;
        char text[512] = {0};
        if (MIDIObjectGetStringProperty(MIDIGetDestination(i), kMIDIPropertyDisplayName, &name) == noErr && name) {
            CFStringGetCString(name, text, sizeof text, kCFStringEncodingUTF8);
            CFRelease(name);
        }
        if (strstr(text, "FLX4") || strstr(text, "FLX-4")) {
            puts("A physical FLX4 output is available; skipped virtual capture to keep its lights live.");
            MIDIClientDispose(client);
            return 0;
        }
    }
    if (MIDIDestinationCreate(client, CFSTR("DDJ-FLX4 Feedback Test"), observe, NULL, &endpoint) != noErr) return 1;
    for (unsigned i = 0; i < 40; ++i) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
    MIDIEndpointDispose(endpoint);
    MIDIClientDispose(client);
    pthread_mutex_lock(&lock);
    printf("Guest -> CoreMIDI feedback: %u notes (%u on, %u off), %u FLX4 keepalives\n", notes,on,off,keepalive);
    int passed = notes && on && off && keepalive;
    pthread_mutex_unlock(&lock);
    return passed ? 0 : 1;
}
