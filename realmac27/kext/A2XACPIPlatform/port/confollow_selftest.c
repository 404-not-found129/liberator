// confollow_selftest.c - offline arms for confollow.h. Build: cc -O1 -Wall -I<port> confollow_selftest.c && ./a.out
// Exit 0 = every arm passed AND every negative control (the OLD policy) failed where it must.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "confollow.h"

// ---- fake config space: buses 0..7 ----
static uint32_t cfg[8][32][8][16];
static uint32_t rdf(void *c, uint8_t b, uint8_t d, uint8_t f, uint8_t r)
{
    (void)c;
    if (b >= 8 || d >= 32 || f >= 8 || r >= 64) return 0xffffffffu;
    return cfg[b][d][f][r / 4];
}
static void wr(uint8_t b, uint8_t d, uint8_t f, uint8_t r, uint32_t v) { cfg[b][d][f][r / 4] = v; }
static void absent_all(void) { memset(cfg, 0xff, sizeof cfg); }

// An Arrow Lake desktop as measured under 26:
// RP01 0:6.0 -> GPU 2:0.0, BAR0 0x80000000 (64M), BAR1 64-bit pf VRAM, BAR3 64-bit pf 32 MB.
#define FB_OFF 0x0ULL
static void pc_layout(uint64_t bar1, uint64_t bar3, uint8_t sec, int gpuDecode, int rpDecode)
{
    absent_all();
    wr(0, 0, 0, 0x00, 0x7d358086); wr(0, 0, 0, 0x0c, 0x00000000);                // host bridge MC__
    wr(0, 6, 0, 0x00, 0xae4d8086); wr(0, 6, 0, 0x0c, 0x00010000);                 // RP01, type 1
    wr(0, 6, 0, 0x04, rpDecode ? 0x6 : 0x0);
    wr(0, 6, 0, 0x18, (uint32_t)sec << 16 | (uint32_t)sec << 8 | 0);
    wr(0, 6, 0, 0x20, 0x84f08000);                                                // mem 0x80000000-0x84ffffff
    uint64_t lo = bar1 < bar3 ? bar1 : bar3, hi = (bar3 + (32ULL << 20) - 1) > (bar1 + (8ULL << 30) - 1)
                                                  ? bar3 + (32ULL << 20) - 1 : bar1 + (8ULL << 30) - 1;
    wr(0, 6, 0, 0x24, (uint32_t)(((hi >> 16) & 0xfff0) << 16) | (uint32_t)((lo >> 16) & 0xfff0) | 1);
    wr(0, 6, 0, 0x28, (uint32_t)(lo >> 32)); wr(0, 6, 0, 0x2c, (uint32_t)(hi >> 32));
    wr(sec, 0, 0, 0x00, 0x2d0510de); wr(sec, 0, 0, 0x0c, 0x00800000);             // GPU, multifunction
    wr(sec, 0, 0, 0x04, gpuDecode ? 0x6 : 0x0);
    wr(sec, 0, 0, 0x10, 0x80000000);
    wr(sec, 0, 0, 0x14, (uint32_t)bar1 | 0xc); wr(sec, 0, 0, 0x18, (uint32_t)(bar1 >> 32));
    wr(sec, 0, 0, 0x1c, (uint32_t)bar3 | 0xc); wr(sec, 0, 0, 0x20, (uint32_t)(bar3 >> 32));
    wr(sec, 0, 1, 0x00, 0x22eb10de); wr(sec, 0, 1, 0x0c, 0x00800000);             // HDMI audio
    wr(sec, 0, 1, 0x10, 0x85080000);
    wr(0, 0x1f, 0, 0x00, 0x7f068086); wr(0, 0x1f, 0, 0x0c, 0x00800000);           // LPC
}

// ---- fake xnu console: what is drawn, where ----
struct fakepe { int on; uint64_t base; int bases, enables; };
static struct fakepe pe;
static uint64_t truth;          // where the screen really is right now (0 = window closed / moving)
static int drewStale;           // the console was ON while truth != pe.base
static void check(void) { if (pe.on && pe.base != truth) drewStale = 1; }
static void xnu(unsigned op, uint64_t base)
{
    if (op == CF_OP_DISABLE) pe.on = 0;
    if (op == CF_OP_ENABLE) { pe.on = 1; pe.enables++; }
    if (op == CF_OP_BASE) { pe.base = base & ~3ULL; pe.bases++; }
    check();
}

// policy under test: NEW = confollow.h, OLD = forward everything (Apple's nesting-free path, the 5167dab4 build),
// LIVE = an earlier diagnostic (drop disables)
enum { NEW, OLD, LIVE };
static int policy;
static struct cf_loc gl;
static struct cf_state st;
static int where_cb(void *c, uint64_t *p) { (void)c; return cf_where(rdf, NULL, &gl, p); }
static void op(unsigned o, uint64_t base)
{
    if (policy != NEW) {
        if (policy == LIVE && o == CF_OP_DISABLE) return;
        xnu(o, base); return;
    }
    uint64_t nb = 0;
    unsigned a = cf_step(&st, o, base, where_cb, NULL, &nb);
    if (a & CF_BASE_FIRST) xnu(CF_OP_BASE, nb | 1);
    if (a & CF_FORWARD) xnu(o, base);
}
static void boot(uint64_t fb)
{
    memset(&pe, 0, sizeof pe); pe.on = 1; pe.base = fb; truth = fb; drewStale = 0;
    memset(&st, 0, sizeof st); st.cur = fb; st.live = (policy == LIVE);
    cf_locate(rdf, NULL, 0, fb, &gl);
}
// The boot pass of one host bridge that moves the GPU (PC00). `hit`: the configurator reports the move (op 8).
#define OLD1 0xA000000000ULL
#define OLD3 0xA200000000ULL
#define NEW1 0x3FC00000000ULL
#define NEW3 0x3FE00000000ULL
static void pc00_move_begin(void) { op(CF_OP_DISABLE, 0); truth = 0; check(); }
static void pc00_move_end(int hit, uint8_t newsec, int decode)
{
    pc_layout(NEW1, NEW3, newsec, decode, 1);
    truth = decode ? NEW1 + FB_OFF : 0;
    check();
    if (hit) op(CF_OP_BASE, (NEW1 + FB_OFF) | 1);
    op(CF_OP_ENABLE, 0);
}
static void pc02_pass(void) { op(CF_OP_DISABLE, 0); op(CF_OP_ENABLE, 0); }

static int fails;
#define ARM(name, cond) do { int _ok = (cond); printf("%s %s\n", _ok ? "ok  " : "FAIL", name); if (!_ok) fails++; } while (0)

// each scenario returns 1 when the screen ends on, at the real address, and never drew at a stale one
static int scen_interleaved(int hit)
{
    pc_layout(OLD1, OLD3, 2, 1, 1); boot(OLD1 + FB_OFF);
    pc00_move_begin();
    pc02_pass();                     // PC02's configurator finishes while PC00 is still moving the GPU
    pc00_move_end(hit, 3, 1);
    return !drewStale && pe.on && pe.base == truth;
}
static int scen_nohit(void)
{
    pc_layout(OLD1, OLD3, 2, 1, 1); boot(OLD1 + FB_OFF);
    pc00_move_begin(); pc00_move_end(0, 3, 1);
    return !drewStale && pe.on && pe.base == truth;
}

int main(void)
{
    struct cf_loc l; uint64_t p = 0;

    pc_layout(OLD1, OLD3, 2, 1, 1);
    int found = cf_locate(rdf, NULL, 0, OLD1 + 0x1000, &l);
    ARM("the boot screen is found in the GPU's 64-bit BAR1 (index 1) behind root port 0:6.0",
        found && l.viaBridge && l.bdev == 6 && l.dev == 0 && l.fn == 0 && l.bar == 1 && l.off == 0x1000);
    ARM("the 32 MB BAR3 right above BAR1 is not mistaken for the screen's BAR (fb inside BAR3 picks BAR3)",
        cf_locate(rdf, NULL, 0, OLD3 + 0x10, &l) && l.bar == 3 && l.off == 0x10);
    cf_locate(rdf, NULL, 0, OLD1 + 0x1000, &l);
    pc_layout(NEW1, NEW3, 3, 1, 1);
    ARM("after the boot pass moves BAR1 to 0x3FC00000000 AND renumbers the GPU bus 2->3, where() follows it",
        cf_where(rdf, NULL, &l, &p) == 1 && p == NEW1 + 0x1000);
    pc_layout(NEW1, NEW3, 3, 0, 1);
    ARM("GPU memory decode off after the pass: where() says not decoding (0), not an address", cf_where(rdf, NULL, &l, &p) == 0);
    pc_layout(NEW1, NEW3, 3, 1, 0);
    ARM("root port not forwarding memory: where() says not decoding (0)", cf_where(rdf, NULL, &l, &p) == 0);
    pc_layout(OLD1, OLD3, 2, 1, 1);
    for (int r = 0; r < 64; r += 4) wr(0, 6, 0, (uint8_t)r, 0xffffffffu);        // a2x_pcihide3=0x600: slot reads empty
    ARM("masked root port (reads all ones): nothing is tracked", !cf_locate(rdf, NULL, 0, OLD1, &l) && cf_where(rdf, NULL, &l, &p) == -1);

    // ---- policy arms; each one also runs against the old policy, which MUST fail it ----
    policy = NEW;
    ARM("PC02's enable inside PC00's move does NOT turn the screen on (nested), and it ends at the new base (with op 8)",
        scen_interleaved(1));
    ARM("the configurator moved the BAR and sent NO base change: we deliver it before the enable",
        scen_nohit());
    ARM("both at once: interleaved AND no base change", scen_interleaved(0));
    pc_layout(OLD1, OLD3, 2, 1, 1); boot(OLD1); pc00_move_begin(); pc00_move_end(1, 3, 1);
    ARM("the configurator's own base change is trusted: exactly one base change reaches xnu", pe.bases == 1 && pe.base == NEW1);
    pc_layout(OLD1, OLD3, 2, 1, 1); boot(OLD1); pc00_move_begin(); pc00_move_end(0, 3, 0);
    ARM("GPU not decoding after the pass: the screen stays OFF instead of drawing into nothing", !pe.on && !drewStale);
    pc_layout(OLD1, OLD3, 2, 1, 1); boot(OLD1); op(CF_OP_DISABLE, 0);
    ARM("disable is honored (an earlier diagnostic dropped it)", !pe.on);
    pc_layout(OLD1, OLD3, 2, 1, 1); boot(OLD1); op(CF_OP_ENABLE, 0);
    ARM("an enable nobody disabled through us (NVRMFB error path) is forwarded", pe.on && pe.enables == 1);
    pc_layout(OLD1, OLD3, 2, 1, 1);
    for (int r = 0; r < 64; r += 4) wr(0, 6, 0, (uint8_t)r, 0xffffffffu);
    boot(OLD1); op(CF_OP_DISABLE, 0); op(CF_OP_ENABLE, 0);
    ARM("masked config (today's working boot): disable/enable pass through unchanged, screen on at the boot base",
        pe.on && pe.base == OLD1 && pe.bases == 0);

    // ---- negative controls: the arms above must FIRE on the old code ----
    policy = OLD;
    ARM("control: OLD policy (5167dab4, nesting-free) FAILS the interleaved arm", !scen_interleaved(1));
    ARM("control: OLD policy FAILS the no-base-change arm", !scen_nohit());
    policy = LIVE;
    ARM("control: LIVE policy (earlier diagnostic, disable dropped) FAILS the interleaved arm", !scen_interleaved(1));
    pc_layout(OLD1, OLD3, 2, 1, 1); boot(OLD1); op(CF_OP_DISABLE, 0);
    ARM("control: LIVE policy FAILS the disable-honored arm", pe.on);

    printf("%s: %d failed\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
