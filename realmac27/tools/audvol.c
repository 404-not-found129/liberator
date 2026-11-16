// audvol NAME [vol]: print (or set, 0..1) the output volume and mute of the device whose name contains NAME
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID ids[64]; if (sz > sizeof ids) sz = sizeof ids; AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    for (UInt32 i = 0; i < sz / sizeof(AudioObjectID); i++) {
        CFStringRef s = NULL; UInt32 ss = sizeof s; char n[256] = "";
        AudioObjectPropertyAddress na = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &ss, &s) || !s) continue;
        CFStringGetCString(s, n, sizeof n, kCFStringEncodingUTF8); CFRelease(s);
        if (argc > 1 && strcmp(n, argv[1])) continue;
        for (UInt32 el = 0; el <= 2; el++) {
            AudioObjectPropertyAddress va = { kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput, el };
            AudioObjectPropertyAddress ma = { kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput, el };
            if (argc > 2 && AudioObjectHasProperty(ids[i], &va)) { Float32 v = atof(argv[2]); AudioObjectSetPropertyData(ids[i], &va, 0, NULL, sizeof v, &v); }
            if (argc > 2 && AudioObjectHasProperty(ids[i], &ma)) { UInt32 m = 0; AudioObjectSetPropertyData(ids[i], &ma, 0, NULL, sizeof m, &m); }
            Float32 v = -1; UInt32 m = 9, vs = sizeof v, ms = sizeof m;
            int hv = AudioObjectHasProperty(ids[i], &va), hm = AudioObjectHasProperty(ids[i], &ma);
            if (hv) AudioObjectGetPropertyData(ids[i], &va, 0, NULL, &vs, &v);
            if (hm) AudioObjectGetPropertyData(ids[i], &ma, 0, NULL, &ms, &m);
            if (hv || hm) printf("%u %s el%u vol %.2f mute %u\n", ids[i], n, el, v, m);
        }
    }
    return 0;
}
