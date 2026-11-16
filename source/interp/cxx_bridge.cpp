// cxx_bridge.cpp — libc++ functions whose return the generic native adapter cannot carry.
// Audited over the 97 libc++ functions a large C++ game imports. Two shapes break the register-mapping adapter:
//   - a result returned through memory (std::string, std::locale: non-trivially-copyable): AAPCS64 passes the result pointer in
//     x8, x86-64 SysV as a hidden first argument (rdi, `this` then in rsi);
//   - a 16-byte value returned in two registers (string_view): x0+x1 here, rax+rdx there — the adapter keeps only x0.
// Each handler is a full-register-file bind: arm64 x0..x8 arrive in __arm2x86_rf (objc_bridge.c), the native function is
//    called through a typed x86 pointer (dlsym of the same mangled symbol), and the result goes back the arm64 way.
//    std::string / std::locale objects are libc++'s own layout on both slices (same ABI v1, 64-bit, little-endian).
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" __thread unsigned char __arm2x86_rf[208];
#define RX(i) (((uint64_t *)__arm2x86_rf)[i])
static void *need(const char *sym) {
    void *p = dlsym(RTLD_DEFAULT, sym);
    if (!p) { fprintf(stderr, "[cxx_bridge] %s not found in the native libc++\n", sym); a2x_stop("native function not found", sym); abort(); }
    return p;
}
struct SV { const char *p; uint64_t n; };
// sret, member const: ret(x8) = this(x0)->f()
#define SRET_MEMBER(NAME, SYM) extern "C" void NAME(void) { static void *(*f)(void *, void *); \
    if (!f) f = (void *(*)(void *, void *))need(SYM); f((void *)RX(8), (void *)RX(0)); RX(0) = RX(8); }
// two-register return, member const: x0,x1 = this(x0)->f()
#define SV_MEMBER(NAME, SYM) extern "C" void NAME(void) { static SV (*f)(void *); \
    if (!f) f = (SV (*)(void *))need(SYM); SV r = f((void *)RX(0)); RX(0) = (uint64_t)r.p; RX(1) = r.n; }
SRET_MEMBER(__arm2x86_cxx_stringbuf_str, "_ZNKSt3__115basic_stringbufIcNS_11char_traitsIcEENS_9allocatorIcEEE3strEv")
SRET_MEMBER(__arm2x86_cxx_ios_getloc, "_ZNKSt3__18ios_base6getlocEv")
SV_MEMBER(__arm2x86_cxx_path_parent_path, "_ZNKSt3__14__fs10filesystem4path13__parent_pathEv")
SV_MEMBER(__arm2x86_cxx_path_stem, "_ZNKSt3__14__fs10filesystem4path6__stemEv")
// std::to_string(unsigned): ret(x8), value in w0
extern "C" void __arm2x86_cxx_to_string_u(void) {
    static void *(*f)(void *, unsigned);
    if (!f) f = (void *(*)(void *, unsigned))need("_ZNSt3__19to_stringEj");
    f((void *)RX(8), (unsigned)RX(0)); RX(0) = RX(8);
}
// SLSDisplayGetTiming RETURNS A struct through x8, and the generic adapter drops it. macOS 27's WindowManager
//    calls it at three sites, every one `add x8, sp, #N / mov x0, #0 / mov x1, <display> / bl _SLSDisplayGetTiming`, then
//    reads the result from [sp, #N]. On x86-64 that result pointer is a hidden first argument, so through the adapter
//    SkyLight took x0 = 0 as the destination: SIGSEGV, fault 0x0, in SkyLight display_get_timing_common+0x53 with the
//    display ID (0x8c41c03) still in r14. That crash-looped WindowManager, and Finder cannot draw a window without it.
//    A native probe asking the same display for its modes and refresh rate gets correct answers, so the display is fine.
extern "C" void __arm2x86_sls_display_get_timing(void) {
    static void (*f)(void *, uint64_t, uint64_t);
    if (!f) { f = (void (*)(void *, uint64_t, uint64_t))dlsym(RTLD_DEFAULT, "SLSDisplayGetTiming");
              if (!f) { fprintf(stderr, "[cxx_bridge] SLSDisplayGetTiming not found in SkyLight\n"); a2x_stop("native function not found", "SLSDisplayGetTiming"); abort(); } }
    f((void *)RX(8), RX(0), RX(1)); RX(0) = RX(8);
}
// the rest of std::to_string (ehmx: to_string(int) went through the adapter and libc++ wrote the string to rdi = 2).
//    Integers arrive in x0, float/double in v0 (__arm2x86_rf + 80). to_string(long double) is not here: arm64's long double
//    is a double, x86's is 80-bit — that one needs a conversion, not a forward.
#define TO_STRING_X(NAME, SYM, T) extern "C" void NAME(void) { static void *(*f)(void *, T); \
    if (!f) f = (void *(*)(void *, T))need(SYM); f((void *)RX(8), (T)RX(0)); RX(0) = RX(8); }
#define TO_STRING_V(NAME, SYM, T) extern "C" void NAME(void) { static void *(*f)(void *, T); \
    if (!f) f = (void *(*)(void *, T))need(SYM); f((void *)RX(8), *(T *)(__arm2x86_rf + 80)); RX(0) = RX(8); }
TO_STRING_X(__arm2x86_cxx_to_string_i, "_ZNSt3__19to_stringEi", int)
TO_STRING_X(__arm2x86_cxx_to_string_l, "_ZNSt3__19to_stringEl", long)
TO_STRING_X(__arm2x86_cxx_to_string_m, "_ZNSt3__19to_stringEm", unsigned long)
TO_STRING_X(__arm2x86_cxx_to_string_x, "_ZNSt3__19to_stringEx", long long)
TO_STRING_X(__arm2x86_cxx_to_string_y, "_ZNSt3__19to_stringEy", unsigned long long)
TO_STRING_V(__arm2x86_cxx_to_string_f, "_ZNSt3__19to_stringEf", float)
TO_STRING_V(__arm2x86_cxx_to_string_d, "_ZNSt3__19to_stringEd", double)
// __sincos_stret & co: arm64 returns {sin, cos} as an HFA in d0/d1 (s0/s1); x86-64 returns the double pair in
//    xmm0/xmm1 and packs the float pair into xmm0 — the generic adapter carries v0 only, so cos came back as a stale v1.
//    clang turns every sin(x)+cos(x) pair into one of these (ehmx, Cyberpunk imports both). A scalar FP write zeroes the
//    rest of the vector register, as on the hardware.
#include <math.h>
#define RV(j) (__arm2x86_rf + 80 + 16 * (j))
#define SINCOS_D(NAME, S, C) extern "C" void NAME(void) { double x; memcpy(&x, RV(0), 8); double s = S(x), c = C(x); \
    memset(RV(0), 0, 32); memcpy(RV(0), &s, 8); memcpy(RV(1), &c, 8); }
#define SINCOS_F(NAME, S, C) extern "C" void NAME(void) { float x; memcpy(&x, RV(0), 4); float s = S(x), c = C(x); \
    memset(RV(0), 0, 32); memcpy(RV(0), &s, 4); memcpy(RV(1), &c, 4); }
extern "C" double __sinpi(double); extern "C" double __cospi(double); extern "C" float __sinpif(float); extern "C" float __cospif(float);
SINCOS_D(__arm2x86_sincos_stret, sin, cos)
SINCOS_F(__arm2x86_sincosf_stret, sinf, cosf)
SINCOS_D(__arm2x86_sincospi_stret, __sinpi, __cospi)
SINCOS_F(__arm2x86_sincospif_stret, __sinpif, __cospif)
// objc_retain_xN / objc_release_xN are arm64-only libobjc size-saving entry points (objc4 objc-msg-arm64.s:
//    `mov x0, xN; b _objc_retain`): the object arrives in xN, not x0. x86-64 libobjc has none of them, so the import stayed
//    0 and the first call aborted `no entry for 0x0` (Stocks, macOS 27: 47 unresolved, all but a few of them these).
//    Same contract here: native retain/release of xN, and x0 = the object afterwards, as the arm64 entry point leaves it.
extern "C" void *objc_retain(void *); extern "C" void objc_release(void *);
//    The register file carries x0..x8 only, so the lifter stores xN into slot 0 for these binds ('xin' in xlat2's CXX
//    table) and one handler serves every N. x0 = the object afterwards, as the arm64 entry point leaves it.
extern "C" void __arm2x86_objc_retain_xin(void) { RX(0) = (uint64_t)objc_retain((void *)RX(0)); }
extern "C" void __arm2x86_objc_release_xin(void) { uint64_t o = RX(0); objc_release((void *)o); RX(0) = o; }
// Per-N wrappers, because the host has no lifter to do the move for it. In a per-app build the
//    lifter stores xN into slot 0 before this bind (the 'xin' handlers above, and that is why they read slot 0);
//    the interpreter copies x0..x8 into the register file verbatim, so for every N != 0 the two handlers above
//    retained whatever x0 happened to hold — and for N > 8 the register file cannot even carry the object.
// On macOS 27 the 11 arm64-only apps import _objc_retain_x1..x28 and _swift_retain_x1..x28;
//    Maps alone left 34 of them unresolved, and the first call would have aborted "no entry for 0x0".
// __arm2x86_rfall is the additive side channel: all 31 arm64 integer registers, filled on the same path
//    as __arm2x86_rf, so slot N really is xN. a64_is_rf_callee keys on the wrapper's address, so giving each N
//    its own function is what recovers N — no new conveyance is needed beyond the registers themselves.
// x18 is reserved on Darwin and has no entry point; the list below skips it exactly as the generator does.
// The arm64 entry point's contract, read off the real thing (arm64 libobjc, _objc_retain_x19): the first
//    instruction is `ands x0, x19, x19` — the object arrives in xN and the result is left in x0.
extern "C" { __thread unsigned long __arm2x86_rfall[31]; }
// swift_retain/swift_release are the Swift-native refcount entry points and are not objc_retain: a native
//    Swift object carries its own refcount word. Resolved once and loudly — substituting the ObjC entry point
//    would corrupt that refcount with no symptom until a much later use-after-free.
static void *(*a2x_swift_retain)(void *);
static void  (*a2x_swift_release)(void *);
static void a2x_swift_rr(void) {
    if (a2x_swift_retain && a2x_swift_release) return;
    a2x_swift_retain  = (void *(*)(void *))dlsym(RTLD_DEFAULT, "swift_retain");
    a2x_swift_release = (void  (*)(void *))dlsym(RTLD_DEFAULT, "swift_release");
    if (!a2x_swift_retain || !a2x_swift_release) {
        // On macOS 27 libswiftCore has no file on disk (it lives in the dyld cache), but dlopen of its
        //    canonical path still resolves it. This is the fallback for a guest that binds no other Swift symbol.
        void *h = dlopen("/usr/lib/swift/libswiftCore.dylib", RTLD_LAZY | RTLD_GLOBAL);
        if (h) {
            if (!a2x_swift_retain)  a2x_swift_retain  = (void *(*)(void *))dlsym(h, "swift_retain");
            if (!a2x_swift_release) a2x_swift_release = (void  (*)(void *))dlsym(h, "swift_release");
        }
    }
    if (!a2x_swift_retain || !a2x_swift_release) {
        fprintf(stderr, "[cxx_bridge] the native Swift runtime exports no swift_retain/swift_release, so a "
                        "register-specialised Swift retain cannot be honoured\n");
        a2x_stop("native function not found", "swift_retain"); abort();
    }
}
/* ___chkstk_darwin is a stack probe with its own contract (size in x9, every register preserved).
      The guest stack is committed memory, so the probe has nothing to do; doing nothing is exactly
      the contract. It writes no guest register on purpose. */
extern "C" void __arm2x86_chkstk_noop(void) { }

#define A2X_RR_XN(N) \
  extern "C" void __arm2x86_objc_retain_x##N(void)   { RX(0) = (uint64_t)objc_retain((void *)__arm2x86_rfall[N]); } \
  extern "C" void __arm2x86_objc_release_x##N(void)  { uint64_t o = __arm2x86_rfall[N]; objc_release((void *)o); RX(0) = o; } \
  extern "C" void __arm2x86_swift_retain_x##N(void)  { a2x_swift_rr(); RX(0) = (uint64_t)a2x_swift_retain((void *)__arm2x86_rfall[N]); } \
  extern "C" void __arm2x86_swift_release_x##N(void) { uint64_t o = __arm2x86_rfall[N]; a2x_swift_rr(); a2x_swift_release((void *)o); RX(0) = o; }
A2X_RR_XN(0)
A2X_RR_XN(1)
A2X_RR_XN(2)
A2X_RR_XN(3)
A2X_RR_XN(4)
A2X_RR_XN(5)
A2X_RR_XN(6)
A2X_RR_XN(7)
A2X_RR_XN(8)
A2X_RR_XN(9)
A2X_RR_XN(10)
A2X_RR_XN(11)
A2X_RR_XN(12)
A2X_RR_XN(13)
A2X_RR_XN(14)
A2X_RR_XN(15)
A2X_RR_XN(16)
A2X_RR_XN(17)
A2X_RR_XN(19)
A2X_RR_XN(20)
A2X_RR_XN(21)
A2X_RR_XN(22)
A2X_RR_XN(23)
A2X_RR_XN(24)
A2X_RR_XN(25)
A2X_RR_XN(26)
A2X_RR_XN(27)
A2X_RR_XN(28)
#undef A2X_RR_XN
// operator+(const char*, const std::string&): ret(x8), lhs x0, rhs x1
extern "C" void __arm2x86_cxx_plus_cstr_string(void) {
    static void *(*f)(void *, const char *, const void *);
    if (!f) f = (void *(*)(void *, const char *, const void *))
        need("_ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EEPKS6_RKS9_");
    f((void *)RX(8), (const char *)RX(0), (const void *)RX(1)); RX(0) = RX(8);
}

// The throw happens natively; translated frames carry x86 LSDAs (eh.py → invoke/landingpad), so it unwinds to the
//    arm64 catch. `dest` is the exception's arm64 destructor: libc++abi calls it natively when the catch ends, so it goes
//    through a native trampoline (__arm2x86_cb) like every other arm64 function pointer handed to native code.
#include <typeinfo>
extern "C" unsigned long __arm2x86_cb(unsigned long fn);
extern "C" void __arm2x86_cxa_throw(void *obj, std::type_info *ti, void (*dest)(void *)) {
    dest = (void (*)(void *))__arm2x86_cb((unsigned long)dest);
    static void (*real)(void *, std::type_info *, void (*)(void *));
    if (!real) real = (void (*)(void *, std::type_info *, void (*)(void *)))need("__cxa_throw");
    // arm64 Apple RTTI sets bit 63 of a program-defined type's name pointer (non-unique RTTI: "compare by string");
    //    x86 reads it as a plain pointer, and strlen on that non-canonical address would crash this log line.
    fprintf(stderr, "[xlat2] C++ exception thrown from translated code: %s\n",
            ti ? (const char *)((uintptr_t)ti->name() & ~(1ULL << 63)) : "?");
    real(obj, ti, dest);
}

// std::string arguments into native C++. With the guest libc++ mapped, a guest-made std::string is in the arm64
//    layout ({data, size, cap|bit63} long / chars + size in byte 23 short) and a native x86 function reads the classic layout
//    ({cap|1, size, data} / size*2 + chars). CoreLocationAgent's CLConnectionClient::setHandlerForMessage(std::string const&, ...)
//    turned its message name into an NSString from those wrong bytes, got nil, and LocationSupport aborted with
//    "-[__NSDictionaryM setObject:forKeyedSubscript:]: key cannot be nil" (4 per boot). The 298 routes import 7 such
//    native functions (LocationSupport 4, ProtocolBuffer 3). Each binds to a slot here: every `std::string const&` and by-value
//    std::string argument is handed over as a native copy built from the guest bytes, destroyed after the call (the caller owns a
//    by-value argument on both ABIs). Refused, and bound as before, with a [strb] line: a non-const std::string& (would need the
//    result written back in the arm64 layout), any float/double or unknown by-value parameter, more than 8 integer arguments,
//    or more than STRB_N functions. Return values are not converted (not encoded in the name): name() const is still open.
#include <cxxabi.h>
#include <new>
#include <string>
#include <vector>
namespace {
enum { K_INT = 0, K_CREF = 1, K_VAL = 2 };
struct Strb { void *fn; int n; int thisret; unsigned char kind[8]; };
const int STRB_N = 48;
Strb g_strb[STRB_N];
int g_nstrb;
const char *const kStr = "std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>";
// the arm64 (alternate) layout, read only; a guest string we cannot read sanely is passed as empty and said once
bool a64_str(const unsigned char *s, const char **p, size_t *n) {
    if (!s) return false;
    if (s[23] & 0x80) {
        *p = *(const char *const *)s; *n = *(const uint64_t *)(s + 8);
        return *p && *n < (1ull << 32);
    }
    *p = (const char *)s; *n = s[23];
    return *n <= 22;
}
void strb_common(int k) {
    const Strb &b = g_strb[k];
    uint64_t a[8], x0 = RX(0);
    alignas(std::string) unsigned char buf[8][sizeof(std::string)];
    bool live[8] = {};
    for (int i = 0; i < 8; i++) a[i] = RX(i);
    for (int i = 0; i < b.n; i++) {
        if (b.kind[i] == K_INT) continue;
        const char *p = ""; size_t n = 0;
        if (!a64_str((const unsigned char *)(uintptr_t)a[i], &p, &n)) {
            static int said;
            if (!said++) fprintf(stderr, "[strb] slot %d arg %d is not a readable arm64 std::string: passed empty\n", k, i);
            p = ""; n = 0;
        }
        new (buf[i]) std::string(p, n);
        live[i] = true;
        a[i] = (uint64_t)(uintptr_t)buf[i];
    }
    typedef uint64_t (*F8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    uint64_t r = ((F8)b.fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    for (int i = 0; i < 8; i++) if (live[i]) reinterpret_cast<std::string *>(buf[i])->~basic_string();
    RX(0) = b.thisret ? x0 : r;              // Apple arm64 C1/C2 return `this`; the x86 ones return nothing
}
template <int K> void strb_stub() { strb_common(K); }
template <int... K> struct StubList { static constexpr void (*v[sizeof...(K)])() = { &strb_stub<K>... }; };
typedef StubList<0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
                 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47> Stubs;
std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(' '), z = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, z - a + 1);
}
bool scalar(const std::string &p) {
    static const char *const ok[] = { "bool", "char", "signed char", "unsigned char", "short", "unsigned short", "int",
        "unsigned int", "long", "unsigned long", "long long", "unsigned long long", 0 };
    for (int i = 0; ok[i]; i++) if (p == ok[i]) return true;
    return false;
}
}  // namespace
// sym: the mangled import name with its leading underscore; nat: the native address. 0 = not ours, bind as before.
extern "C" void (*a2x_strb_bind(const char *sym, void *nat))(void) {
    if (!sym || !nat || strncmp(sym, "__Z", 3) || !strstr(sym, "basic_string")) return 0;
    int st = 0;
    char *d = abi::__cxa_demangle(sym + 1, 0, 0, &st);
    if (!d || st) { free(d); return 0; }
    std::string s(d); free(d);
    // the parameter list: the first '(' outside <>, to its matching ')'
    size_t open = std::string::npos; int ang = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '<') ang++; else if (s[i] == '>') ang--;
        else if (s[i] == '(' && !ang) { open = i; break; }
    }
    if (open == std::string::npos) return 0;
    std::string name = s.substr(0, open);
    std::vector<std::string> ps; std::string cur; int dp = 0, da = 0; size_t close = std::string::npos;
    for (size_t i = open + 1; i < s.size(); i++) {
        char c = s[i];
        if (c == '(') dp++; else if (c == ')') { if (!dp) { close = i; break; } dp--; }
        else if (c == '<') da++; else if (c == '>') da--;
        if (c == ',' && !dp && !da) { ps.push_back(trim(cur)); cur.clear(); continue; }
        cur += c;
    }
    if (close == std::string::npos) return 0;
    if (!trim(cur).empty() && trim(cur) != "void") ps.push_back(trim(cur));
    // a member when the name is nested (Class::method); the 7 imports this exists for are all members — the log line says which
    size_t q = name.rfind("::");
    bool member = q != std::string::npos;
    bool ctor = member && [&] { size_t q0 = name.rfind("::", q - 1); std::string cls = name.substr(q0 == std::string::npos ? 0 : q0 + 2, q - (q0 == std::string::npos ? 0 : q0 + 2));
                                return name.substr(q + 2) == cls; }();
    Strb b = {}; b.fn = nat; b.thisret = ctor;
    if (member) b.kind[b.n++] = K_INT;
    int nstr = 0;
    for (const std::string &p : ps) {
        int k;
        if (p == std::string(kStr) + " const&") k = K_CREF;
        else if (p == kStr) k = K_VAL;
        else if (p.find("basic_string") != std::string::npos && p.back() == '&') {
            fprintf(stderr, "[strb] REFUSED %s: non-const std::string& (%s)\n", sym, p.c_str()); return 0; }
        else if (p.back() == '*' || p.back() == '&' || p.find("block_pointer") != std::string::npos
                 || p.compare(0, 21, "std::__1::shared_ptr<") == 0 || scalar(p)
                 || p == "id" || p.compare(0, 3, "id<") == 0 || p == "SEL" || p == "Class") k = K_INT;   // ObjC object pointers
        else { fprintf(stderr, "[strb] REFUSED %s: parameter '%s' not integer-class\n", sym, p.c_str()); return 0; }
        if (b.n >= 8) { fprintf(stderr, "[strb] REFUSED %s: more than 8 integer arguments\n", sym); return 0; }
        b.kind[b.n++] = (unsigned char)k;
        nstr += k != K_INT;
    }
    if (!nstr) return 0;                     // a string only inside a template or return: nothing to convert here
    if (g_nstrb >= STRB_N) { fprintf(stderr, "[strb] REFUSED %s: all %d slots used\n", sym, STRB_N); return 0; }
    int k = g_nstrb++;
    g_strb[k] = b;
    if (A2X_DBGENV("ARM2X86_TRACE_BINDS"))
        fprintf(stderr, "[strb] slot %d %s args %d strings %d member %d ctor %d\n", k, sym, b.n, nstr, member, ctor);
    return Stubs::v[k];
}
