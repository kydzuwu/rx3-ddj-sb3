/* QEMU's CoreAudio IOProc copies its stereo voice directly to output buffer zero, and
 * asks the device to switch to 2 channels first. Keep the device's own channel count,
 * give QEMU a stereo scratch buffer and map it per channel.
 * Two voices: master (audiodev prototype, buffer-length 5000) and the headphone cue mix
 * (audiodev phones, buffer-length 6000 -> >= PHONES_MIN_FRAMES requested). The cue voice
 * goes to device channels 3/4 (SB3 phones); master to 1/2, and to 3/4 only while no cue
 * voice exists. When the SB3 is used, the master voice is moved to the Mac's own output
 * (system default, or built-in if the default is the SB3): QEMU still believes it is on
 * the SB3, so every call for that voice is redirected by IOProc id. The launcher sets the
 * master frequency to that device's rate. Adapt only this process; never change the
 * system default device. */
#include <CoreAudio/CoreAudio.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "stereo_map.h"

#define MAX_FRAMES 65536
#define MAX_SOURCE_CHANNELS 8
#define PHONES_MIN_FRAMES 240
#define SB3_START_ATTEMPTS 10
#define SB3_START_RETRY_US 300000
typedef struct route {
    AudioDeviceIOProc callback;
    void *context;
    AudioDeviceIOProcID identifier;
    float *scratch;
    int phones; /* cue-mix voice */
    float peak; size_t peak_frames;
    int mac;    /* master voice moved from the SB3 to the Mac output */
    AudioDeviceID device, qemu_device; /* real device / the one QEMU thinks it uses */
    int reported;
    struct route *next;
} Route;
static Route *routes;
static pthread_mutex_t route_lock = PTHREAD_MUTEX_INITIALIZER;
static int selected_reported;
static int sb3_probe_result; /* 0 untested, 1 usable, -1 unavailable */
static unsigned source_channels = 2; /* QEMU voice layout, from its stream-format request */
static UInt32 device_frames;          /* first buffer size QEMU asked for; used for every voice */
static int next_is_phones;            /* set by the buffer-size request before CreateIOProcID */
static int phones_routes;
static AudioDeviceID sb3_device;      /* the SB3 handed to QEMU as default output */
static AudioDeviceID mac_device;      /* Mac output for the master voice (0 = none) */
static int mac_checked;
static int redirecting;               /* master voice being set up: SB3 calls go to mac_device */

static OSStatus silent_probe(AudioDeviceID device, const AudioTimeStamp *now,
                             const AudioBufferList *input, const AudioTimeStamp *input_time,
                             AudioBufferList *output, const AudioTimeStamp *output_time,
                             void *context)
{
    (void)device; (void)now; (void)input; (void)input_time;
    (void)output_time; (void)context;
    if (output)
        for (UInt32 i = 0; i < output->mNumberBuffers; ++i)
            if (output->mBuffers[i].mData)
                memset(output->mBuffers[i].mData, 0, output->mBuffers[i].mDataByteSize);
    return noErr;
}

static int sb3_output_usable(AudioDeviceID device)
{
    if (sb3_probe_result) return sb3_probe_result > 0;
    /* Right after the SB3 (re)connects, or right after a previous client let go of it,
     * AudioDeviceStart can fail with 'what' for a moment. Retry for about 3 s. */
    OSStatus status = noErr;
    for (int attempt = 0; attempt < SB3_START_ATTEMPTS; ++attempt) {
        if (attempt) usleep(SB3_START_RETRY_US);
        AudioDeviceIOProcID identifier = NULL;
        status = AudioDeviceCreateIOProcID(device, silent_probe, NULL, &identifier);
        if (status == noErr) {
            status = AudioDeviceStart(device, identifier);
            if (status == noErr) AudioDeviceStop(device, identifier);
            AudioDeviceDestroyIOProcID(device, identifier);
        }
        if (status == noErr) {
            if (attempt)
                fprintf(stderr, "RX3 audio: DDJ-SB3 output started on attempt %d\n", attempt + 1);
            break;
        }
    }
    sb3_probe_result = status == noErr ? 1 : -1;
    if (status != noErr)
        fprintf(stderr, "RX3 audio: DDJ-SB3 output cannot start (CoreAudio status %d); using Mac default output\n",
                (int)status);
    return sb3_probe_result > 0;
}

static int device_name(AudioDeviceID device, char *label, size_t size)
{
    AudioObjectPropertyAddress name_property = { kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef name = NULL;
    UInt32 name_size = sizeof name;
    label[0] = 0;
    if (AudioObjectGetPropertyData(device, &name_property, 0, NULL, &name_size, &name) != noErr || !name)
        return 0;
    CFStringGetCString(name, label, (CFIndex)size, kCFStringEncodingUTF8);
    CFRelease(name);
    return 1;
}

static int has_output(AudioDeviceID device)
{
    AudioObjectPropertyAddress streams = { kAudioDevicePropertyStreams,
        kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 bytes = 0;
    return AudioObjectGetPropertyDataSize(device, &streams, 0, NULL, &bytes) == noErr && bytes > 0;
}

/* The system default output, or the built-in output if the default is the SB3.
 * AppLauncher.swift (macOutputRate) must pick the same device. */
static AudioDeviceID find_mac_output(void)
{
    AudioObjectPropertyAddress address = { kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioDeviceID device = 0;
    UInt32 size = sizeof device;
    char label[256];
    /* Direct reference = the original call, not rx3_get_property_data. */
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device) == noErr &&
        device && device_name(device, label, sizeof label) && !strstr(label, "DDJ-SB3"))
        return device;
    address.mSelector = kAudioHardwarePropertyDevices;
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &bytes) != noErr || !bytes)
        return 0;
    AudioDeviceID *ids = malloc(bytes), found = 0;
    if (ids && AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &bytes, ids) == noErr)
        for (UInt32 i = 0; i < bytes / sizeof(AudioDeviceID) && !found; ++i) {
            AudioObjectPropertyAddress transport = { kAudioDevicePropertyTransportType,
                kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            UInt32 type = 0, type_size = sizeof type;
            if (AudioObjectGetPropertyData(ids[i], &transport, 0, NULL, &type_size, &type) == noErr &&
                type == kAudioDeviceTransportTypeBuiltIn && has_output(ids[i]))
                found = ids[i];
        }
    free(ids);
    return found;
}

static AudioDeviceID master_target(void)
{
    if (!mac_checked) {
        mac_checked = 1;
        mac_device = find_mac_output();
        char label[256] = "none";
        if (mac_device) device_name(mac_device, label, sizeof label);
        fprintf(stderr, "RX3 audio: master voice -> Mac output: %s\n", label);
    }
    return mac_device;
}

static Route *find_route(AudioDeviceIOProcID identifier)
{
    Route *route = routes;
    while (route && route->identifier != identifier) route = route->next;
    return route;
}

/* QEMU asks for the default output device. Redirect that one query inside
 * QEMU to the SB3 if connected; the macOS system default is untouched. */
static OSStatus rx3_get_property_data(AudioObjectID object,
                                      const AudioObjectPropertyAddress *address,
                                      UInt32 qualifier_size, const void *qualifier,
                                      UInt32 *size, void *data)
{
    OSStatus (*original)(AudioObjectID, const AudioObjectPropertyAddress *,
                         UInt32, const void *, UInt32 *, void *) = AudioObjectGetPropertyData;
    /* QEMU asks whether the *device* runs before starting its own IOProc; with two
     * voices on one device the second would never start. Always say no: a repeated
     * start is harmless and an idle voice outputs silence. */
    if (redirecting && object == sb3_device && mac_device) object = mac_device;
    if (object != kAudioObjectSystemObject && address && size && data &&
        address->mSelector == kAudioDevicePropertyDeviceIsRunning && *size >= sizeof(UInt32)) {
        *(UInt32 *)data = 0;
        *size = sizeof(UInt32);
        return noErr;
    }
    if (object == kAudioObjectSystemObject && address && size && data &&
        address->mSelector == kAudioHardwarePropertyDefaultOutputDevice &&
        *size >= sizeof(AudioDeviceID)) {
        AudioObjectPropertyAddress devices = { kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 bytes = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices,
                                           0, NULL, &bytes) == noErr &&
            bytes >= sizeof(AudioDeviceID)) {
            AudioDeviceID *ids = malloc(bytes);
            if (ids && original(kAudioObjectSystemObject, &devices, 0, NULL,
                                &bytes, ids) == noErr) {
                for (UInt32 i = 0; i < bytes / sizeof(AudioDeviceID); ++i) {
                    AudioObjectPropertyAddress name_property = { kAudioObjectPropertyName,
                        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    CFStringRef name = NULL;
                    UInt32 name_size = sizeof name;
                    if (original(ids[i], &name_property, 0, NULL, &name_size,
                                 &name) != noErr || !name) continue;
                    char label[256] = {0};
                    CFStringGetCString(name, label, sizeof label, kCFStringEncodingUTF8);
                    CFRelease(name);
                    if (strstr(label, "DDJ-SB3 Audio Out") &&
                        sb3_output_usable(ids[i])) {
                        *(AudioDeviceID *)data = sb3_device = ids[i];
                        *size = sizeof(AudioDeviceID);
                        if (!selected_reported++)
                            fprintf(stderr, "RX3 audio: using %s for master output\n", label);
                        free(ids);
                        return noErr;
                    }
                }
            }
            free(ids);
        }
    }
    return original(object, address, qualifier_size, qualifier, size, data);
}

/* QEMU sets the device format to its voice channel count (6 for the cue mix).
 * The SB3 is 4 channels: remember QEMU's layout and set only the device's own count. */
static OSStatus rx3_set_property_data(AudioObjectID object,
                                      const AudioObjectPropertyAddress *address,
                                      UInt32 qualifier_size, const void *qualifier,
                                      UInt32 size, const void *data)
{
    OSStatus (*original)(AudioObjectID, const AudioObjectPropertyAddress *,
                         UInt32, const void *, UInt32, const void *) = AudioObjectSetPropertyData;
    if (address && data && size == sizeof(UInt32) &&
        address->mSelector == kAudioDevicePropertyBufferFrameSize) {
        /* One buffer size per device in this process: both voices must agree with it. */
        UInt32 requested = *(const UInt32 *)data;
        next_is_phones = requested >= PHONES_MIN_FRAMES;
        redirecting = !next_is_phones && object == sb3_device && master_target();
        if (redirecting)
            return original(mac_device, address, qualifier_size, qualifier, size, data);
        if (!device_frames) device_frames = requested;
        return original(object, address, qualifier_size, qualifier, size, &device_frames);
    }
    if (address && data && size >= sizeof(AudioStreamBasicDescription) &&
        address->mSelector == kAudioDevicePropertyStreamFormat &&
        address->mScope == kAudioDevicePropertyScopeOutput) {
        AudioStreamBasicDescription wanted = *(const AudioStreamBasicDescription *)data;
        AudioStreamBasicDescription current = {0};
        UInt32 current_size = sizeof current;
        if (redirecting && object == sb3_device) {
            /* Never change the Mac output's rate: the launcher already matched it. */
            object = mac_device;
            if (AudioObjectGetPropertyData(object, address, 0, NULL, &current_size, &current) == noErr &&
                current.mSampleRate && current.mSampleRate != wanted.mSampleRate) {
                fprintf(stderr, "RX3 audio: master %.0f Hz but Mac output runs %.0f Hz; keeping the Mac rate\n",
                        wanted.mSampleRate, current.mSampleRate);
                wanted.mSampleRate = current.mSampleRate;
                data = &wanted;
            }
            current_size = sizeof current;
        }
        if (wanted.mFormatID == kAudioFormatLinearPCM && wanted.mBitsPerChannel == 32 &&
            wanted.mChannelsPerFrame >= 1 && wanted.mChannelsPerFrame <= MAX_SOURCE_CHANNELS &&
            AudioObjectGetPropertyData(object, address, 0, NULL, &current_size, &current) == noErr &&
            current.mChannelsPerFrame && current.mChannelsPerFrame != wanted.mChannelsPerFrame) {
            source_channels = wanted.mChannelsPerFrame;
            fprintf(stderr, "RX3 audio: QEMU voice %u ch, device keeps %u ch\n",
                    source_channels, (unsigned)current.mChannelsPerFrame);
            wanted.mChannelsPerFrame = current.mChannelsPerFrame;
            wanted.mBytesPerFrame = wanted.mBytesPerPacket = current.mChannelsPerFrame * 4;
            return original(object, address, qualifier_size, qualifier, sizeof wanted, &wanted);
        }
        if (wanted.mChannelsPerFrame >= 1 && wanted.mChannelsPerFrame <= MAX_SOURCE_CHANNELS)
            source_channels = wanted.mChannelsPerFrame;
    }
    return original(object, address, qualifier_size, qualifier, size, data);
}

static OSStatus output(AudioDeviceID device, const AudioTimeStamp *now,
                       const AudioBufferList *input, const AudioTimeStamp *input_time,
                       AudioBufferList *destination, const AudioTimeStamp *output_time,
                       void *opaque)
{
    Route *route = opaque;
    if (!destination || !destination->mNumberBuffers)
        return noErr;
    AudioBuffer *first = &destination->mBuffers[0];
    if (!first->mNumberChannels)
        return noErr;
    size_t frames = first->mDataByteSize / (sizeof(float) * first->mNumberChannels);
    if (frames > MAX_FRAMES)
        return kAudio_ParamError;
    unsigned voice = source_channels;
    memset(route->scratch, 0, frames * voice * sizeof(float));
    AudioBufferList source = {
        .mNumberBuffers = 1,
        .mBuffers = {{ .mNumberChannels = voice,
                       .mDataByteSize = (UInt32)(frames * voice * sizeof(float)),
                       .mData = route->scratch }}
    };
    (void)device;
    /* QEMU drops callbacks from any device but the one it opened (the SB3 for a moved voice). */
    OSStatus result = route->callback(route->qemu_device, now, input, input_time, &source,
                                      output_time, route->context);
    /* Each voice writes only its own channels: the HAL mixes the IOProcs' outputs. */
    unsigned low = route->phones ? 2 : 0;
    unsigned high = route->phones || route->mac || phones_routes ? low + 2 : ~0u;
    unsigned first_channel = 0;
    for (UInt32 index = 0; index < destination->mNumberBuffers; ++index) {
        AudioBuffer *buffer = &destination->mBuffers[index];
        if (buffer->mData && buffer->mNumberChannels) {
            size_t count = buffer->mDataByteSize / (sizeof(float) * buffer->mNumberChannels);
            if (count > frames) count = frames;
            rx3_map_range(buffer->mData, buffer->mNumberChannels, first_channel,
                          route->scratch, voice, count, low, high);
        }
        first_channel += buffer->mNumberChannels;
    }
    if (getenv("RX3_MIDI_DEBUG")) {   /* peak per voice every ~5 s */
        float peak = 0;
        for (size_t i = 0; i < frames * voice; ++i) {
            float v = route->scratch[i] < 0 ? -route->scratch[i] : route->scratch[i];
            if (v > peak) peak = v;
        }
        if (peak > route->peak) route->peak = peak;
        route->peak_frames += frames;
        if (route->peak_frames > 220500) {
            fprintf(stderr, "RX3 CoreAudio adapter: %s peak %.4f\n",
                    route->phones ? "phones" : "master", route->peak);
            route->peak = 0; route->peak_frames = 0;
        }
    }
    if (!route->reported) {
        route->reported = 1;
        fprintf(stderr, "RX3 CoreAudio adapter: %s voice (%u ch) -> %u device channels, %zu frames/callback\n",
                route->phones ? "headphone cue" : route->mac ? "master (Mac output)" : "master",
                voice, first_channel, frames);
    }
    return result;
}

static OSStatus rx3_create(AudioDeviceID device, AudioDeviceIOProc callback,
                          void *context, AudioDeviceIOProcID *identifier)
{
    typedef OSStatus (*Create)(AudioDeviceID, AudioDeviceIOProc, void *, AudioDeviceIOProcID *);
    /* dyld leaves the interposing image's direct references bound to the
     * original. dlsym returns the replacement on current macOS and recurses. */
    Create original = AudioDeviceCreateIOProcID;
    AudioDeviceID qemu_device = device;
    int mac = redirecting && device == sb3_device;
    redirecting = 0;
    if (mac) device = mac_device;
    AudioStreamBasicDescription format = {0};
    UInt32 size = sizeof format;
    AudioObjectPropertyAddress property = { kAudioDevicePropertyStreamFormat,
        kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    OSStatus status = AudioObjectGetPropertyData(device, &property, 0, NULL, &size, &format);
    if (status || format.mFormatID != kAudioFormatLinearPCM ||
        !(format.mFormatFlags & kAudioFormatFlagIsFloat) || format.mBitsPerChannel != 32)
        return original(device, callback, context, identifier);
    Route *route = calloc(1, sizeof *route);
    if (!route) return kAudio_MemFullError;
    route->scratch = calloc(MAX_FRAMES * MAX_SOURCE_CHANNELS, sizeof(float));
    if (!route->scratch) { free(route); return kAudio_MemFullError; }
    route->callback = callback;
    route->context = context;
    route->phones = next_is_phones;
    route->mac = mac;
    route->device = device;
    route->qemu_device = qemu_device;
    next_is_phones = 0;
    status = original(device, output, route, identifier);
    if (status) { free(route->scratch); free(route); return status; }
    route->identifier = *identifier;
    pthread_mutex_lock(&route_lock);
    route->next = routes;
    routes = route;
    phones_routes += route->phones;
    pthread_mutex_unlock(&route_lock);
    return status;
}

static OSStatus rx3_destroy(AudioDeviceID device, AudioDeviceIOProcID identifier)
{
    typedef OSStatus (*Destroy)(AudioDeviceID, AudioDeviceIOProcID);
    Destroy original = AudioDeviceDestroyIOProcID;
    pthread_mutex_lock(&route_lock);
    Route *moved = find_route(identifier);
    if (moved) device = moved->device;
    pthread_mutex_unlock(&route_lock);
    AudioDeviceStop(device, identifier); /* QEMU skips the stop: running is always "no" */
    OSStatus status = original(device, identifier);
    if (status) return status;
    pthread_mutex_lock(&route_lock);
    Route **link = &routes;
    while (*link && (*link)->identifier != identifier) link = &(*link)->next;
    if (*link) {
        Route *route = *link;
        *link = route->next;
        phones_routes -= route->phones;
        free(route->scratch);
        free(route);
    }
    pthread_mutex_unlock(&route_lock);
    return status;
}

/* QEMU starts each voice once and never retries; the same transient 'what' failure
 * the probe retries for can hit here (seen right after a relaunch). */
static OSStatus rx3_start(AudioDeviceID device, AudioDeviceIOProcID identifier)
{
    typedef OSStatus (*Start)(AudioDeviceID, AudioDeviceIOProcID);
    Start original = AudioDeviceStart;
    pthread_mutex_lock(&route_lock);
    Route *route = find_route(identifier);
    if (route) device = route->device;
    pthread_mutex_unlock(&route_lock);
    OSStatus status = original(device, identifier);
    for (int attempt = 1; status != noErr && attempt < SB3_START_ATTEMPTS; ++attempt) {
        usleep(SB3_START_RETRY_US / 3);
        status = original(device, identifier);
        if (status == noErr)
            fprintf(stderr, "RX3 audio: voice started on attempt %d\n", attempt + 1);
    }
    if (status != noErr)
        fprintf(stderr, "RX3 audio: voice start failed (CoreAudio status %d)\n", (int)status);
    return status;
}

static OSStatus rx3_stop(AudioDeviceID device, AudioDeviceIOProcID identifier)
{
    typedef OSStatus (*Stop)(AudioDeviceID, AudioDeviceIOProcID);
    Stop original = AudioDeviceStop;
    pthread_mutex_lock(&route_lock);
    Route *route = find_route(identifier);
    if (route) device = route->device;
    pthread_mutex_unlock(&route_lock);
    return original(device, identifier);
}

__attribute__((used)) static struct { const void *replacement, *original; }
interpose[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)rx3_get_property_data, (const void *)AudioObjectGetPropertyData },
    { (const void *)rx3_set_property_data, (const void *)AudioObjectSetPropertyData },
    { (const void *)rx3_create, (const void *)AudioDeviceCreateIOProcID },
    { (const void *)rx3_destroy, (const void *)AudioDeviceDestroyIOProcID },
    { (const void *)rx3_start, (const void *)AudioDeviceStart },
    { (const void *)rx3_stop, (const void *)AudioDeviceStop }
};
