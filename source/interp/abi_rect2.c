/* abi_rect2.c - native calls whose arguments are pointers or large structures and whose result is a structure.
   Each layout was read from the x86-64 function's own code (macOS 27.2 x86 cache): the result goes through a hidden
   pointer in rdi there. On arm64 a result of four doubles comes back in d0-d3, any other result over 16 bytes goes
   through x8, and a structure argument over 16 bytes is passed as a pointer to a copy. */
typedef struct { long v[4]; } a2x_l4;
/* CGRect SLSWindowIteratorGetScreenRect(SLSWindowIteratorRef, CFIndex): x86 keeps rsi and rdx, writes 32 bytes at rdi */
void __arm2x86_abi_r2_SLSWindowIteratorGetScreenRect(void) {
    static a2x_hR (*f)(void *, long); if (!f) f = a2x_need("SLSWindowIteratorGetScreenRect");
    a2x_hR r = f((void *)A2X_RX(0), (long)A2X_RX(1)); A2X_HFA_RET(r);
}
/* WKRect WKRectGetValue(WKRectRef): unwraps rsi, writes four doubles at rdi */
void __arm2x86_abi_r2_WKRectGetValue(void) {
    static a2x_hR (*f)(void *); if (!f) f = a2x_need("WKRectGetValue");
    a2x_hR r = f((void *)A2X_RX(0)); A2X_HFA_RET(r);
}
/* CGRect DFRSystemEventGetButtonScreenRect(event): copies 32 bytes from rsi+0x28 to rdi */
void __arm2x86_abi_r2_DFRSystemEventGetButtonScreenRect(void) {
    static a2x_hR (*f)(void *); if (!f) f = a2x_need("DFRSystemEventGetButtonScreenRect");
    a2x_hR r = f((void *)A2X_RX(0)); A2X_HFA_RET(r);
}
/* CGRect NUPixelRectToCGRect(NUPixelRect): four integers on the stack at 16(%rbp)..40(%rbp), converted to doubles */
void __arm2x86_abi_r2_NUPixelRectToCGRect(void) {
    static a2x_hR (*f)(a2x_l4); if (!f) f = a2x_need("NUPixelRectToCGRect");
    a2x_hR r = f(*(const a2x_l4 *)A2X_RX(0)); A2X_HFA_RET(r);
}
/* CGRect CALPixelAlignedRect(CGRect): the rectangle is on the stack at 16(%rbp) and 32(%rbp); no register argument */
void __arm2x86_abi_r2_CALPixelAlignedRect(void) {
    static a2x_hR (*f)(a2x_hR); if (!f) f = a2x_need("CALPixelAlignedRect");
    a2x_hR r = f((a2x_hR){ { A2X_VD(0), A2X_VD(1), A2X_VD(2), A2X_VD(3) } }); A2X_HFA_RET(r);
}
/* id PXIndexPathFromSimpleIndexPath(PXSimpleIndexPath): 32 bytes on the stack at 16(%rbp); the result is an object */
void __arm2x86_abi_r2_PXIndexPathFromSimpleIndexPath(void) {
    static void *(*f)(a2x_l4); if (!f) f = a2x_need("PXIndexPathFromSimpleIndexPath");
    A2X_RX(0) = (uint64_t)(uintptr_t)f(*(const a2x_l4 *)A2X_RX(0));
}
/* LSVersionNumber _LSMakeVersionNumber(major, minor, bugfix): rsi, rdx and rcx stored at rdi, 0 at rdi+0x18; 32
   bytes of integers, which arm64 returns through x8 */
void __arm2x86_abi_r2__LSMakeVersionNumber(void) {
    static a2x_l4 (*f)(long, long, long); if (!f) f = a2x_need("_LSMakeVersionNumber");
    a2x_l4 r = f((long)A2X_RX(0), (long)A2X_RX(1), (long)A2X_RX(2)); memcpy((void *)(uintptr_t)A2X_RX(8), &r, sizeof r);
    A2X_RX(0) = A2X_RX(8);
}
