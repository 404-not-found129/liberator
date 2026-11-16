// a64_eh.c — C++ exceptions thrown by native code land in the guest's own catch blocks.
//
// The class. An interpreted program calls the real x86 frameworks, and some of them throw C++ exceptions it
//    is written to catch (Security's CssmError, std:: exceptions out of libc++). The native unwinder walks x86 frames
//    only: the guest's arm64 frames live on the guest stack and are described by the guest's own __unwind_info, which
//    libunwind never sees. So the search phase found no handler and libc++abi called terminate. On macOS 27.2:
//    `systemkeychain -vfcC` → "terminating due to uncaught exception of type Security::CssmError …
//    CSSMERR_DL_DATASTORE_DOESNOT_EXIST" — the exception systemkeychain itself catches to create the keychain. The
//    same class kills CoreLocationAgent.
// The mechanism, both phases of the Itanium ABI, nothing approximated:
//    · every native call the interpreter makes goes through __arm2x86_eh_guard (eh_guard.ll), whose personality is
//      __arm2x86_eh_personality below. Phase 1 (search): it walks the guest frames of that interpreter activation with
//      the guest's compact unwind + LSDA and the native libc++abi's own can_catch. Guest handler → defer to the native
//      __gxx_personality_v0 on the guard's catch-all (HANDLER_FOUND); none → CONTINUE_UNWIND, exactly as before.
//    · phase 2 lands in the guard; the interpreter then runs the guest's landing pads (cleanups first, then the
//      handler) with x0 = the _Unwind_Exception and x1 = the selector, as libunwind would on arm64. A guest cleanup
//      ends in _Unwind_Resume, which the interpreter routes here to continue the guest walk.
// What it does not do (each is counted, never guessed): DWARF-mode guest frames, frameless mid-chain frames,
//    exception specifications (negative filters), typed @catch in objc-personality frames, and guest cleanups when
//    the handler is native code above this activation (the exception then unwinds natively as it always did).
// Layout constants are checked, not read from a header: a test program against the native x86_64 libc++abi
//    passes 11/11 — class at ue+0, exceptionType at ue-80, adjustedPtr at ue-8, object at ue+32,
//    type_info vtable slot 4 = can_catch(this, thrown, void*&), multiple-inheritance and pointer adjustment.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unwind.h>
#include "a64i.h"

extern FILE *a2x_errf(void);
extern int a2x_guest_image_at(uint64_t pc, const uint8_t **file, uint64_t *flen, uint64_t *slide);

#define A64_RET_SENTINEL 0x0000dead5a2cbeefULL
#define CXX_CLASS     0x434C4E47432B2B00ULL     /* "CLNGC++\0" */
#define CXX_DEP_CLASS 0x434C4E47432B2B01ULL     /* "CLNGC++\1" dependent (rethrow_exception) */

/* ---- guest image: segments of the arm64 slice, so __TEXT bytes are read from the file (code pages are sealed) ---- */
struct gimg { const uint8_t *file; uint64_t flen, slide, mh; uint64_t ui_addr, ui_size;
              struct { uint64_t vmaddr, vmsize, fileoff, filesize; } seg[16]; int nseg; };

static int gimg_for(uint64_t pc, struct gimg *g) {
    memset(g, 0, sizeof *g);
    if (!a2x_guest_image_at(pc, &g->file, &g->flen, &g->slide)) return 0;
    const uint8_t *f = g->file;
    if (g->flen < 32 || *(const uint32_t *)f != 0xfeedfacf) return 0;
    uint32_t ncmds = *(const uint32_t *)(f + 16), sz = *(const uint32_t *)(f + 20);
    uint64_t off = 32, end = 32 + (uint64_t)sz;
    if (end > g->flen) return 0;
    for (uint32_t i = 0; i < ncmds && off + 8 <= end; i++) {
        uint32_t cmd = *(const uint32_t *)(f + off), cs = *(const uint32_t *)(f + off + 4);
        if (cs < 8 || off + cs > end) return 0;
        if (cmd == 0x19 && cs >= 72) {                      /* LC_SEGMENT_64 */
            const uint8_t *s = f + off;
            uint64_t vmaddr = *(const uint64_t *)(s + 24), vmsize = *(const uint64_t *)(s + 32);
            uint64_t fo = *(const uint64_t *)(s + 40), fs = *(const uint64_t *)(s + 48);
            uint32_t nsect = *(const uint32_t *)(s + 64);
            if (!strncmp((const char *)s + 8, "__TEXT", 16)) g->mh = vmaddr + g->slide;
            if (g->nseg < 16) { g->seg[g->nseg].vmaddr = vmaddr; g->seg[g->nseg].vmsize = vmsize;
                                g->seg[g->nseg].fileoff = fo; g->seg[g->nseg].filesize = fs; g->nseg++; }
            for (uint32_t k = 0; k < nsect && 72 + (uint64_t)(k + 1) * 80 <= cs; k++) {
                const uint8_t *sc = s + 72 + k * 80;
                if (!strncmp((const char *)sc, "__unwind_info", 16) && !strncmp((const char *)sc + 16, "__TEXT", 16)) {
                    g->ui_addr = *(const uint64_t *)(sc + 32) + g->slide; g->ui_size = *(const uint64_t *)(sc + 40);
                }
            }
        }
        off += cs;
    }
    return g->mh != 0;
}
/* runtime address -> bytes in the file (only file-backed segment ranges) */
static const uint8_t *gptr(const struct gimg *g, uint64_t a, uint64_t n) {
    uint64_t u = a - g->slide;
    for (int i = 0; i < g->nseg; i++)
        if (u >= g->seg[i].vmaddr && u + n <= g->seg[i].vmaddr + g->seg[i].filesize) {
            uint64_t fo = g->seg[i].fileoff + (u - g->seg[i].vmaddr);
            return fo + n <= g->flen ? g->file + fo : 0;
        }
    return 0;
}
static int rd32(const struct gimg *g, uint64_t a, uint32_t *v) { const uint8_t *p = gptr(g, a, 4); if (!p) return 0; memcpy(v, p, 4); return 1; }
static int rd16(const struct gimg *g, uint64_t a, uint16_t *v) { const uint8_t *p = gptr(g, a, 2); if (!p) return 0; memcpy(v, p, 2); return 1; }

/* ---- compact unwind (arm64) ---- */
struct cu { uint64_t fstart, fend; uint32_t enc; uint64_t lsda, pers; };
static int cu_find(const struct gimg *g, uint64_t pc, struct cu *o) {
    memset(o, 0, sizeof *o);
    if (!g->ui_addr || g->ui_size < 28) return 0;
    uint64_t U = g->ui_addr;
    uint32_t h[7];
    for (int i = 0; i < 7; i++) if (!rd32(g, U + 4 * i, &h[i])) return 0;
    if (h[0] != 1 || h[6] < 2) return 0;
    uint32_t rel = (uint32_t)(pc - g->mh);
    uint64_t IX = U + h[5];
    uint32_t lo = 0, hi = h[6] - 1, fo;                     /* last entry is the end sentinel */
    if (!rd32(g, IX + 12 * (uint64_t)hi, &fo) || rel >= fo) return 0;
    while (hi - lo > 1) { uint32_t mid = (lo + hi) / 2; if (!rd32(g, IX + 12 * (uint64_t)mid, &fo)) return 0;
                          if (fo <= rel) lo = mid; else hi = mid; }
    uint32_t i1f, i1s, i1l, i2f, i2l;
    if (!rd32(g, IX + 12 * (uint64_t)lo, &i1f) || !rd32(g, IX + 12 * (uint64_t)lo + 4, &i1s) ||
        !rd32(g, IX + 12 * (uint64_t)lo + 8, &i1l) || !rd32(g, IX + 12 * (uint64_t)lo + 12, &i2f) ||
        !rd32(g, IX + 12 * (uint64_t)lo + 20, &i2l)) return 0;
    if (!i1s || rel < i1f) return 0;
    uint64_t P = U + i1s; uint32_t kind; uint16_t eoff, ecnt;
    if (!rd32(g, P, &kind) || !rd16(g, P + 4, &eoff) || !rd16(g, P + 6, &ecnt) || !ecnt) return 0;
    uint32_t fstart = 0, fend = i2f, enc = 0;
    if (kind == 2) {
        int found = -1;
        for (uint32_t k = 0; k < ecnt; k++) { uint32_t f; if (!rd32(g, P + eoff + 8 * k, &f)) return 0;
                                              if (f <= rel) found = (int)k; else break; }
        if (found < 0) return 0;
        if (!rd32(g, P + eoff + 8 * (uint64_t)found, &fstart) || !rd32(g, P + eoff + 8 * (uint64_t)found + 4, &enc)) return 0;
        if ((uint32_t)found + 1 < ecnt && !rd32(g, P + eoff + 8 * ((uint64_t)found + 1), &fend)) return 0;
    } else if (kind == 3) {
        uint16_t encoff, enccnt; if (!rd16(g, P + 8, &encoff) || !rd16(g, P + 10, &enccnt)) return 0;
        int found = -1; uint32_t e = 0;
        for (uint32_t k = 0; k < ecnt; k++) { if (!rd32(g, P + eoff + 4 * k, &e)) return 0;
                                              if ((e & 0xFFFFFF) + i1f <= rel) found = (int)k; else break; }
        if (found < 0) return 0;
        rd32(g, P + eoff + 4 * (uint64_t)found, &e);
        fstart = (e & 0xFFFFFF) + i1f;
        if ((uint32_t)found + 1 < ecnt) { uint32_t n; if (!rd32(g, P + eoff + 4 * ((uint64_t)found + 1), &n)) return 0; fend = (n & 0xFFFFFF) + i1f; }
        uint32_t ei = e >> 24;
        if (ei < h[2]) { if (!rd32(g, U + h[1] + 4 * (uint64_t)ei, &enc)) return 0; }
        else if (ei - h[2] < enccnt) { if (!rd32(g, P + encoff + 4 * (uint64_t)(ei - h[2]), &enc)) return 0; }
        else return 0;
    } else return 0;
    o->fstart = g->mh + fstart; o->fend = g->mh + fend; o->enc = enc;
    if (enc & 0x40000000) {                                 /* UNWIND_HAS_LSDA */
        for (uint64_t L = U + i1l; L + 8 <= U + i2l; L += 8) {
            uint32_t lf, lo2; if (!rd32(g, L, &lf) || !rd32(g, L + 4, &lo2)) return 0;
            if (lf == fstart) { o->lsda = g->mh + lo2; break; }
        }
        if (!o->lsda) return 0;
    }
    uint32_t pi = (enc >> 28) & 3;
    if (pi) {
        if (pi > h[4]) return 0;
        uint32_t po; if (!rd32(g, U + h[3] + 4 * (uint64_t)(pi - 1), &po)) return 0;
        o->pers = *(const uint64_t *)(uintptr_t)(g->mh + po);  /* the GOT slot the host bound: a native address */
    }
    return 1;
}

/* ---- guest frame state ---- */
struct gf { uint64_t x[31], sp, pc; uint64_t d[8]; };        /* d8..d15 low halves */
static uint64_t strip(uint64_t a) { return a & 0x00007FFFFFFFFFFFULL; }
static int step_frame(struct gf *f, uint32_t enc) {
    if ((enc & 0x0F000000) != 0x04000000) return 0;           /* FRAME only (see header) */
    uint64_t fp = f->x[29];
    if (!fp || (fp & 7) || fp < f->sp) return 0;
    uint64_t loc = fp - 8;
    static const uint32_t xb[5] = { 1, 2, 4, 8, 0x10 };
    for (int k = 0; k < 5; k++) if (enc & xb[k]) {
        f->x[19 + 2 * k] = *(uint64_t *)(uintptr_t)loc; loc -= 8;
        f->x[20 + 2 * k] = *(uint64_t *)(uintptr_t)loc; loc -= 8; }
    static const uint32_t db[4] = { 0x100, 0x200, 0x400, 0x800 };
    for (int k = 0; k < 4; k++) if (enc & db[k]) {
        f->d[2 * k] = *(uint64_t *)(uintptr_t)loc; loc -= 8;
        f->d[2 * k + 1] = *(uint64_t *)(uintptr_t)loc; loc -= 8; }
    uint64_t nfp = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
    /* the activation's return sentinel is a 48-bit value: stripping it to 47 bits hid the end of the walk */
    f->x[29] = nfp; f->x[30] = lr; f->sp = fp + 16; f->pc = lr == A64_RET_SENTINEL ? lr : strip(lr);
    return 1;
}

/* ---- DWARF pointer encodings in the LSDA ---- */
static int rd_uleb(const struct gimg *g, uint64_t *a, uint64_t *v) {
    uint64_t r = 0; int sh = 0; const uint8_t *p;
    do { if (!(p = gptr(g, *a, 1)) || sh > 63) return 0; r |= (uint64_t)(*p & 0x7f) << sh; sh += 7; (*a)++; } while (*p & 0x80);
    *v = r; return 1;
}
static int rd_sleb(const struct gimg *g, uint64_t *a, int64_t *v) {
    uint64_t r = 0; int sh = 0; const uint8_t *p;
    do { if (!(p = gptr(g, *a, 1)) || sh > 63) return 0; r |= (uint64_t)(*p & 0x7f) << sh; sh += 7; (*a)++; } while (*p & 0x80);
    if (sh < 64 && (*p & 0x40)) r |= ~0ULL << sh;
    *v = (int64_t)r; return 1;
}
static int enc_size(uint8_t e) { switch (e & 0x0f) { case 0: case 4: case 0xc: return 8; case 2: case 0xa: return 2; case 3: case 0xb: return 4; } return 0; }
static int rd_enc(const struct gimg *g, uint64_t *a, uint8_t e, uint64_t *v) {
    if (e == 0xff) { *v = 0; return 1; }
    uint64_t at = *a, r = 0; const uint8_t *p;
    switch (e & 0x0f) {
    case 0x0: case 0x4: if (!(p = gptr(g, at, 8))) return 0; memcpy(&r, p, 8); *a += 8; break;
    case 0x1: if (!rd_uleb(g, a, &r)) return 0; break;
    case 0x9: { int64_t s; if (!rd_sleb(g, a, &s)) return 0; r = (uint64_t)s; break; }
    case 0x2: { uint16_t u; if (!rd16(g, at, &u)) return 0; r = u; *a += 2; break; }
    case 0x3: { uint32_t u; if (!rd32(g, at, &u)) return 0; r = u; *a += 4; break; }
    case 0xa: { uint16_t u; if (!rd16(g, at, &u)) return 0; r = (uint64_t)(int64_t)(int16_t)u; *a += 2; break; }
    case 0xb: { uint32_t u; if (!rd32(g, at, &u)) return 0; r = (uint64_t)(int64_t)(int32_t)u; *a += 4; break; }
    case 0xc: if (!(p = gptr(g, at, 8))) return 0; memcpy(&r, p, 8); *a += 8; break;
    default: return 0;
    }
    if (r) {
        switch (e & 0x70) { case 0: break; case 0x10: r += at; break; default: return 0; }   /* absptr | pcrel */
        if (e & 0x80) r = *(const uint64_t *)(uintptr_t)r;    /* indirect: a GOT slot (mapped data) */
    }
    *v = r; return 1;
}

/* ---- the exception ---- */
/* can_catch returns bool: only AL is defined, the rest of eax is whatever the callee left. Read as int, a refusal
   looked like a match and catch(long) took a thrown int. */
typedef _Bool (*cc_t)(const void *, const void *, void **);
static const void *exc_type(void *ue) { return *(void *const *)((char *)ue - 80); }
static void *exc_obj(void *ue) {
    return *(uint64_t *)ue == CXX_DEP_CLASS ? *(void **)((char *)ue - 88) : (char *)ue + 32;
}
static int __arm2x86_guest_text(uint64_t a) {               /* is this inside a guest image */
    const uint8_t *f; uint64_t l, sl; return a2x_guest_image_at(a, &f, &l, &sl);
}

enum { LS_NONE = 0, LS_CLEANUP, LS_HANDLER, LS_TERMINATE, LS_UNKNOWN };
static unsigned long g_nunk;
static int eh_trace(void) { static int t = -1; if (t < 0) t = A2X_DBGENV("ARM2X86_TRACE_EH") ? 1 : 0; return t; }
static void unk(const char *why, uint64_t pc) {
    if (g_nunk++ < 16) fprintf(a2x_errf(), "[eh] guest walk stops (%s) at pc 0x%llx\n", why, (unsigned long long)pc);
}
/* One frame's LSDA against the exception: what libc++abi's scan_eh_tab decides, for an arm64 frame. */
static int lsda_scan(const struct gimg *g, const struct cu *c, uint64_t ip, void *ue, int typed,
                     uint64_t *lp_out, int64_t *sel_out, void **adj_out) {
    uint64_t a = c->lsda, lpstart = c->fstart, ttbase = 0, v; uint8_t e;
    const uint8_t *p;
    if (!(p = gptr(g, a, 1))) return LS_UNKNOWN; e = *p; a++;
    if (e != 0xff && !rd_enc(g, &a, e, &lpstart)) return LS_UNKNOWN;
    if (!(p = gptr(g, a, 1))) return LS_UNKNOWN; uint8_t tte = *p; a++;
    if (tte != 0xff) { if (!rd_uleb(g, &a, &v)) return LS_UNKNOWN; ttbase = a + v; }
    if (!(p = gptr(g, a, 1))) return LS_UNKNOWN; uint8_t cse = *p; a++;
    uint64_t cslen; if (!rd_uleb(g, &a, &cslen)) return LS_UNKNOWN;
    uint64_t cs = a, csend = a + cslen, acttab = csend;
    uint64_t off = ip - c->fstart;
    while (cs < csend) {
        uint64_t st, ln, lp, act;
        if (!rd_enc(g, &cs, cse, &st) || !rd_enc(g, &cs, cse, &ln) || !rd_enc(g, &cs, cse, &lp) || !rd_uleb(g, &cs, &act))
            return LS_UNKNOWN;
        if (off < st) break;                                  /* sorted: ip precedes every remaining entry */
        if (off >= st + ln) continue;
        if (!lp) return LS_NONE;                              /* covered, no landing pad: keep unwinding */
        *lp_out = lpstart + lp;
        if (!act) return LS_CLEANUP;
        uint64_t ar = acttab + act - 1; int cleanup = 0;
        for (int guard = 0; guard < 256; guard++) {
            int64_t ti, nx; uint64_t nxat;
            if (!rd_sleb(g, &ar, &ti)) return LS_UNKNOWN;
            nxat = ar;
            if (!rd_sleb(g, &ar, &nx)) return LS_UNKNOWN;
            if (ti > 0) {
                int es = enc_size(tte); if (!es || !ttbase) return LS_UNKNOWN;
                uint64_t ta = ttbase - (uint64_t)ti * (uint64_t)es, T, raw = 0;
                { uint64_t t2 = ta; uint8_t rawe = (uint8_t)(tte & 0x0f); if (!rd_enc(g, &t2, rawe, &raw)) return LS_UNKNOWN; }
                if (!rd_enc(g, &ta, tte, &T)) return LS_UNKNOWN;
                if (eh_trace()) fprintf(a2x_errf(), "[eh] ip 0x%llx action ti %lld raw 0x%llx type %p\n", (unsigned long long)ip,
                                        (long long)ti, (unsigned long long)raw, (void *)(uintptr_t)T);
                if (!raw) { *sel_out = ti; *adj_out = exc_obj(ue); return LS_HANDLER; }    /* catch (...) */
                if (!T) { unk("unbound type_info", ip); goto next_action; }
                if (typed) {
                    cc_t f = (*(cc_t *const *)(uintptr_t)T)[4];
                    if (__arm2x86_guest_text((uint64_t)(uintptr_t)f)) { unk("guest type_info vtable", ip); }
                    else { void *adj = exc_obj(ue);
                           if (f((const void *)(uintptr_t)T, exc_type(ue), &adj)) { *sel_out = ti; *adj_out = adj; return LS_HANDLER; }
                           /* arm64 non-unique RTTI: a guest's own weak copy of a type_info sets bit 63 of its name
                              pointer = "compare by string". x86 libc++abi compares pointers only, so the same type
                              from two images never matched. Exact type by name; base-class search stays pointer-based. */
                           uint64_t cn = *(const uint64_t *)(uintptr_t)(T + 8);
                           uint64_t tn = *(const uint64_t *)((const char *)exc_type(ue) + 8);
                           if ((cn >> 63) && !strcmp((const char *)(uintptr_t)(cn & ~(1ULL << 63)),
                                                     (const char *)(uintptr_t)(tn & ~(1ULL << 63)))) {
                               *sel_out = ti; *adj_out = exc_obj(ue); return LS_HANDLER; } }
                }
            next_action:;
            } else if (ti < 0) {
                unk("exception specification", ip);           /* treated as not matching */
            } else cleanup = 1;
            if (!nx) break;
            ar = nxat + (uint64_t)nx;
        }
        return cleanup ? LS_CLEANUP : LS_NONE;
    }
    return LS_TERMINATE;                                      /* not in the call-site table: libc++abi terminates */
}

/* Walk the guest frames of one activation. mode 0 = search (find the handler frame), 1 = land (stop at the first
   frame with a landing pad for this exception; the handler frame is known by its CFA). */
struct walk { uint64_t handler_cfa; int64_t sel; void *adj; uint64_t lp; struct gf at; int handler; };
static int guest_walk(struct gf f, void *ue, int mode, uint64_t handler_cfa, struct walk *w) {
    static void *gxx, *objc;
    if (!gxx) { gxx = dlsym(RTLD_DEFAULT, "__gxx_personality_v0"); objc = dlsym(RTLD_DEFAULT, "__objc_personality_v0"); }
    for (int depth = 0; depth < 1024; depth++) {
        if (f.pc == A64_RET_SENTINEL || !f.pc) return 0;     /* the activation's entry: no guest handler */
        struct gimg g; struct cu c;
        if (!gimg_for(f.pc - 1, &g)) { unk("pc outside the guest images", f.pc); return 0; }
        if (!cu_find(&g, f.pc - 1, &c)) { unk("no compact unwind entry", f.pc); return 0; }
        uint64_t cfa = f.x[29] + 16;
        if (c.pers && c.lsda) {
            int typed = c.pers == (uint64_t)(uintptr_t)gxx;
            if (!typed && c.pers != (uint64_t)(uintptr_t)objc) { unk("unknown personality", f.pc); return 0; }
            uint64_t lp = 0; int64_t sel = 0; void *adj = 0;
            int r = lsda_scan(&g, &c, f.pc - 1, ue, typed, &lp, &sel, &adj);
            if (r == LS_UNKNOWN) { unk("unreadable LSDA", f.pc); return 0; }
            if (r == LS_TERMINATE) return 0;
            if (mode == 0 && r == LS_HANDLER) { w->handler_cfa = cfa; w->sel = sel; w->adj = adj; return 1; }
            if (mode == 1 && (r == LS_CLEANUP || r == LS_HANDLER)) {
                w->lp = lp; w->at = f;
                w->handler = cfa == handler_cfa && r == LS_HANDLER;
                if (!w->handler) w->sel = 0;
                return 1;
            }
        }
        if (mode == 1 && cfa == handler_cfa) { unk("handler frame lost its landing pad", f.pc); return 0; }
        if ((c.enc & 0x0F000000) == 0x03000000) { unk("DWARF-mode frame", f.pc); return 0; }
        if (!step_frame(&f, c.enc)) { unk("frameless or unencoded frame", f.pc); return 0; }
    }
    return 0;
}

/* ---- per-thread state ---- */
struct arec { uintptr_t marker; const a64 *st; };
static __thread struct arec t_rec[64];
static __thread int t_nrec;
struct pend { void *ue; uint64_t handler_cfa; int64_t sel; void *adj; };
static __thread struct pend t_pend[8];
static __thread int t_npend;

void a2x_eh_push(const a64 *st) {
    uintptr_t m = (uintptr_t)st;
    while (t_nrec && t_rec[t_nrec - 1].marker <= m) t_nrec--;   /* records deeper than us are dead frames */
    if (t_nrec < 64) { t_rec[t_nrec].marker = m; t_rec[t_nrec].st = st; t_nrec++; }
}
void a2x_eh_pop(const a64 *st) {
    while (t_nrec && t_rec[t_nrec - 1].marker <= (uintptr_t)st) t_nrec--;
}
static struct pend *pend_for(void *ue) { for (int i = 0; i < t_npend; i++) if (t_pend[i].ue == ue) return &t_pend[i]; return 0; }
static void pend_drop(struct pend *p) { int i = (int)(p - t_pend); t_pend[i] = t_pend[--t_npend]; }

static struct gf gf_from(const a64 *s) {
    struct gf f; memcpy(f.x, s->x, sizeof f.x); f.sp = s->sp;
    f.pc = s->x[30] == A64_RET_SENTINEL ? A64_RET_SENTINEL : strip(s->x[30]);
    for (int k = 0; k < 8; k++) memcpy(&f.d[k], s->v[8 + k], 8);
    return f;
}

typedef _Unwind_Reason_Code (*pers_t)(int, _Unwind_Action, uint64_t, struct _Unwind_Exception *, struct _Unwind_Context *);
_Unwind_Reason_Code __arm2x86_eh_personality(int version, _Unwind_Action actions, uint64_t cls,
                                              struct _Unwind_Exception *ue, struct _Unwind_Context *ctx) {
    static pers_t gxx;
    if (!gxx) gxx = (pers_t)dlsym(RTLD_DEFAULT, "__gxx_personality_v0");
    if (!gxx || version != 1 || (cls != CXX_CLASS && cls != CXX_DEP_CLASS)) return _URC_CONTINUE_UNWIND;
    if (actions & _UA_SEARCH_PHASE) {
        uintptr_t cfa = (uintptr_t)_Unwind_GetCFA(ctx);
        const struct arec *r = 0;
        for (int i = t_nrec - 1; i >= 0; i--) if (t_rec[i].marker >= cfa) { r = &t_rec[i]; break; }
        if (!r) return _URC_CONTINUE_UNWIND;
        struct walk w; memset(&w, 0, sizeof w);
        if (!guest_walk(gf_from(r->st), ue, 0, 0, &w)) return _URC_CONTINUE_UNWIND;
        struct pend *p = pend_for(ue);
        if (!p) { if (t_npend == 8) return _URC_CONTINUE_UNWIND; p = &t_pend[t_npend++]; }
        p->ue = ue; p->handler_cfa = w.handler_cfa; p->sel = w.sel; p->adj = w.adj;
        return gxx(version, actions, cls, ue, ctx);           /* the guard's catch-all: HANDLER_FOUND */
    }
    /* A non-handler guard handed to __gxx_personality_v0 in phase 2 has its catch-all installed anyway,
       so a throw meant for an outer activation would land in the inner one. The guard has
       no cleanups: outside the handler frame it is transparent. */
    if (!(actions & _UA_HANDLER_FRAME)) return _URC_CONTINUE_UNWIND;
    return gxx(version, actions, cls, ue, ctx);
}

/* Run the next guest landing pad for ue, starting from frame f. Returns 1 with *s ready to run it. */
static int land_from(a64 *s, struct gf f, void *ue) {
    struct pend *p = pend_for(ue);
    if (!p) return 0;
    struct walk w; memset(&w, 0, sizeof w); w.sel = p->sel;
    if (!guest_walk(f, ue, 1, p->handler_cfa, &w)) return 0;
    for (int r = 19; r <= 30; r++) s->x[r] = w.at.x[r];
    s->sp = w.at.sp;
    for (int k = 0; k < 8; k++) memcpy(s->v[8 + k], &w.at.d[k], 8);
    s->x[0] = (uint64_t)(uintptr_t)ue;
    s->x[1] = (uint64_t)w.sel;
    s->pc = w.lp;
    if (w.handler) { *(void **)((char *)ue - 8) = p->adj; pend_drop(p); }   /* what __cxa_begin_catch returns */
    static int said;
    if (said++ < 8)
        fprintf(a2x_errf(), "[eh] native C++ exception -> guest %s at 0x%llx (selector %lld)\n",
                w.handler ? "catch" : "cleanup", (unsigned long long)w.lp, (long long)w.sel);
    return 1;
}
int a2x_eh_land(a64 *s, void *ue) { return land_from(s, gf_from(s), ue); }

/* A guest cleanup pad calls _Unwind_Resume(ue): continue the guest walk from the frame that called it. */
int a2x_eh_is_resume(unsigned long tgt) {
    static unsigned long r;
    if (!r) r = (unsigned long)(uintptr_t)dlsym(RTLD_DEFAULT, "_Unwind_Resume");
    return r && tgt == r;
}
int a2x_eh_resume(a64 *s) {
    void *ue = (void *)(uintptr_t)s->x[0];
    if (!pend_for(ue)) return 0;
    if (!land_from(s, gf_from(s), ue)) {
        fprintf(a2x_errf(), "[eh] _Unwind_Resume: no further guest landing pad for a handler found in phase 1\n");
        a2x_stop("exception unwinding: no further landing pad", ""); abort();
    }
    return 1;
}

/* Phase 1 claimed a guest handler and the walk to it failed: a bug of ours, never a silent continue. */
void a2x_eh_fatal(void *ue) {
    fprintf(a2x_errf(), "[eh] FATAL: guest handler found in phase 1 but no landing pad reached (ue %p)\n", ue);
    a2x_stop("exception unwinding: the handler was not reached", ""); abort();
}
