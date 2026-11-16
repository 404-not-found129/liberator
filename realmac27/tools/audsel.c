// audsel: list CoreAudio output devices (no args) or make the one whose name contains ARG the default output.
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>
static int name(AudioObjectID d, char *b, size_t n) {
    CFStringRef s = NULL; UInt32 sz = sizeof s;
    AudioObjectPropertyAddress a = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, &s) || !s) return 0;
    CFStringGetCString(s, b, (CFIndex)n, kCFStringEncodingUTF8); CFRelease(s); return 1;
}
static int outs(AudioObjectID d) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 sz = 0; return AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &sz) == 0 && sz > 0;
}
int main(int argc, char **argv) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID ids[64]; if (sz > sizeof ids) sz = sizeof ids;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    AudioObjectPropertyAddress da = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectID cur = 0; UInt32 cs = sizeof cur; AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &cs, &cur);
    for (UInt32 i = 0; i < sz / sizeof(AudioObjectID); i++) {
        char n[256] = ""; if (!name(ids[i], n, sizeof n) || !outs(ids[i])) continue;
        if (argc < 2) { printf("%u%s %s\n", ids[i], ids[i] == cur ? " *" : "", n); continue; }
        if (strstr(n, argv[1])) {
            OSStatus e = AudioObjectSetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, sizeof ids[i], &ids[i]);
            AudioObjectPropertyAddress sa = { kAudioHardwarePropertyDefaultSystemOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            AudioObjectSetPropertyData(kAudioObjectSystemObject, &sa, 0, NULL, sizeof ids[i], &ids[i]);
            printf("default output -> %u %s (status %d)\n", ids[i], n, (int)e); return e != 0;
        }
    }
    return argc < 2 ? 0 : 2;
}
