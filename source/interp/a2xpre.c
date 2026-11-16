/* a2xpre — runs in the host before CoreFoundation initializes and does the dispatcher's job in-process.
 * Only when this process is the kernel's interpreter exec: executable basename "a2xhost" and argv =
 * [a2xhost, <arm64 guest>, args...]. Every other entry (dispatched, bundle, self-hosted) is untouched. */
#include <crt_externs.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define NEEDLE "/System/iOSSupport"


/* Read the guest file, never mmap(fd). The adopt half (a2x_adopt_signature) registers the app's signature
      with codeLimit cut to 4K-page slots, and the kernel keeps it on the vnode until it is recycled. A file mapping of
      that app is then validated against the cut blob: every page past ~100 KB is "Invalid" and the union-entitled host
      is SIGKILLed (CS_KILL). With mmap, TextEdit exited with rc=137 on every launch after its first adopt. */
static void *a2xpre_readall(int fd, size_t len) {
    uint8_t *b = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b == MAP_FAILED) return MAP_FAILED;
    for (size_t o = 0; o < len; ) {
        ssize_t r = pread(fd, b + o, len - o, (off_t)o);
        if (r <= 0) { munmap(b, len); return MAP_FAILED; }
        o += (size_t)r;
    }
    return b;
}

/* 1 = arm64 Mach-O (thin or any fat arch), 0 = not, and *cat = guest names /System/iOSSupport. */
static int inspect(const char *path, int *cat) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 32) { close(fd); return 0; }
    size_t len = (size_t)st.st_size;
    const uint8_t *m = a2xpre_readall(fd, len);
    close(fd);
    if (m == MAP_FAILED) return 0;
    int arm = 0;
    uint32_t mg = *(const uint32_t *)m;
    if (mg == MH_MAGIC_64) {
        arm = (((const struct mach_header_64 *)m)->cputype == CPU_TYPE_ARM64);
    } else if (mg == FAT_CIGAM) {                      /* fat headers are big-endian */
        uint32_t n = __builtin_bswap32(((const struct fat_header *)m)->nfat_arch);
        const struct fat_arch *fa = (const struct fat_arch *)(m + sizeof(struct fat_header));
        for (uint32_t i = 0; i < n && i < 8 && (const uint8_t *)(fa + i + 1) <= m + len; i++)
            if ((int32_t)__builtin_bswap32((uint32_t)fa[i].cputype) == CPU_TYPE_ARM64) arm = 1;
    }
    *cat = arm && memmem(m, len, NEEDLE, sizeof NEEDLE - 1) != 0;
    munmap((void *)m, len);
    return arm;
}

__attribute__((constructor))
static void a2xpre_init(int argc, const char **argv, const char **envp, const char **apple) {
    (void)envp; (void)apple;
    char self[4096]; uint32_t sz = sizeof self;
    if (getenv("A2X_GUEST") || argc < 2) return;
    if (_NSGetExecutablePath(self, &sz) != 0) return;
    const char *b = strrchr(self, '/'); b = b ? b + 1 : self;
    if (strcmp(b, "a2xhost") != 0) return;
    int cat = 0;
    if (!inspect(argv[1], &cat)) return;
    setenv("CFProcessPath", argv[1], 1);
    setenv("A2X_GUEST", argv[1], 1);
    *_NSGetArgc() = argc - 1;                           /* CoreFoundation/Foundation snapshot these */
    *_NSGetArgv() = (char **)(argv + 1);
#ifdef A2XPRE_CAT_BUILD
    if (!cat) {                                         /* AppKit guest: the mac host, as the dispatcher did */
        execv("/usr/libexec/a2x/a2xhost-mac", (char *const *)(argv + 1));
        { static const char m[] = "a2xpre: execv /usr/libexec/a2x/a2xhost-mac failed\n"; ssize_t r = write(2, m, sizeof m - 1); (void)r; }
        _exit(5);
    }
#endif
}
