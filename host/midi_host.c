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
#include <time.h>
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
static int debug;
/* SB3 pads in FX FADE / PAD SCRATCH mode make the unit itself emit fake
 * filter, channel fader and jog messages (captured on hardware 2026-10-27).
 * Those must not reach the guest as real mixer moves. Per deck: pads held,
 * and the time the last one was released. */
static int fade_held[2];
static unsigned long long fade_until[2];
static volatile int sb3_resync;
#define FADE_TAIL_MS 1500

/* Deck state read back from the guest's LEDs (guest->SB3). rbp blinks a LED by
 * re-sending it on/off, so "toggled within BLINK_WINDOW_MS" = blinking.
 * PLAY lit and not blinking = playing; LOOP IN blinking = loop active. */
#define BLINK_WINDOW_MS 1500
#define BLINK_HALF_MS 300
static volatile int play_led[2], loop_led[2], fx_led, keylock_led[2], cue_led[2];
static volatile unsigned long long play_toggle_ms[2], loop_toggle_ms[2], loop_prev_ms[2], fx_toggle_ms, fx_prev_ms;
static volatile int fx_deck_mask = 1;  /* Beat FX target from the last FX1 press: bit 0 deck 1, bit 1 deck 2 */
static int inout_call[2][2];           /* IN/OUT pressed while stopped -> CALL </> until released */

static unsigned long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + (unsigned long long)t.tv_nsec / 1000000;
}

/* Returns 1 if this SB3 message is a phantom produced by an FX FADE /
 * PAD SCRATCH pad and must be dropped. Updates the pad-held state. */
static void led_seen(volatile int *state, volatile unsigned long long *toggled, int on) {
    if (*state != on) { *state = on; *toggled = now_ms(); }
}

static int deck_playing(int deck) {
    return play_led[deck] && now_ms() - play_toggle_ms[deck] > BLINK_WINDOW_MS;
}

static int sb3_phantom(unsigned char status, unsigned char d1, unsigned char d2) {
    unsigned char kind = status & 0xf0, ch = status & 0x0f;
    if (kind == 0x80) kind = 0x90, d2 = 0;
    if (kind == 0x90 && (ch == 7 || ch == 8) && d1 >= 16 && d1 < 48) {
        int deck = ch - 7;
        if (d2) ++fade_held[deck];
        else if (fade_held[deck] > 0 && --fade_held[deck] == 0) {
            fade_until[deck] = now_ms() + FADE_TAIL_MS;
            sb3_resync = 1;
        }
        return 0;
    }
    int deck = -1;
    if (ch <= 1) {
        if ((kind == 0xb0 && (d1 == 19 || d1 == 51 || d1 == 33 || d1 == 34 ||
                              d1 == 35 || d1 == 31)) ||
            (kind == 0x90 && d1 == 54))
            deck = ch;
    } else if (ch == 6 && kind == 0xb0) {
        if (d1 == 23 || d1 == 55) deck = 0;
        else if (d1 == 24 || d1 == 56) deck = 1;
    }
    if (deck < 0) return 0;
    return fade_held[deck] > 0 || now_ms() < fade_until[deck];
}

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
            unsigned char mapped[SB3_OUT_MAX][3];
            unsigned char kind_in = sb3_status & 0xf0, ch_in = sb3_status & 0x0f;
            /* IN / OUT on a stopped deck step through memory cues (CALL < / >). */
            if ((kind_in == 0x90 || kind_in == 0x80) && ch_in <= 1 &&
                (sb3_data[0] == 18 || sb3_data[0] == 19)) {
                int which = sb3_data[0] - 18;
                if (kind_in == 0x90 && sb3_data[1]) inout_call[ch_in][which] = !deck_playing(ch_in);
                if (inout_call[ch_in][which]) sb3_data[0] = which ? 98 : 97;
            }
            if (kind_in == 0x90 && (ch_in == 4 || ch_in == 5) && sb3_data[1]) {
                if (sb3_data[0] == 71) fx_deck_mask = ch_in == 4 ? 1 : 2;
                else if (sb3_data[0] == 99) fx_deck_mask = 3;
            }
            int phantom = sb3_phantom(sb3_status, sb3_data[0], sb3_data[1]);
            int count = phantom ? 0 : sb3_translate(sb3_status, sb3_data[0], sb3_data[1], mapped);
            for (int m = 0; m < count; ++m) forward_bytes(mapped[m], 3);
            if (debug) {
                if (count) {
                    fprintf(stderr, "SB3->guest %02X %02X %02X ->", sb3_status, sb3_data[0], sb3_data[1]);
                    for (int m = 0; m < count; ++m)
                        fprintf(stderr, " %02X %02X %02X", mapped[m][0], mapped[m][1], mapped[m][2]);
                    fputc('\n', stderr);
                } else
                    fprintf(stderr, "SB3->guest %02X %02X %02X dropped%s\n",
                            sb3_status, sb3_data[0], sb3_data[1], phantom ? " (pad FX phantom)" : "");
            }
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

static void send_raw(const unsigned char *bytes, size_t length) {
    union { MIDIPacketList aligned; Byte storage[512]; } buffer;
    MIDIPacketList *list = &buffer.aligned;
    MIDIPacket *packet = MIDIPacketListInit(list);
    if (!MIDIPacketListAdd(list, sizeof buffer, packet, 0, length, bytes)) return;
    pthread_mutex_lock(&output_lock);
    if (destination) MIDISend(output_port, destination, list);
    pthread_mutex_unlock(&output_lock);
}

static void send_output(const unsigned char *bytes, size_t length, void *context) {
    (void)context;
    unsigned char mapped[3];
    if (sb3_mode) {
        /* The FLX4's keepalive and unverified lights must never reach the SB3.
         * These output rows are documented by the SB3 MIDI list. */
        if (length != 3) return;
        unsigned char kind = bytes[0] & 0xf0, ch = bytes[0] & 0x0f;
        unsigned char note = bytes[1], value = bytes[2];
        if (debug)
            fprintf(stderr, "guest->SB3 %02X %02X %02X\n", bytes[0], bytes[1], bytes[2]);
        if (kind == 0x80) kind = 0x90, value = 0; /* note-off turns LED off */
        if (kind == 0x90) {
            if (ch <= 1 && note == 11) led_seen(&play_led[ch], &play_toggle_ms[ch], value != 0);
            if (ch <= 1 && note == 16 && loop_led[ch] != (value != 0)) {
                loop_prev_ms[ch] = loop_toggle_ms[ch];   /* two toggles = blinking, one = track load */
                led_seen(&loop_led[ch], &loop_toggle_ms[ch], value != 0);
            }
            if (ch == 4 && note == 71) {
                /* The guest blinks this LED while Beat FX is on; steady lit = off. */
                if (fx_led != (value != 0)) fx_prev_ms = fx_toggle_ms;
                led_seen(&fx_led, &fx_toggle_ms, value != 0);
                return;
            }
            if (ch <= 1 && note == 26) { keylock_led[ch] = value != 0; return; } /* blinked by blink_leds() */
            if (ch <= 1 && note == 84) { cue_led[ch] = value != 0; return; }     /* CH CUE: blinked too */
            if (ch <= 1) {
                if (note == 16) note = 18;      /* LOOP IN LED -> IN button */
                else if (note == 17) note = 19; /* LOOP OUT LED -> OUT button */
                else if (note == 77) return;    /* AUTO LOOP LED is driven by blink_leds() */
                else if (note != 11 && note != 12 && note != 88 && note != 92 &&
                    note != 84 &&
                    note != 27 && note != 30 && note != 32 && note != 34) return;
            } else if (ch == 6 && note == 99) note = 91;
            else if (ch == 4 || ch == 5) return; /* FX1 LED is driven by blink_leds() */
            else if (ch == 7 || ch == 9) {
                if (!(note < 8 || (note >= 16 && note < 24) ||
                      (note >= 32 && note < 40) ||
                      (note >= 48 && note < 56))) return;
                ch = ch == 9 ? 8 : 7;
            } else return;
            mapped[0] = 0x90 | ch; mapped[1] = note; mapped[2] = value;
        } else if (kind == 0xb0 && note == 2 && ch <= 1) {
            /* CH Level Indicator: SB3 list 3-12, MIDI-OUT Bn 02, 0-127. */
            mapped[0] = 0xb0 | ch; mapped[1] = 2; mapped[2] = value;
        } else return;
        bytes = mapped;
    }
    /* Include packet-list headers and natural alignment, including SysEx. */
    union { MIDIPacketList aligned; Byte storage[512]; } buffer;
    MIDIPacketList *list = &buffer.aligned;
    MIDIPacket *packet = MIDIPacketListInit(list);
    if (!MIDIPacketListAdd(list, sizeof buffer, packet, 0, length, bytes)) return;
    if (++guest_messages == 1)
        fprintf(stderr, "Guest MIDI feedback stream active (%zu-byte message)\n", length);
    pthread_mutex_lock(&output_lock);
    if (destination) {
        OSStatus result = MIDISend(output_port, destination, list);
        if (result == noErr) {
            ++output_messages;
            if (output_messages == 1 || output_messages % 4096 == 0)
                fprintf(stderr, "%s LED feedback: %lu messages sent, %lu errors\n",
                        sb3_mode ? "SB3" : "FLX4", output_messages, output_errors);
        } else {
            ++output_errors;
            if (output_errors == 1 || output_errors % 4096 == 0)
                fprintf(stderr, "Controller MIDI output error: %d\n", (int)result);
        }
    }
    pthread_mutex_unlock(&output_lock);
}

/* LEDs the guest has no SB3 equivalent for, blinked here at ~1.7 Hz:
 * AUTO LOOP while that deck's loop is active, FX1 on the Beat FX target deck(s)
 * while Beat FX is on, KEY LOCK while master tempo is on. Re-sent every 2 s
 * like the guest's own LEDs. */
static void *blink_leds(void *context) {
    (void)context;
    int last[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    for (unsigned tick = 0;; ++tick) {
        usleep(BLINK_HALF_MS * 1000);
        if (!sb3_mode || !destination) { for (int k = 0; k < 8; ++k) last[k] = -1; continue; }
        unsigned long long t = now_ms();
        int phase = tick & 1, force = tick % 7 == 0;
        int fx_on = fx_prev_ms && t - fx_toggle_ms < BLINK_WINDOW_MS &&
                    fx_toggle_ms - fx_prev_ms < BLINK_WINDOW_MS;
        for (int deck = 0; deck < 2; ++deck) {
            int loop = loop_prev_ms[deck] && t - loop_toggle_ms[deck] < BLINK_WINDOW_MS &&
                       loop_toggle_ms[deck] - loop_prev_ms[deck] < BLINK_WINDOW_MS;
            int want[4] = { loop && phase, fx_on && (fx_deck_mask & (1 << deck)) && phase,
                            keylock_led[deck] && phase, cue_led[deck] && phase };
            static const unsigned char status[4] = { 0x90, 0x94, 0x90, 0x90 }, note[4] = { 20, 71, 26, 84 };
            for (int k = 0; k < 4; ++k) {
                int *l = &last[deck * 4 + k];
                if (want[k] == *l && !force) continue;
                unsigned char m[3] = { (unsigned char)(status[k] | deck), note[k], want[k] ? 0x7f : 0 };
                send_raw(m, 3);
                *l = want[k];
            }
        }
    }
    return NULL;
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
    debug = getenv("RX3_MIDI_DEBUG") != NULL;
    if (!listing && argc != 2) {
        fprintf(stderr, "Usage: midi-host --list | /path/to/midi.sock\n"); return 2;
    }
    MIDIClientRef client = 0;
    OSStatus midi_status = noErr;
    for (int attempt = 0; attempt < 30; ++attempt) {
        midi_status = MIDIClientCreate(CFSTR("RX3 Mac Prototype"), NULL, NULL, &client);
        if (midi_status == noErr) break;
        if (attempt == 0)
            fprintf(stderr, "CoreMIDI is starting (status %d); retrying...\n", (int)midi_status);
        sleep(1);
    }
    if (midi_status != noErr) {
        fprintf(stderr, "Could not open CoreMIDI (status %d)\n", (int)midi_status);
        return 1;
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
    pthread_t blinker;
    if (pthread_create(&blinker, NULL, blink_leds, NULL)) return 1;
    pthread_detach(blinker);
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
            if (sb3_mode) {
                if (strstr(name, "SB3") || strstr(name, "SB-3")) { found_output = endpoint; break; }
            } else if (strstr(name, "FLX4") || strstr(name, "FLX-4")) {
                found_output = endpoint; break;
            }
        }
        pthread_mutex_lock(&output_lock);
        int sb3_connected = 0;
        if (found_output != destination) {
            destination = found_output;
            sb3_connected = destination && sb3_mode;
            fprintf(stderr, "%s\n", destination ?
                    (sb3_mode ? "SB3 MIDI output connected" : "FLX4 MIDI output connected") :
                    "Controller MIDI output unavailable");
        }
        pthread_mutex_unlock(&output_lock);
        if (sb3_connected) {
            /* The SB3 ignores host LED messages until it receives this SysEx
             * (Mixxx's DDJ-SB3 mapping init). 9B 09 7F then asks it to report
             * every knob/fader position so the guest starts in sync. */
            static const unsigned char init[] = {0xf0, 0x00, 0x20, 0x7f, 0x03, 0x01, 0xf7};
            static const unsigned char dump[] = {0x9b, 0x09, 0x7f};
            send_raw(init, sizeof init);
            send_raw(dump, sizeof dump);
            fprintf(stderr, "SB3 init SysEx and position request sent\n");
        }
        if (sb3_mode && sb3_resync) {
            unsigned long long t = now_ms();
            pthread_mutex_lock(&lock);
            int quiet = !fade_held[0] && !fade_held[1] &&
                        t >= fade_until[0] && t >= fade_until[1];
            if (quiet) sb3_resync = 0;
            pthread_mutex_unlock(&lock);
            if (quiet) {
                /* Pad FX moved the guest's view of nothing (phantoms were
                 * dropped), but re-read real positions to be sure. */
                static const unsigned char dump[] = {0x9b, 0x09, 0x7f};
                send_raw(dump, sizeof dump);
                if (debug) fprintf(stderr, "SB3 position resync after pad FX\n");
            }
        }
        service_notifications();
    }
}
