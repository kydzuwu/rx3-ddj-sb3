#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static OSStatus callback(AudioDeviceID id, const AudioTimeStamp *now,
                         const AudioBufferList *in, const AudioTimeStamp *in_time,
                         AudioBufferList *out, const AudioTimeStamp *out_time, void *context) {
    (void)id; (void)now; (void)in; (void)in_time; (void)out_time; (void)context;
    for (UInt32 i = 0; i < out->mNumberBuffers; ++i)
        memset(out->mBuffers[i].mData, 0, out->mBuffers[i].mDataByteSize);
    return noErr;
}
int main(void) {
    AudioObjectPropertyAddress all = { kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 bytes = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &all, 0, NULL, &bytes);
    AudioDeviceID *ids = malloc(bytes), device = 0;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &all, 0, NULL, &bytes, ids);
    for (UInt32 i = 0; i < bytes / sizeof(*ids); ++i) {
        AudioObjectPropertyAddress property = { kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        CFStringRef name = NULL;
        UInt32 size = sizeof name;
        if (AudioObjectGetPropertyData(ids[i], &property, 0, NULL, &size, &name)) continue;
        char label[256] = {0};
        CFStringGetCString(name, label, sizeof label, kCFStringEncodingUTF8);
        CFRelease(name);
        if (strstr(label, "DDJ-SB3")) { device = ids[i]; break; }
    }
    free(ids);
    if (!device) return 2;
    AudioDeviceIOProcID proc = NULL;
    OSStatus s = AudioDeviceCreateIOProcID(device, callback, NULL, &proc);
    printf("create %d\n", (int)s);
    if (s) return 3;
    s = AudioDeviceStart(device, proc);
    printf("start %d\n", (int)s);
    if (!s) { sleep(1); AudioDeviceStop(device, proc); }
    AudioDeviceDestroyIOProcID(device, proc);
    return s ? 4 : 0;
}
