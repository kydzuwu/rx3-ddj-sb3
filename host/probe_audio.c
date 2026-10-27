#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
int main(void) {
    AudioDeviceID device = 0;
    UInt32 size = sizeof device;
    AudioObjectPropertyAddress property = { kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, NULL,
                                   &size, &device) != noErr) return 1;
    property.mSelector = kAudioObjectPropertyName;
    CFStringRef name = NULL;
    size = sizeof name;
    if (AudioObjectGetPropertyData(device, &property, 0, NULL, &size, &name) != noErr) return 1;
    char label[256] = {0};
    CFStringGetCString(name, label, sizeof label, kCFStringEncodingUTF8);
    CFRelease(name);
    puts(label);
    return 0;
}
