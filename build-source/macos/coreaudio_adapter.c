/* QEMU's CoreAudio IOProc copies stereo floats directly to output buffer zero.
 * An interleaved four-channel device interprets that as half a buffer of audio.
 * Adapt only this process's callbacks; never change the system default device. */
#include <CoreAudio/CoreAudio.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_map.h"

#define MAX_FRAMES 65536
typedef struct route {
    AudioDeviceIOProc callback;
    void *context;
    AudioDeviceIOProcID identifier;
    float *scratch;
    int reported;
    struct route *next;
} Route;
static Route *routes;
static pthread_mutex_t route_lock = PTHREAD_MUTEX_INITIALIZER;

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
    memset(route->scratch, 0, frames * 2 * sizeof(float));
    AudioBufferList stereo = {
        .mNumberBuffers = 1,
        .mBuffers = {{ .mNumberChannels = 2,
                       .mDataByteSize = (UInt32)(frames * 2 * sizeof(float)),
                       .mData = route->scratch }}
    };
    OSStatus result = route->callback(device, now, input, input_time, &stereo,
                                      output_time, route->context);
    unsigned first_channel = 0;
    for (UInt32 index = 0; index < destination->mNumberBuffers; ++index) {
        AudioBuffer *buffer = &destination->mBuffers[index];
        if (buffer->mData && buffer->mNumberChannels) {
            size_t count = buffer->mDataByteSize / (sizeof(float) * buffer->mNumberChannels);
            if (count > frames) count = frames;
            rx3_map_stereo(buffer->mData, buffer->mNumberChannels, first_channel,
                           route->scratch, count);
        }
        first_channel += buffer->mNumberChannels;
    }
    if (!route->reported) {
        route->reported = 1;
        fprintf(stderr, "RX3 CoreAudio adapter: stereo -> %u device channels, %zu frames/callback\n",
                first_channel, frames);
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
    route->scratch = calloc(MAX_FRAMES * 2, sizeof(float));
    if (!route->scratch) { free(route); return kAudio_MemFullError; }
    route->callback = callback;
    route->context = context;
    status = original(device, output, route, identifier);
    if (status) { free(route->scratch); free(route); return status; }
    route->identifier = *identifier;
    pthread_mutex_lock(&route_lock);
    route->next = routes;
    routes = route;
    pthread_mutex_unlock(&route_lock);
    return status;
}

static OSStatus rx3_destroy(AudioDeviceID device, AudioDeviceIOProcID identifier)
{
    typedef OSStatus (*Destroy)(AudioDeviceID, AudioDeviceIOProcID);
    Destroy original = AudioDeviceDestroyIOProcID;
    OSStatus status = original(device, identifier);
    if (status) return status;
    pthread_mutex_lock(&route_lock);
    Route **link = &routes;
    while (*link && (*link)->identifier != identifier) link = &(*link)->next;
    if (*link) {
        Route *route = *link;
        *link = route->next;
        free(route->scratch);
        free(route);
    }
    pthread_mutex_unlock(&route_lock);
    return status;
}

__attribute__((used)) static struct { const void *replacement, *original; }
interpose[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)rx3_create, (const void *)AudioDeviceCreateIOProcID },
    { (const void *)rx3_destroy, (const void *)AudioDeviceDestroyIOProcID }
};
