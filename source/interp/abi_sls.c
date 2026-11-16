/* abi_sls.c — SkyLight transaction calls that take a transform BY VALUE. #included by objc_bridge.c right after
   abi_audit.c (CATransform3D, CGAffineTransform, A2X_RX and a2x_need come from abi_structs_gen.c).

   Without this, a window vanishes for as long as it is dragged on macOS 27.2. WindowServer logs "Singular matrix"
      ~20 times a second during a drag and 0 when idle, from window_set_transform <- CGXSetWindowTransform3DList.
      The 4x4 it decodes is [junk, 0, 1,0,0,0, 0,1,0,0, 0,0,1,0, tx, ty]: the sender's matrix two values late,
      its last two (0, 1) cut off, so m44 = 0.
   The mechanism is abi_audit.c's, with a bigger struct: a 128-byte CATransform3D (and a 48-byte
      CGAffineTransform, six doubles, too long for an HFA) is a pointer in an x register on arm64 and is copied
      onto the stack on x86-64. These are SPI, so sigdb has no signature and the generic adapter passed the
      pointer as a scalar; native SkyLight then encoded whatever lay on its own stack.
      Signatures read from the arm64 SkyLight prologues (27.2):
        SLSTransactionSetWindowTransform3D(tx x0, wid w1, const CATransform3D *x2)
        SLSTransactionSetWindowTransform  (tx x0, wid w1, int w2, int w3, const CGAffineTransform *x4)
   WindowManager (arm64e only, so interpreted) is the one importer of the 3D call: it is the drag "lift". */
void __arm2x86_abi_sls_SLSTransactionSetWindowTransform3D(void) {
    static unsigned long (*f)(void *, unsigned int, CATransform3D);
    if (!f) f = a2x_need("SLSTransactionSetWindowTransform3D");
    CATransform3D t = *(const CATransform3D *)(A2X_RX(2));
    A2X_RX(0) = f((void *)A2X_RX(0), (unsigned int)A2X_RX(1), t);
}
void __arm2x86_abi_sls_SLSTransactionSetWindowTransform(void) {
    static unsigned long (*f)(void *, unsigned int, int, int, CGAffineTransform);
    if (!f) f = a2x_need("SLSTransactionSetWindowTransform");
    CGAffineTransform t = *(const CGAffineTransform *)(A2X_RX(4));
    A2X_RX(0) = f((void *)A2X_RX(0), (unsigned int)A2X_RX(1), (int)A2X_RX(2), (int)A2X_RX(3), t);
}

/* With the transform fixed the window was still not drawn while dragged: the transforms now reached
      WindowServer correct (identity + translation, m44 = 1, 0 "Singular matrix") and the window was absent from
      every captured frame until the mouse was released.
   Same boundary, the other direction of it: a CGRect is an HFA on arm64 (d0-d3) and MEMORY on x86-64 (the
      stack). Apple's x86_64 SkyLight reads all three of these from [rbp+0x10]:
        SLSTransactionSetWindowLockedBounds       (tx x0, wid w1, CGRect d0-d3)
        SLSTransactionSetWindowLockedBoundsAtPlace(tx x0, wid w1, int w2, CGRect d0-d3)
        SLSTransactionSetSpaceTransform           (tx x0, space x1, int w2, const CGAffineTransform *x3)
      Found by scanning every SkyLight import of WindowManager for a stack argument in the x86_64 body; the
      scan is calibrated on the two transform calls above (both flagged). Seven flagged of 153; the other two
      (SLSNewWindowWithOpaqueShapeAndContext, SLSWindowIteratorGetMatchingSpaceID) take a 7th integer argument,
      which the generic door already carries. */
#define A2X_SLS_RECT ((CGRect){ A2X_VD(0), A2X_VD(1), A2X_VD(2), A2X_VD(3) })
void __arm2x86_abi_sls_SLSTransactionSetWindowLockedBounds(void) {
    static unsigned long (*f)(void *, unsigned int, CGRect);
    if (!f) f = a2x_need("SLSTransactionSetWindowLockedBounds");
    A2X_RX(0) = f((void *)A2X_RX(0), (unsigned int)A2X_RX(1), A2X_SLS_RECT);
}
void __arm2x86_abi_sls_SLSTransactionSetWindowLockedBoundsAtPlace(void) {
    static unsigned long (*f)(void *, unsigned int, int, CGRect);
    if (!f) f = a2x_need("SLSTransactionSetWindowLockedBoundsAtPlace");
    A2X_RX(0) = f((void *)A2X_RX(0), (unsigned int)A2X_RX(1), (int)A2X_RX(2), A2X_SLS_RECT);
}
void __arm2x86_abi_sls_SLSTransactionSetSpaceTransform(void) {
    static unsigned long (*f)(void *, unsigned long, int, CGAffineTransform);
    if (!f) f = a2x_need("SLSTransactionSetSpaceTransform");
    CGAffineTransform t = *(const CGAffineTransform *)(A2X_RX(3));
    A2X_RX(0) = f((void *)A2X_RX(0), (unsigned long)A2X_RX(1), (int)A2X_RX(2), t);
}
