/* sb3-as-sb2: shows rekordbox 5 a virtual MIDI controller named "DDJ-SB2" and translates
 * between it and a real DDJ-SB3 (sb2_map.h), so rekordbox's own DDJ-SB2 mapping drives the SB3
 * and lights its LEDs. This is a MIDI translation only: it does not touch rekordbox's licence.
 * Usage: sb3-as-sb2 [--name "DDJ-SB2"] [--verbose] | --wake. Stop with Ctrl+C. --wake only sends
 * the SB3 its LED init and exits, for rekordbox's own DDJ-SB3 mapping (rekordbox/DDJ-SB3.midi.csv). Quit the RX3 app first:
 * both talk to the SB3. */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "sb2_map.h"

static MIDIPortRef out_port;
static MIDIEndpointRef sb3_out, virtual_source;
static int verbose;

static void name_of(MIDIEndpointRef endpoint, char *buffer, size_t size) {
    CFStringRef name = NULL;
    buffer[0] = 0;
    if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) == noErr && name) {
        CFStringGetCString(name, buffer, (CFIndex)size, kCFStringEncodingUTF8);
        CFRelease(name);
    }
}

static int is_sb3(MIDIEndpointRef endpoint) {
    char name[256];
    name_of(endpoint, name, sizeof name);
    return strstr(name, "SB3") || strstr(name, "SB-3");
}

static void send_bytes(MIDIPortRef port, MIDIEndpointRef to, const unsigned char *bytes, int length, int as_source) {
    Byte buffer[512];
    MIDIPacketList *list = (MIDIPacketList *)buffer;
    MIDIPacket *packet = MIDIPacketListInit(list);
    packet = MIDIPacketListAdd(list, sizeof buffer, packet, 0, (ByteCount)length, bytes);
    if (!packet) return;
    if (as_source) { if (to) MIDIReceived(to, list); }
    else if (to) MIDISend(port, to, list);
}

/* Walks a packet's bytes as channel messages; SysEx and realtime bytes are skipped. */
typedef int (*translator)(unsigned char, unsigned char, unsigned char, unsigned char[SB2_OUT_MAX][3]);
static void each_message(const MIDIPacketList *list, translator map, int to_rekordbox) {
    const MIDIPacket *packet = &list->packet[0];
    for (UInt32 p = 0; p < list->numPackets; ++p) {
        const Byte *b = packet->data;
        for (UInt16 i = 0; i < packet->length;) {
            Byte status = b[i];
            if (status == 0xF0) { while (i < packet->length && b[i] != 0xF7) ++i; ++i; continue; }
            if (status < 0x80 || status >= 0xF0) { ++i; continue; }
            int size = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3;
            if (i + size > packet->length) break;
            unsigned char out[SB2_OUT_MAX][3];
            int n = map(status, b[i + 1], size == 3 ? b[i + 2] : 0, out);
            for (int k = 0; k < n; ++k) {
                if (verbose)
                    fprintf(stderr, "%s %02X %02X %02X -> %02X %02X %02X\n", to_rekordbox ? "SB3->rb" : "rb->SB3",
                            status, b[i + 1], size == 3 ? b[i + 2] : 0, out[k][0], out[k][1], out[k][2]);
                if (to_rekordbox) send_bytes(0, virtual_source, out[k], 3, 1);
                else send_bytes(out_port, sb3_out, out[k], 3, 0);
            }
            i += (UInt16)size;
        }
        packet = MIDIPacketNext(packet);
    }
}

static void from_sb3(const MIDIPacketList *list, void *a, void *b) {
    (void)a; (void)b;
    if (getenv("SB2_LOG_ALL")) {
        const MIDIPacket *packet = &list->packet[0];
        for (UInt32 p = 0; p < list->numPackets; ++p, packet = MIDIPacketNext(packet)) {
            if (packet->length && ((packet->data[0] & 0xE0) == 0x80 || ((packet->data[0] & 0xF0) == 0xB0 && packet->data[1] != 0x21 && packet->data[1] != 0x22 && packet->data[1] != 0x23))) {
                fprintf(stderr, "SB3 ->");
                for (UInt16 i = 0; i < packet->length && i < 24; ++i) fprintf(stderr, " %02X", packet->data[i]);
                fprintf(stderr, "\n");
            }
        }
    }
    each_message(list, sb3_to_sb2, 1);
}

/* The first packets rekordbox sends are always logged, and a count every 10 s, so the log
 * shows whether rekordbox lights the virtual SB2 at all. */
static volatile unsigned long rb_packets, rb_logged;
static void from_rekordbox(const MIDIPacketList *list, void *a, void *b) {
    (void)a; (void)b;
    const MIDIPacket *packet = &list->packet[0];
    for (UInt32 p = 0; p < list->numPackets; ++p) {
        ++rb_packets;
        int meter = packet->length == 3 && (packet->data[0] & 0xF0) == 0xB0 && packet->data[1] == 0x02;
        if (getenv("SB2_LOG_ALL") ? !meter : rb_logged < 40) {
            ++rb_logged;
            fprintf(stderr, "rekordbox ->");
            for (UInt16 i = 0; i < packet->length && i < 24; ++i) fprintf(stderr, " %02X", packet->data[i]);
            fprintf(stderr, packet->length > 24 ? " ...\n" : "\n");
        }
        packet = MIDIPacketNext(packet);
    }
    each_message(list, sb2_to_sb3, 0);
}

static volatile sig_atomic_t running = 1;
static void stop(int signal_number) { (void)signal_number; running = 0; }

int main(int argc, char **argv) {
    const char *name = "DDJ-SB2";
    int wake = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--wake")) wake = 1;
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else { fprintf(stderr, "Usage: sb3-as-sb2 [--name NAME] [--verbose]\n"); return 2; }
    }
    MIDIClientRef client;
    if (MIDIClientCreate(CFSTR("SB3 as SB2"), NULL, NULL, &client) != noErr) {
        fprintf(stderr, "Could not open CoreMIDI\n"); return 1;
    }
    if (wake) {
        static const unsigned char init[] = {0xF0, 0x00, 0x20, 0x7F, 0x03, 0x01, 0xF7};
        if (MIDIOutputPortCreate(client, CFSTR("SB3 out"), &out_port) != noErr) return 1;
        for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i)
            if (is_sb3(MIDIGetDestination(i))) sb3_out = MIDIGetDestination(i);
        if (!sb3_out) { fprintf(stderr, "No DDJ-SB3 found\n"); return 1; }
        send_bytes(out_port, sb3_out, init, sizeof init, 0);
        usleep(200000);
        fprintf(stderr, "DDJ-SB3 lights woken\n");
        return 0;
    }
    CFStringRef cfname = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    MIDIEndpointRef virtual_destination = 0;
    MIDIPortRef in_port;
    if (MIDIInputPortCreate(client, CFSTR("SB3 in"), from_sb3, NULL, &in_port) != noErr ||
        MIDIOutputPortCreate(client, CFSTR("SB3 out"), &out_port) != noErr) {
        fprintf(stderr, "Could not open the MIDI ports\n"); return 1;
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    fprintf(stderr, "Waiting for the DDJ-SB3... (Ctrl+C to stop)\n");
    MIDIEndpointRef sb3_in = 0;
    while (running) {
        MIDIEndpointRef found_in = 0, found_out = 0;
        for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i)
            if (is_sb3(MIDIGetSource(i))) found_in = MIDIGetSource(i);
        for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i)
            if (is_sb3(MIDIGetDestination(i))) found_out = MIDIGetDestination(i);
        /* The virtual DDJ-SB2 exists only while a real SB3 is plugged in, so rekordbox does not
         * pick it over another controller (e.g. a DDJ-400) when no SB3 is there. */
        int want_virtual = found_in || found_out;
        if (want_virtual && !virtual_source) {
            if (MIDISourceCreate(client, cfname, &virtual_source) != noErr ||
                MIDIDestinationCreate(client, cfname, from_rekordbox, NULL, &virtual_destination) != noErr) {
                fprintf(stderr, "Could not create the virtual %s\n", name); return 1;
            }
            fprintf(stderr, "Virtual \"%s\" created\n", name);
        } else if (!want_virtual && virtual_source) {
            MIDIEndpointDispose(virtual_source);
            MIDIEndpointDispose(virtual_destination);
            virtual_source = virtual_destination = 0;
            fprintf(stderr, "Virtual \"%s\" removed\n", name);
        }
        if (found_in != sb3_in) {
            if (sb3_in) MIDIPortDisconnectSource(in_port, sb3_in);
            sb3_in = found_in && MIDIPortConnectSource(in_port, found_in, NULL) == noErr ? found_in : 0;
            fprintf(stderr, sb3_in ? "DDJ-SB3 connected\n" : "DDJ-SB3 disconnected\n");
        }
        if (found_out != sb3_out) {
            sb3_out = found_out;
            if (sb3_out) {
                /* The SB3 keeps its LEDs dark until this init (from the Mixxx DDJ-SB3 script). */
                static const unsigned char init[] = {0xF0, 0x00, 0x20, 0x7F, 0x03, 0x01, 0xF7};
                send_bytes(out_port, sb3_out, init, sizeof init, 0);
            }
        }
        static int ticks;
        static unsigned long last_count;
        if (++ticks % 20 == 0 && rb_packets != last_count) {
            last_count = rb_packets;
            fprintf(stderr, "rekordbox packets so far: %lu\n", last_count);
        }
        /* Run the run loop, not sleep: CoreMIDI only refreshes the device list (an SB3 plugged in or
         * enumerated after login) through it. */
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
    }
    if (virtual_source) MIDIEndpointDispose(virtual_source);
    if (virtual_destination) MIDIEndpointDispose(virtual_destination);
    MIDIClientDispose(client);
    fprintf(stderr, "Stopped\n");
    return 0;
}
