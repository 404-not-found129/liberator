// machofix.c — read an arm64 Mach-O's fixups at load time: rebases to slide, imports to resolve.
//
// This is what moves the work from build time to the OS. xlat2 computes an app's fixups on the build
//    machine (from `dyld_info -fixups`) and bakes them into a per-app host, which is why every app needed its
//    own build. An interpreter does not need any of that: a rebase is a slide, and an import is just dlsym +
//    store, because the interpreter reads the slot and the native-call door handles whatever address is in it.
//    No typed thunks, no libffi setup per import, nothing generated per app.
// Validated against `dyld_info -fixups`, not against my memory of the format: interp/fixtest prints every
//    (address, kind, target) this finds and the test script diffs it against dyld's own answer for the same
//    binary. The bit layouts below are from mach-o/fixup-chains.h and every one of them is checkable that way.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── the chained-fixups structures, as the loader format defines them ── */
#define PTR_ARM64E              1
#define PTR_64                  2
#define PTR_64_OFFSET           6
#define PTR_ARM64E_KERNEL       7
#define PTR_ARM64E_USERLAND     9
#define PTR_ARM64E_FIRMWARE    10
#define PTR_ARM64E_USERLAND24  12

#define IMPORT          1
#define IMPORT_ADDEND   2
#define IMPORT_ADDEND64 3

#define START_NONE 0xFFFF

struct cf_header {
    uint32_t fixups_version, starts_offset, imports_offset, symbols_offset;
    uint32_t imports_count, imports_format, symbols_format;
};
struct cf_starts_in_image { uint32_t seg_count; uint32_t seg_info_offset[1]; };
struct cf_starts_in_segment {
    uint32_t size;
    uint16_t page_size, pointer_format;
    uint64_t segment_offset;
    uint32_t max_valid_pointer;
    uint16_t page_count, page_start[1];
};

static inline int is_arm64e_fmt(unsigned f) {
    return f == PTR_ARM64E || f == PTR_ARM64E_KERNEL || f == PTR_ARM64E_USERLAND
        || f == PTR_ARM64E_FIRMWARE || f == PTR_ARM64E_USERLAND24;
}
/* Which formats make A rebase target an offset rather than an unslid address. Getting this wrong shifts
      every rebased pointer by the image's base, which is the kind of error that boots and then dies far away. */
static inline int rebase_is_offset(unsigned f) {
    return f == PTR_64_OFFSET || f == PTR_ARM64E_USERLAND || f == PTR_ARM64E_USERLAND24
        || f == PTR_ARM64E_KERNEL || f == PTR_ARM64E_FIRMWARE;
}

/* one fixup, handed to the caller */
struct a64_fixup {
    uint64_t at;            /* the unslid address of the slot */
    uint64_t target;        /* rebase: the unslid target. bind: the addend */
    const char *symbol;     /* bind only */
    int lib_ordinal;        /* bind only */
    int weak;               /* bind only */
    int is_bind;
};

typedef void (*a64_fixup_cb)(const struct a64_fixup *f, void *ctx);

/* ── the Mach-O load commands we need ── */
struct mh64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct lc { uint32_t cmd, cmdsize; };
struct seg64 {
    uint32_t cmd, cmdsize; char segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
};
struct linkedit { uint32_t cmd, cmdsize, dataoff, datasize; };

#define LC_SEGMENT_64_          0x19
#define LC_DYLD_CHAINED_FIXUPS_ 0x80000034

/* decode the imports table entry `ord` into a symbol name and an addend */
/* The symbol-name offset is A file field and was unbounded. IMPORT's name offset is 23 bits, so it
      can name a string up to 8 MB past the symbols table while the whole blob is a few hundred KB —
      and host.c's apply() then strcmp()s that against the bridge table and passes it to dlsym(). The
      blob size is the only thing that can say no, so it is now carried in. */
static int import_at(const struct cf_header *h, const uint8_t *base, uint32_t blobsz, uint32_t ord,
                     const char **sym, int64_t *addend, int *lib, int *weak) {
    const uint8_t *imp = base + h->imports_offset;
    const char *strs = (const char *)(base + h->symbols_offset);
    if (ord >= h->imports_count) return 0;
#define IMP_NAME(off) do { uint32_t no_ = (off); \
        if ((uint64_t)h->symbols_offset + no_ >= blobsz) return 0; \
        *sym = strs + no_; } while (0)
    switch (h->imports_format) {
    case IMPORT: {
        uint32_t v;
        if ((uint64_t)h->imports_offset + 4 * (uint64_t)ord + 4 > blobsz) return 0;
        memcpy(&v, imp + 4 * ord, 4);
        *lib = (int)(v & 0xff); *weak = (int)((v >> 8) & 1);
        IMP_NAME(v >> 9); *addend = 0;
        return 1;
    }
    case IMPORT_ADDEND: {
        uint32_t v; int32_t a;
        if ((uint64_t)h->imports_offset + 8 * (uint64_t)ord + 8 > blobsz) return 0;
        memcpy(&v, imp + 8 * ord, 4); memcpy(&a, imp + 8 * ord + 4, 4);
        *lib = (int)(v & 0xff); *weak = (int)((v >> 8) & 1);
        IMP_NAME(v >> 9); *addend = a;
        return 1;
    }
    case IMPORT_ADDEND64: {
        uint64_t v; int64_t a;
        if ((uint64_t)h->imports_offset + 16 * (uint64_t)ord + 16 > blobsz) return 0;
        memcpy(&v, imp + 16 * ord, 8); memcpy(&a, imp + 16 * ord + 8, 8);
        *lib = (int)(v & 0xffff); *weak = (int)((v >> 16) & 1);
        IMP_NAME((uint32_t)(v >> 32)); *addend = a;
        return 1;
    }
    default: return 0;
    }
#undef IMP_NAME
}

/* walk every chain in the image and report each fixup. `file` is the whole mapped file. */
/* A file on disk can be universal. apfs_boot_util is fat (and dyld_info picks its x86_64 slice), so a
      parser that only accepts a thin Mach-O rejects it outright — and the host is handed whatever path the
      user double-clicked. Pick the arm64/arm64e slice; the fat header is BIG-endian, unlike everything else
      in the file. */
#define FAT_MAGIC_BE 0xbebafeca   /* 0xcafebabe seen little-endian */
#define CPU_ARM64    0x0100000c

static const uint8_t *thin_arm64(const uint8_t *file, size_t flen, size_t *len_out) {
    uint32_t magic;
    memcpy(&magic, file, 4);
    if (magic != FAT_MAGIC_BE) { *len_out = flen; return file; }
    uint32_t n;
    memcpy(&n, file + 4, 4);
    n = __builtin_bswap32(n);
    for (uint32_t k = 0; k < n && 8 + 20 * (k + 1) <= flen; k++) {
        const uint8_t *fa = file + 8 + 20 * k;
        uint32_t ct, off, sz;
        memcpy(&ct, fa, 4); memcpy(&off, fa + 8, 4); memcpy(&sz, fa + 12, 4);
        ct = __builtin_bswap32(ct); off = __builtin_bswap32(off); sz = __builtin_bswap32(sz);
        if (ct == CPU_ARM64 && (size_t)off + sz <= flen) { *len_out = sz; return file + off; }
    }
    *len_out = 0;
    return 0;                                   /* fat, but no arm64 slice: not ours to run */
}

/* SLIDE. The main executable asks for a fixed base and gets it, but a dylib is built at vmaddr 0 and has
      to be placed somewhere free — so every address this reports, and every offset-form rebase target, is
      relative to where the image was actually loaded. The main image simply passes slide 0. */
/* ── the older fix-up tables (LC_DYLD_INFO): opcode streams for rebases, binds and lazy binds ── */
struct dyld_info_cmd {
    uint32_t cmd, cmdsize, rebase_off, rebase_size, bind_off, bind_size, weak_bind_off, weak_bind_size,
             lazy_bind_off, lazy_bind_size, export_off, export_size;
};
static int cl_uleb(const uint8_t **p, const uint8_t *end, uint64_t *out) {
    uint64_t v = 0; unsigned shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        if (shift < 64) v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) { *out = v; return 1; }
    }
    return 0;
}
static int cl_sleb(const uint8_t **p, const uint8_t *end, int64_t *out) {
    int64_t v = 0; unsigned shift = 0; uint8_t b = 0;
    while (*p < end) {
        b = *(*p)++;
        if (shift < 64) v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) { if (shift < 64 && (b & 0x40)) v |= -((int64_t)1 << shift); *out = v; return 1; }
    }
    return 0;
}
/* one rebase: the slot holds an unslid address in the file; it becomes that address plus the slide */
static int cl_rebase(const uint8_t *file, size_t flen, uint64_t slide, const uint64_t *seg_vmaddr,
                     const uint64_t *seg_fileoff, unsigned nseg, unsigned seg, uint64_t off,
                     a64_fixup_cb cb, void *ctx) {
    if (seg >= nseg) return -7;
    uint64_t fo = seg_fileoff[seg] + off;
    if (fo + 8 > flen) return -3;
    uint64_t raw; memcpy(&raw, file + fo, 8);
    struct a64_fixup f; memset(&f, 0, sizeof f);
    f.at = seg_vmaddr[seg] + off + slide; f.target = raw + slide;
    cb(&f, ctx);
    return 0;
}
static int cl_walk_rebases(const uint8_t *file, size_t flen, uint64_t slide, const uint64_t *seg_vmaddr,
                           const uint64_t *seg_fileoff, unsigned nseg, const uint8_t *p, const uint8_t *end,
                           a64_fixup_cb cb, void *ctx) {
    unsigned seg = 0, type = 1; uint64_t off = 0, n, skip; int rc;
    while (p < end) {
        uint8_t op = *p & 0xf0, imm = *p & 0x0f; p++;
        switch (op) {
        case 0x00: return 0;
        case 0x10: type = imm; break;
        case 0x20: seg = imm; if (!cl_uleb(&p, end, &off)) return -8; break;
        case 0x30: if (!cl_uleb(&p, end, &n)) return -8; off += n; break;
        case 0x40: off += (uint64_t)imm * 8; break;
        case 0x50: n = imm; goto times;
        case 0x60: if (!cl_uleb(&p, end, &n)) return -8;
        times:
            if (type != 1) return -9;                   /* only whole pointers exist on arm64 */
            for (uint64_t i = 0; i < n; i++) {
                if ((rc = cl_rebase(file, flen, slide, seg_vmaddr, seg_fileoff, nseg, seg, off, cb, ctx))) return rc;
                off += 8;
            }
            break;
        case 0x70:
            if (type != 1) return -9;
            if ((rc = cl_rebase(file, flen, slide, seg_vmaddr, seg_fileoff, nseg, seg, off, cb, ctx))) return rc;
            if (!cl_uleb(&p, end, &n)) return -8; off += n + 8; break;
        case 0x80:
            if (type != 1) return -9;
            if (!cl_uleb(&p, end, &n) || !cl_uleb(&p, end, &skip)) return -8;
            for (uint64_t i = 0; i < n; i++) {
                if ((rc = cl_rebase(file, flen, slide, seg_vmaddr, seg_fileoff, nseg, seg, off, cb, ctx))) return rc;
                off += skip + 8;
            }
            break;
        default: return -10;
        }
    }
    return 0;
}
/* binds. `lazy`: the lazy table ends every entry with done, so done does not end the walk there. */
static int cl_walk_binds(uint64_t slide, const uint64_t *seg_vmaddr, unsigned nseg, const uint8_t *p, const uint8_t *end,
                         int lazy, a64_fixup_cb cb, void *ctx) {
    unsigned seg = 0, type = 1; uint64_t off = 0, n, skip; int64_t addend = 0; int ord = 0, weak = 0;
    const char *sym = 0;
#define CL_BIND() do { if (seg >= nseg) return -7; if (!sym) return -11; if (type != 1) return -9; \
        struct a64_fixup f; memset(&f, 0, sizeof f); f.at = seg_vmaddr[seg] + off + slide; f.target = (uint64_t)addend; \
        f.symbol = sym; f.lib_ordinal = ord; f.weak = weak; f.is_bind = 1; cb(&f, ctx); } while (0)
    while (p < end) {
        uint8_t op = *p & 0xf0, imm = *p & 0x0f; p++;
        switch (op) {
        case 0x00: if (!lazy) return 0; break;
        case 0x10: ord = imm; break;
        case 0x20: if (!cl_uleb(&p, end, &n)) return -8; ord = (int)n; break;
        case 0x30: ord = imm ? (int)(int8_t)(0xf0 | imm) : 0; break;      /* 0 self, -1 main program, -2 flat, -3 weak */
        case 0x40: {
            weak = imm & 1; sym = (const char *)p;
            const uint8_t *z = memchr(p, 0, (size_t)(end - p));
            if (!z) return -8;
            p = z + 1; break;
        }
        case 0x50: type = imm; break;
        case 0x60: if (!cl_sleb(&p, end, &addend)) return -8; break;
        case 0x70: seg = imm; if (!cl_uleb(&p, end, &off)) return -8; break;
        case 0x80: if (!cl_uleb(&p, end, &n)) return -8; off += n; break;
        case 0x90: CL_BIND(); off += 8; break;
        case 0xa0: CL_BIND(); if (!cl_uleb(&p, end, &n)) return -8; off += n + 8; break;
        case 0xb0: CL_BIND(); off += (uint64_t)imm * 8 + 8; break;
        case 0xc0:
            if (!cl_uleb(&p, end, &n) || !cl_uleb(&p, end, &skip)) return -8;
            for (uint64_t i = 0; i < n; i++) { CL_BIND(); off += skip + 8; }
            break;
        default: return -10;
        }
    }
#undef CL_BIND
    return 0;
}
static int cl_walk(const uint8_t *file, size_t flen, uint64_t slide, const uint64_t *seg_vmaddr,
                   const uint64_t *seg_fileoff, unsigned nseg, const struct dyld_info_cmd *di,
                   a64_fixup_cb cb, void *ctx) {
    int rc;
    if ((size_t)di->rebase_off + di->rebase_size > flen || (size_t)di->bind_off + di->bind_size > flen
        || (size_t)di->lazy_bind_off + di->lazy_bind_size > flen) return -3;
    if (di->rebase_size && (rc = cl_walk_rebases(file, flen, slide, seg_vmaddr, seg_fileoff, nseg,
            file + di->rebase_off, file + di->rebase_off + di->rebase_size, cb, ctx))) return rc;
    if (di->bind_size && (rc = cl_walk_binds(slide, seg_vmaddr, nseg, file + di->bind_off,
            file + di->bind_off + di->bind_size, 0, cb, ctx))) return rc;
    if (di->lazy_bind_size && (rc = cl_walk_binds(slide, seg_vmaddr, nseg, file + di->lazy_bind_off,
            file + di->lazy_bind_off + di->lazy_bind_size, 1, cb, ctx))) return rc;
    return 0;
}

int a64_walk_fixups_slide(const uint8_t *whole, size_t wlen, uint64_t slide, a64_fixup_cb cb, void *ctx) {
    size_t flen = 0;
    const uint8_t *file = thin_arm64(whole, wlen, &flen);
    if (!file) return -6;
    const struct mh64 *mh = (const struct mh64 *)file;
    if (mh->magic != 0xfeedfacf) return -1;

    const struct linkedit *cf = 0;
    const struct dyld_info_cmd *di = 0;
    uint64_t text_vmaddr = 0;
    uint64_t seg_vmaddr[64], seg_fileoff[64];
    unsigned nseg = 0;
    int have_text = 0;

    const struct lc *c = (const struct lc *)(file + sizeof *mh);
    for (uint32_t k = 0; k < mh->ncmds; k++) {
        if (c->cmd == LC_SEGMENT_64_) {
            const struct seg64 *sg = (const struct seg64 *)c;
            if (nseg < 64) { seg_vmaddr[nseg] = sg->vmaddr; seg_fileoff[nseg] = sg->fileoff; nseg++; }
            if (!have_text && !strcmp(sg->segname, "__TEXT")) { text_vmaddr = sg->vmaddr; have_text = 1; }
        } else if (c->cmd == LC_DYLD_CHAINED_FIXUPS_) {
            cf = (const struct linkedit *)c;
        } else if ((c->cmd == 0x22 || c->cmd == 0x80000022) && c->cmdsize >= sizeof(struct dyld_info_cmd)) {
            di = (const struct dyld_info_cmd *)c;
        }
        c = (const struct lc *)((const uint8_t *)c + c->cmdsize);
    }
    /* no chained fix-ups: the older opcode tables when the image has them, else nothing to do (-2) */
    if (!cf) return di ? cl_walk(file, flen, slide, seg_vmaddr, seg_fileoff, nseg, di, cb, ctx) : -2;
    if ((size_t)cf->dataoff + cf->datasize > flen) return -3;

    const uint8_t *blob = file + cf->dataoff;
    const uint32_t blobsz = cf->datasize;
    /* The blob's own three offsets were trusted. starts/imports/symbols are file fields inside
          LC_DYLD_CHAINED_FIXUPS, and every structure below is reached through them — so a bad one
          reads outside the mapped file and reports fixups that then get applied to guest memory. */
    if (blobsz < sizeof(struct cf_header)) return -7;
    const struct cf_header *h = (const struct cf_header *)blob;
    if (h->starts_offset + sizeof(struct cf_starts_in_image) > blobsz
        || h->imports_offset > blobsz || h->symbols_offset > blobsz) return -7;
    const struct cf_starts_in_image *si =
        (const struct cf_starts_in_image *)(blob + h->starts_offset);
    if ((uint64_t)h->starts_offset + 4 + 4 * (uint64_t)si->seg_count > blobsz) return -7;

    for (uint32_t sx = 0; sx < si->seg_count; sx++) {
        if (!si->seg_info_offset[sx]) continue;          /* this segment has no fixups */
        /* And the per-segment record, whose page_start[] array is page_count entries long: the
              record's own `size` field is what says the array is there. */
        uint64_t ssoff = (uint64_t)h->starts_offset + si->seg_info_offset[sx];
        if (ssoff + sizeof(struct cf_starts_in_segment) > blobsz) return -8;
        const struct cf_starts_in_segment *ss =
            (const struct cf_starts_in_segment *)(blob + ssoff);
        if (ssoff + 22 + 2 * (uint64_t)ss->page_count > blobsz || !ss->page_size) return -8;
        if (sx >= nseg) continue;
        unsigned fmt = ss->pointer_format;
        unsigned stride = is_arm64e_fmt(fmt) ? 8 : 4;

        for (uint16_t pg = 0; pg < ss->page_count; pg++) {
            uint16_t start = ss->page_start[pg];
            if (start == START_NONE) continue;
            /* the chain's first slot, as a file offset and as an unslid address */
            uint64_t off = ss->segment_offset + (uint64_t)pg * ss->page_size + start;
            /* The chain was bounded only by the file, not by its own segment. `next` is 11 or 12
                  bits out of the pointer itself, so a chain can walk out of the segment it belongs to
                  and keep going — every slot it then reports is applied to guest memory from the wrong
                  segment's base. The page extent the starts record itself declares is the limit. */
            uint64_t chain_end = ss->segment_offset + (uint64_t)ss->page_count * ss->page_size;
            for (;;) {
                if (off + 8 > chain_end) return -10;
                uint64_t fileo = seg_fileoff[sx] + (off - ss->segment_offset);
                if (fileo + 8 > flen) return -4;
                uint64_t raw;
                memcpy(&raw, file + fileo, 8);
                uint64_t at = seg_vmaddr[sx] + (off - ss->segment_offset);

                struct a64_fixup f;
                memset(&f, 0, sizeof f);
                f.at = at + slide;
                uint64_t next;

                if (is_arm64e_fmt(fmt)) {
                    int bind = (int)((raw >> 62) & 1);
                    int auth = (int)((raw >> 63) & 1);
                    next = (raw >> 51) & 0x7ff;
                    if (bind) {
                        /* USERLAND24 widens the ordinal to 24 bits; every other arm64e form uses 16 */
                        uint32_t ord = (fmt == PTR_ARM64E_USERLAND24)
                                     ? (uint32_t)(raw & 0xffffff) : (uint32_t)(raw & 0xffff);
                        const char *sym = 0; int64_t add = 0; int lib = 0, wk = 0;
                        if (!import_at(h, blob, blobsz, ord, &sym, &add, &lib, &wk)) return -5;
                        /* a non-auth bind carries its own 19-bit signed addend in the pointer */
                        if (!auth) {
                            uint64_t ab = (raw >> 32) & 0x7ffff;
                            if (fmt == PTR_ARM64E_USERLAND24) ab = (raw >> 32) & 0x7ffff;
                            int64_t sa = (int64_t)(ab << 45) >> 45;     /* sign-extend 19 bits */
                            add += sa;
                        }
                        f.is_bind = 1; f.symbol = sym; f.target = (uint64_t)add;
                        f.lib_ordinal = lib; f.weak = wk;
                    } else if (auth) {
                        /* an auth rebase's target is 32 bits and is always an offset from the image base */
                        f.target = slide + text_vmaddr + (raw & 0xffffffffULL);
                    } else {
                        uint64_t tgt = raw & 0x7ffffffffffULL;           /* 43 bits */
                        uint64_t high8 = (raw >> 43) & 0xff;
                        f.target = (rebase_is_offset(fmt) ? slide + text_vmaddr + tgt : tgt + slide)
                                 | (high8 << 56);
                    }
                } else {
                    int bind = (int)((raw >> 63) & 1);
                    next = (raw >> 51) & 0xfff;
                    if (bind) {
                        uint32_t ord = (uint32_t)(raw & 0xffffff);
                        const char *sym = 0; int64_t add = 0; int lib = 0, wk = 0;
                        if (!import_at(h, blob, blobsz, ord, &sym, &add, &lib, &wk)) return -5;
                        add += (int64_t)((raw >> 24) & 0xff);            /* 8-bit unsigned addend */
                        f.is_bind = 1; f.symbol = sym; f.target = (uint64_t)add;
                        f.lib_ordinal = lib; f.weak = wk;
                    } else {
                        uint64_t tgt = raw & 0xfffffffffULL;             /* 36 bits */
                        uint64_t high8 = (raw >> 36) & 0xff;
                        f.target = (rebase_is_offset(fmt) ? slide + text_vmaddr + tgt : tgt + slide)
                                 | (high8 << 56);
                    }
                }

                cb(&f, ctx);
                if (!next) break;
                off += next * stride;
            }
        }
    }
    return 0;
}

/* the original entry point, kept so existing callers and the dyld-comparison gate do not change */
int a64_walk_fixups(const uint8_t *whole, size_t wlen, a64_fixup_cb cb, void *ctx) {
    return a64_walk_fixups_slide(whole, wlen, 0, cb, ctx);
}
