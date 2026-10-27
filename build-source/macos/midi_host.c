/* Bidirectional macOS CoreMIDI <-> local QEMU virtio serial. */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "midi_output.h"
#include "sb3_map.h"

static int connection = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
static MIDIPortRef output_port;
static MIDIEndpointRef destination;
static unsigned long output_messages, output_errors;
static unsigned long guest_messages;
static volatile int sb3_mode;
static unsigned char sb3_status, sb3_data[2];
static int sb3_length;

static void forward_bytes(const unsigned char *bytes, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t n = write(connection, bytes + offset, length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { perror("MIDI guest connection"); exit(1); }
        offset += (size_t)n;
    }
}

static void translate_sb3(const unsigned char *bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        unsigned char b = bytes[i];
        if (b >= 0xf8) continue;
        if (b & 0x80) {
            if (b < 0xf0) sb3_status = b;
            else sb3_status = 0;
            sb3_length = 0;
            continue;
        }
        if (!sb3_status) continue;
        unsigned char kind = sb3_status & 0xf0;
        if (kind != 0x80 && kind != 0x90 && kind != 0xb0) continue;
        sb3_data[sb3_length++] = b;
        if (sb3_length == 2) {
            unsigned char mapped[3];
            if (sb3_translate(sb3_status, sb3_data[0], sb3_data[1], mapped))
                forward_bytes(mapped, sizeof mapped);
            sb3_length = 0;
        }
    }
}

static void service_notifications(void) {
    if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false) == kCFRunLoopRunFinished)
        usleep(100000);
}

static void display_name(MIDIObjectRef object, char *out, size_t capacity) {
    CFStringRef name = NULL;
    if (MIDIObjectGetStringProperty(object, kMIDIPropertyDisplayName, &name) == noErr && name) {
        CFStringGetCString(name, out, capacity, kCFStringEncodingUTF8);
        CFRelease(name);
    } else snprintf(out, capacity, "Unnamed MIDI source");
}

static void receive(const MIDIPacketList *list, void *context, void *source) {
    (void)context; (void)source;
    const MIDIPacket *packet = &list->packet[0];
    pthread_mutex_lock(&lock);
    for (UInt32 i = 0; i < list->numPackets; ++i) {
        if (sb3_mode) translate_sb3(packet->data, packet->length);
        else forward_bytes(packet->data, packet->length);
        packet = MIDIPacketNext(packet);
    }
    pthread_mutex_unlock(&lock);
}

static void send_output(const unsigned char *bytes, size_t length, void *context) {
    (void)context;
    /* FLX4 feedback includes a keepalive SysEx; never send it to an SB3. */
    if (sb3_mode) return;
    /* Include packet-list headers and natural alignment, including SysEx. */
    union { MIDIPacketList aligned; Byte storage[512]; } buffer;
    MIDIPacketList *list = &buffer.aligned;
    MIDIPacket *packet = MIDIPacketListInit(list);
    if (!MIDIPacketListAdd(list, sizeof buffer, packet, 0, length, bytes)) return;
    if (++guest_messages == 1)
        fprintf(stderr, "FLX4 guest feedback stream active (%zu-byte MIDI message)\n", length);
    pthread_mutex_lock(&output_lock);
    if (destination) {
        OSStatus result = MIDISend(output_port, destination, list);
        if (result == noErr) {
            ++output_messages;
            if (output_messages == 1 || output_messages % 4096 == 0)
                fprintf(stderr, "FLX4 LED feedback: %lu messages sent, %lu errors\n", output_messages, output_errors);
        } else {
            ++output_errors;
            if (output_errors == 1 || output_errors % 4096 == 0)
                fprintf(stderr, "FLX4 MIDI output error: %d\n", (int)result);
        }
    }
    pthread_mutex_unlock(&output_lock);
}

static void *read_output(void *context) {
    (void)context;
    struct rx3_midi_output parser = {0};
    unsigned char bytes[1024];
    for (;;) {
        ssize_t count = read(connection, bytes, sizeof bytes);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            fprintf(stderr, "FLX4 guest output connection closed\n");
            exit(count < 0 ? 1 : 0);
        }
        for (ssize_t i = 0; i < count; ++i)
            rx3_midi_output_feed(&parser, bytes[i], send_output, NULL);
    }
}

int main(int argc, char **argv) {
    int listing = argc == 2 && strcmp(argv[1], "--list") == 0;
    if (!listing && argc != 2) {
        fprintf(stderr, "Usage: midi-host --list | /path/to/midi.sock\n"); return 2;
    }
    MIDIClientRef client;
    if (MIDIClientCreate(CFSTR("RX3 Mac Prototype"), NULL, NULL, &client) != noErr) {
        fprintf(stderr, "Could not open CoreMIDI\n"); return 1;
    }
    if (listing) {
        for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
            char name[512]; display_name(MIDIGetSource(i), name, sizeof(name)); printf("Input: %s\n", name);
        }
        for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i) {
            char name[512]; display_name(MIDIGetDestination(i), name, sizeof(name)); printf("Output: %s\n", name);
        }
        MIDIClientDispose(client); return 0;
    }
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    if (strlen(argv[1]) >= sizeof(address.sun_path)) {
        fprintf(stderr, "Guest socket path too long\n"); return 2;
    }
    strcpy(address.sun_path, argv[1]);
    connection = socket(AF_UNIX, SOCK_STREAM, 0);
    int suppress = 1;
    setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &suppress, sizeof(suppress));
    while (connect(connection, (struct sockaddr *)&address, sizeof(address)) < 0) {
        if (errno != ENOENT && errno != ECONNREFUSED) { perror("Guest socket"); return 1; }
        service_notifications();
    }
    MIDIPortRef input;
    if (MIDIInputPortCreate(client, CFSTR("RX3 Controller Input"), receive, NULL, &input) != noErr) return 1;
    if (MIDIOutputPortCreate(client, CFSTR("RX3 Controller Output"), &output_port) != noErr) return 1;
    pthread_t reader;
    if (pthread_create(&reader, NULL, read_output, NULL)) return 1;
    pthread_detach(reader);
    MIDIEndpointRef connected = 0;
    fprintf(stderr, "Waiting for DDJ-SB3 or DDJ-FLX4 MIDI source...\n");
    for (;;) {
        MIDIEndpointRef found = 0;
        int found_sb3 = 0;
        for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
            MIDIEndpointRef endpoint = MIDIGetSource(i);
            char name[512]; display_name(endpoint, name, sizeof(name));
            if (strstr(name, "SB3") || strstr(name, "SB-3")) {
                found = endpoint; found_sb3 = 1; break;
            }
            if (!found && (strstr(name, "FLX4") || strstr(name, "FLX-4"))) found = endpoint;
        }
        if (found != connected || (found && sb3_mode != found_sb3)) {
            if (connected) MIDIPortDisconnectSource(input, connected);
            connected = 0;
            sb3_mode = found_sb3;
            sb3_status = 0;
            sb3_length = 0;
            if (found && MIDIPortConnectSource(input, found, NULL) == noErr) {
                connected = found;
                fprintf(stderr, "%s MIDI input connected\n", sb3_mode ? "SB3" : "FLX4");
            }
        }
        MIDIEndpointRef found_output = 0;
        for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i) {
            MIDIEndpointRef endpoint = MIDIGetDestination(i);
            char name[512]; display_name(endpoint, name, sizeof(name));
            if (strstr(name, "FLX4") || strstr(name, "FLX-4")) { found_output = endpoint; break; }
        }
        pthread_mutex_lock(&output_lock);
        if (found_output != destination) {
            destination = found_output;
            fprintf(stderr, "%s\n", destination ? "FLX4 MIDI output connected" : "FLX4 MIDI output unavailable (SB3 LEDs disabled)");
        }
        pthread_mutex_unlock(&output_lock);
        service_notifications();
    }
}
