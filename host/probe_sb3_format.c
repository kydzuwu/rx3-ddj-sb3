#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    AudioObjectPropertyAddress all = { kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &all, 0, NULL, &bytes)) return 1;
    AudioDeviceID *ids = malloc(bytes);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &all, 0, NULL, &bytes, ids)) return 1;
    for (UInt32 i = 0; i < bytes / sizeof(*ids); ++i) {
        AudioObjectPropertyAddress prop = { kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        CFStringRef name = NULL;
        UInt32 size = sizeof name;
        if (AudioObjectGetPropertyData(ids[i], &prop, 0, NULL, &size, &name)) continue;
        char text[256] = {0};
        CFStringGetCString(name, text, sizeof text, kCFStringEncodingUTF8);
        CFRelease(name);
        if (!strstr(text, "DDJ-SB3")) continue;
        printf("Device %u: %s\n", ids[i], text);
        prop.mSelector = kAudioDevicePropertyNominalSampleRate;
        prop.mScope = kAudioObjectPropertyScopeGlobal;
        Float64 rate = 0;
        size = sizeof rate;
        OSStatus s = AudioObjectGetPropertyData(ids[i], &prop, 0, NULL, &size, &rate);
        printf("Rate status %d value %.0f\n", (int)s, rate);
        prop.mSelector = kAudioDevicePropertyAvailableNominalSampleRates;
        if (!AudioObjectGetPropertyDataSize(ids[i], &prop, 0, NULL, &size)) {
            AudioValueRange *ranges = malloc(size);
            if (!AudioObjectGetPropertyData(ids[i], &prop, 0, NULL, &size, ranges))
                for (UInt32 j = 0; j < size / sizeof(*ranges); ++j)
                    printf("Available %.0f..%.0f\n", ranges[j].mMinimum, ranges[j].mMaximum);
            free(ranges);
        }
        prop.mSelector = kAudioDevicePropertyStreamFormat;
        prop.mScope = kAudioObjectPropertyScopeOutput;
        AudioStreamBasicDescription format = {0}; size = sizeof format;
        s = AudioObjectGetPropertyData(ids[i], &prop, 0, NULL, &size, &format);
        printf("Format status %d rate %.0f channels %u bits %u flags 0x%x\n",
               (int)s, format.mSampleRate, format.mChannelsPerFrame,
               format.mBitsPerChannel, (unsigned)format.mFormatFlags);
        const AudioObjectPropertySelector selectors[] = {
            kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyDeviceIsRunning,
            kAudioDevicePropertyHogMode
        };
        const char *labels[] = {"alive", "running", "hog pid"};
        for (int j = 0; j < 3; ++j) {
            prop.mSelector = selectors[j]; prop.mScope = kAudioObjectPropertyScopeGlobal;
            UInt32 value = 0; size = sizeof value;
            s = AudioObjectGetPropertyData(ids[i], &prop, 0, NULL, &size, &value);
            printf("%s status %d value %d\n", labels[j], (int)s, (int)value);
        }
    }
    free(ids);
}
