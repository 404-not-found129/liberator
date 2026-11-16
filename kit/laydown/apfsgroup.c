/* apfsgroup - put existing APFS volumes of one container into one volume group.
 *
 *   apfsgroup <container, e.g. disk1> <group UUID> <slice number> <slice number> [...]
 *
 * diskutil can add a NEW system volume to the group of an existing data volume, and nothing else: it cannot group two
 * volumes that both exist. The lay-down needs exactly that. The system volume has to be made by a block restore of
 * Apple's image, because only a block restore carries the firmlink attribute of /private, /Library, /Users and the
 * other folders that show the data volume (com.apple.fs.firmlink; the file system refuses to let a file copy write
 * it), and a block restore replaces the volume it restores to, which takes it out of any group it was in.
 *
 * This calls the function Apple's own restore tool (asr) calls when it restores a system and data pair:
 * APFSContainerVolumeGroupAdd(container, array of slice numbers, group UUID) in the system's APFS framework.
 * The group UUID is the data volume's UUID, as on every installed macOS. Exit 0 only when the call returns 0. */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: apfsgroup <container> <group uuid> <slice> <slice> [...]\n"); return 2; }
    const char *c = argv[1];
    if (strncmp(c, "disk", 4) != 0 || strlen(c) > 12 || c[4] == 0 || strspn(c + 4, "0123456789") != strlen(c + 4)) {
        fprintf(stderr, "apfsgroup: the container must be diskN\n"); return 2;
    }
    uuid_t g;
    if (uuid_parse(argv[2], g) != 0) { fprintf(stderr, "apfsgroup: not a UUID: %s\n", argv[2]); return 2; }
    CFMutableArrayRef a = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    for (int i = 3; i < argc; i++) {
        if (argv[i][0] == 0 || strspn(argv[i], "0123456789") != strlen(argv[i]) || strlen(argv[i]) > 4) {
            fprintf(stderr, "apfsgroup: not a slice number: %s\n", argv[i]); return 2;
        }
        int n = atoi(argv[i]);
        CFNumberRef num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &n);
        CFArrayAppendValue(a, num); CFRelease(num);
    }
    void *h = dlopen("/System/Library/PrivateFrameworks/APFS.framework/APFS", RTLD_NOW);
    if (!h) { fprintf(stderr, "apfsgroup: this macOS has no APFS framework to call\n"); return 3; }
    int (*add)(const char *, CFArrayRef, unsigned char *) =
        (int (*)(const char *, CFArrayRef, unsigned char *))dlsym(h, "APFSContainerVolumeGroupAdd");
    if (!add) { fprintf(stderr, "apfsgroup: this macOS's APFS framework has no volume-group call\n"); return 3; }
    int rc = add(c, a, g);
    CFRelease(a);
    if (rc != 0) { fprintf(stderr, "apfsgroup: grouping failed (%d)\n", rc); return 1; }
    return 0;
}
