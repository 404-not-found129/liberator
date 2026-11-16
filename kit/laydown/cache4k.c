/* cache4k - write a copy of a dyld shared cache file whose code signature hashes 4 KB pages.
 *
 * macOS 27 ships its x86_64 dyld cache for machines with 16 KB pages: the code directory embedded in each cache
 * file has page size 2^14. The Intel kernel accepts only 2^12 and validates what it maps in 4 KB pages, so the
 * files as shipped cannot be mapped on an Intel machine. The signature is ad hoc (it carries no certificate), so
 * hashing the same bytes in 4 KB pages changes the granularity of the hashes and nothing else.
 *
 *   cache4k <input cache file> <output file>     write the 4 KB form; exit 0 only when the output is complete
 *   cache4k --info <cache file>                  print: page shift, code slots, code limit, signature size
 *
 * A cache file keeps the offset and size of its signature at 0x28 and 0x30 (little-endian, 64 bit). The signature
 * is a superblob: magic, length, count, then (type, offset) pairs. Type 0 is the code directory: an 88-byte
 * big-endian header, the identifier, the special-slot hashes, then one SHA-256 per page of [0, code limit).
 * The output is the input's bytes up to the signature, then a new superblob: the code directory with its header,
 * identifier and special slots kept and the page hashes redone, followed by every other blob unchanged, then
 * zeros up to a 4 KB boundary, as the shipped files are padded. The size at 0x30 becomes the padded size, and the
 * first page is hashed with that new size in place, so the hashes describe the output file exactly.
 * An input that already has 4 KB pages is copied unchanged. The output is written beside the target under a
 * temporary name and renamed when complete. */
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAGIC_SUPERBLOB 0xfade0cc0u
#define MAGIC_CODEDIR   0xfade0c02u

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint64_t le64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static int fail(const char *what, const char *path) { fprintf(stderr, "cache4k: %s: %s\n", path, what); return 1; }

static int read_at(int fd, uint64_t off, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, (off_t)(off + got));
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}
static int write_all(int fd, const void *buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t r = write(fd, (const char *)buf + put, n - put);
        if (r <= 0) return -1;
        put += (size_t)r;
    }
    return 0;
}

struct sig { uint64_t off, size, file_size; uint8_t *blob; uint32_t count, cd_off, cd_len, hash_off, n_special, n_code;
             uint64_t code_limit; uint8_t hash_size, hash_type, page_shift; };

static int load(int fd, const char *path, struct sig *s) {
    uint8_t h[0x38]; struct stat st;
    if (fstat(fd, &st) || read_at(fd, 0, h, sizeof h)) return fail("cannot read the header", path);
    if (memcmp(h, "dyld_v1", 7)) return fail("not a dyld shared cache file", path);
    s->file_size = (uint64_t)st.st_size; s->off = le64(h + 0x28); s->size = le64(h + 0x30);
    if (s->size < 12 || s->size > (64u << 20) || s->off > s->file_size || s->off + s->size > s->file_size)
        return fail("the signature lies outside the file", path);
    s->blob = malloc((size_t)s->size);
    if (!s->blob || read_at(fd, s->off, s->blob, (size_t)s->size)) return fail("cannot read the signature", path);
    if (be32(s->blob) != MAGIC_SUPERBLOB) return fail("the signature is not a superblob", path);
    uint32_t total = be32(s->blob + 4); s->count = be32(s->blob + 8);
    if (total > s->size || s->count == 0 || s->count > 64 || 12 + 8ull * s->count > total) return fail("bad superblob", path);
    s->cd_off = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        uint32_t type = be32(s->blob + 12 + 8 * i), off = be32(s->blob + 16 + 8 * i);
        if (off + 8ull > total) return fail("a blob lies outside the superblob", path);
        if (type == 0) s->cd_off = off;
    }
    if (!s->cd_off) return fail("the signature has no code directory", path);
    const uint8_t *cd = s->blob + s->cd_off;
    s->cd_len = be32(cd + 4);
    if (be32(cd) != MAGIC_CODEDIR || s->cd_len < 44 || s->cd_off + (uint64_t)s->cd_len > total) return fail("bad code directory", path);
    s->hash_off = be32(cd + 16); s->n_special = be32(cd + 24); s->n_code = be32(cd + 28); s->code_limit = be32(cd + 32);
    s->hash_size = cd[36]; s->hash_type = cd[37]; s->page_shift = cd[39];
    if (s->hash_size != 32 || s->hash_type != 2) return fail("the code directory does not use SHA-256", path);
    if (s->hash_off > s->cd_len || s->hash_off + 32ull * s->n_code > s->cd_len) return fail("the hash table lies outside the code directory", path);
    if (s->code_limit == 0 || s->code_limit > s->off) return fail("the code limit is not inside the signed part of the file", path);
    return 0;
}

int main(int argc, char **argv) {
    struct sig s; memset(&s, 0, sizeof s);
    if (argc == 3 && !strcmp(argv[1], "--info")) {
        int fd = open(argv[2], O_RDONLY);
        if (fd < 0) return fail("cannot open", argv[2]);
        if (load(fd, argv[2], &s)) return 1;
        printf("page-shift %u code-slots %u code-limit %llu signature-size %llu\n", s.page_shift, s.n_code,
               (unsigned long long)s.code_limit, (unsigned long long)s.size);
        return 0;
    }
    if (argc != 3 || argv[1][0] == '-') { fprintf(stderr, "usage: cache4k <input cache file> <output file>\n       cache4k --info <cache file>\n"); return 2; }
    const char *in = argv[1], *out = argv[2];
    int fd = open(in, O_RDONLY);
    if (fd < 0) return fail("cannot open", in);
    if (load(fd, in, &s)) return 1;

    /* the new signature */
    uint8_t *sb = NULL; uint64_t sb_len = 0, sb_pad = 0;
    int keep = s.page_shift == 12;
    if (!keep) {
        if (s.page_shift != 14) return fail("the page size is neither 16 KB nor 4 KB", in);
        uint64_t n_code = (s.code_limit + 4095) / 4096;
        uint64_t cd_len = s.hash_off + 32 * n_code, others = 0;
        for (uint32_t i = 0; i < s.count; i++) {
            uint32_t type = be32(s.blob + 12 + 8 * i), off = be32(s.blob + 16 + 8 * i);
            if (type != 0) others += be32(s.blob + off + 4);
        }
        sb_len = 12 + 8ull * s.count + cd_len + others;
        sb_pad = (sb_len + 4095) & ~4095ull;
        if (n_code > 0xffffffffull || sb_pad > 0xffffffffull || !(sb = calloc(1, (size_t)sb_pad))) return fail("the new signature is too large", in);
        put32(sb, MAGIC_SUPERBLOB); put32(sb + 4, (uint32_t)sb_len); put32(sb + 8, s.count);
        uint8_t *cd = sb + 12 + 8 * s.count;
        memcpy(cd, s.blob + s.cd_off, s.hash_off);            /* header, identifier, special slots */
        put32(cd + 4, (uint32_t)cd_len); put32(cd + 28, (uint32_t)n_code); cd[39] = 12;
        uint8_t page[4096];
        for (uint64_t i = 0, off = 0; i < n_code; i++, off += 4096) {
            size_t n = (size_t)(s.code_limit - off < 4096 ? s.code_limit - off : 4096);
            if (read_at(fd, off, page, n)) return fail("short read while hashing", in);
            if (i == 0) { if (n < 0x38) return fail("the code limit is inside the header", in);
                          for (int b = 0; b < 8; b++) page[0x30 + b] = (uint8_t)(sb_pad >> (8 * b)); }
            CC_SHA256(page, (CC_LONG)n, cd + s.hash_off + 32 * i);
        }
        /* the index: the code directory first, then the other blobs in their original order */
        uint32_t at = 12 + 8 * s.count, slot = 0;
        put32(sb + 12, 0); put32(sb + 16, at); slot = 1; at += (uint32_t)cd_len;
        for (uint32_t i = 0; i < s.count; i++) {
            uint32_t type = be32(s.blob + 12 + 8 * i), off = be32(s.blob + 16 + 8 * i);
            if (type == 0) continue;
            uint32_t len = be32(s.blob + off + 4);
            if (off + (uint64_t)len > s.size || at + (uint64_t)len > sb_len) return fail("a blob's length is wrong", in);
            memcpy(sb + at, s.blob + off, len);
            put32(sb + 12 + 8 * slot, type); put32(sb + 16 + 8 * slot, at); slot++; at += len;
        }
        if (at != sb_len) return fail("internal: signature size mismatch", in);
    }

    /* the output: the input up to its signature, then the signature */
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.cache4k-part", out) >= (int)sizeof tmp) return fail("path too long", out);
    int od = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (od < 0) return fail("cannot create", tmp);
    static uint8_t buf[1 << 20];
    uint64_t body = keep ? s.file_size : s.off;
    for (uint64_t off = 0; off < body;) {
        size_t n = (size_t)(body - off < sizeof buf ? body - off : sizeof buf);
        if (read_at(fd, off, buf, n)) { unlink(tmp); return fail("short read while copying", in); }
        if (off == 0 && !keep) for (int i = 0; i < 8; i++) buf[0x30 + i] = (uint8_t)(sb_pad >> (8 * i));
        if (write_all(od, buf, n)) { unlink(tmp); return fail("cannot write", tmp); }
        off += n;
    }
    if (!keep && write_all(od, sb, (size_t)sb_pad)) { unlink(tmp); return fail("cannot write", tmp); }
    if (fsync(od) || close(od)) { unlink(tmp); return fail("cannot finish", tmp); }

    /* read the result back before it gets its name */
    struct sig r; memset(&r, 0, sizeof r);
    int rd = open(tmp, O_RDONLY);
    if (rd < 0 || load(rd, tmp, &r) || r.page_shift != 12 || r.code_limit != s.code_limit
        || r.n_code != (uint32_t)((s.code_limit + 4095) / 4096)) { unlink(tmp); return fail("the written file does not read back as a 4 KB cache", out); }
    close(rd);
    if (rename(tmp, out)) { unlink(tmp); return fail("cannot rename into place", out); }
    return 0;
}
