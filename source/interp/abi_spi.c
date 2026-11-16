/* abi_spi.c — more SPI that takes or returns a struct BY VALUE, found by a stack-argument sweep.
   #included by objc_bridge.c after abi_sls.c; types and macros come from abi_structs_gen.c.

   The sweep: every C import of the 205 arm64-only programs
      (10,925 names) was resolved to its x86_64 body in the 27.2 cache (10,226 found) and flagged when that body
      reads [rbp+0x10..]. Calibrated on the five abi_sls.c calls: all five flagged. 458 flagged; 87 already
      routed, 157 typed by sigdb, 37 only a 7th integer argument, 34 variadic. These eight are the ones the Dock,
      Finder, WindowManager and the sandbox/TCC path use; each signature was read from both prologues.
   Still unhandled from the same list — named so nobody assumes they are covered: AXFGeometry* (9),
      ACSHCreateRotatingAngleImage, ACSHSendZoomFocusChangedNotificationForRect, TISMakeRectWithAspectRatioInsideRect,
      QLDrawGrayLinearGradient, QLPushRoundedRect. */
typedef struct { float m[20]; } A2XColorMatrix;   /* CA::ColorMatrix, 80 bytes: x86 reads the two at [rbp+0x10] and [rbp+0x60] */

/* arm64 (cid w0, w1, w2, CGRect d0-d3) · x86-64 (edi, esi, edx, CGRect on the stack) */
void __arm2x86_abi_spi_SLSSetDockRectWithOrientation(void) {
    static unsigned long (*f)(int, int, int, CGRect); if (!f) f = a2x_need("SLSSetDockRectWithOrientation");
    A2X_RX(0) = f((int)A2X_RX(0), (int)A2X_RX(1), (int)A2X_RX(2), (CGRect){ A2X_VD(0), A2X_VD(1), A2X_VD(2), A2X_VD(3) });
}
void __arm2x86_abi_spi_CGRegionCreateWithRect(void) {
    static void *(*f)(CGRect); if (!f) f = a2x_need("CGRegionCreateWithRect");
    A2X_RX(0) = (unsigned long)f((CGRect){ A2X_VD(0), A2X_VD(1), A2X_VD(2), A2X_VD(3) });
}
void __arm2x86_abi_spi_BSStringFromCGRect(void) {
    static void *(*f)(CGRect); if (!f) f = a2x_need("BSStringFromCGRect");
    A2X_RX(0) = (unsigned long)f((CGRect){ A2X_VD(0), A2X_VD(1), A2X_VD(2), A2X_VD(3) });
}
/* arm64 (CGPoint d0,d1; const CATransform3D *x0) -> CGPoint in d0,d1 · x86-64 (xmm0,xmm1; the matrix on the stack) */
void __arm2x86_abi_spi_CA_CGPointApplyTransform(void) {
    static CGPoint (*f)(CGPoint, CATransform3D); if (!f) f = a2x_need("CA_CGPointApplyTransform");
    CGPoint p = (CGPoint){ A2X_VD(0), A2X_VD(1) };
    CATransform3D t = *(const CATransform3D *)(A2X_RX(0));
    CGPoint r = f(p, t);
    memset(__arm2x86_rf + 80, 0, 64);
    A2X_VD(0) = r.x; A2X_VD(1) = r.y;
}
/* arm64 (result x8, const ColorMatrix *x0, const ColorMatrix *x1) · x86-64 (sret rdi, both matrices on the stack) */
void __arm2x86_abi_spi_CAColorMatrixConcat(void) {
    static A2XColorMatrix (*f)(A2XColorMatrix, A2XColorMatrix); if (!f) f = a2x_need("CAColorMatrixConcat");
    A2XColorMatrix a = *(const A2XColorMatrix *)(A2X_RX(0)), b = *(const A2XColorMatrix *)(A2X_RX(1));
    A2XColorMatrix r = f(a, b);
    __builtin_memcpy((void *)A2X_RX(8), &r, sizeof r);
}
/* audit_token_t by value, the abi_audit.c mechanism, three more names */
void __arm2x86_abi_spi_TCCAccessPreflightWithAuditToken(void) {
    static int (*f)(void *, audit_token_t, void *); if (!f) f = a2x_need("TCCAccessPreflightWithAuditToken");
    A2X_RX(0) = (unsigned long)(long)f((void *)A2X_RX(0), *(const audit_token_t *)(A2X_RX(1)), (void *)A2X_RX(2));
}
void __arm2x86_abi_spi_sandbox_container_path_for_audit_token(void) {
    static int (*f)(audit_token_t, char *, unsigned long); if (!f) f = a2x_need("sandbox_container_path_for_audit_token");
    A2X_RX(0) = (unsigned long)(long)f(*(const audit_token_t *)(A2X_RX(0)), (char *)A2X_RX(1), (unsigned long)A2X_RX(2));
}
void __arm2x86_abi_spi_sandbox_extension_issue_file_to_process(void) {
    static char *(*f)(const char *, const char *, unsigned int, audit_token_t);
    if (!f) f = a2x_need("sandbox_extension_issue_file_to_process");
    A2X_RX(0) = (unsigned long)f((const char *)A2X_RX(0), (const char *)A2X_RX(1), (unsigned int)A2X_RX(2), *(const audit_token_t *)(A2X_RX(3)));
}

/* AccessibilityFoundation geometry: rectangles and points by value (from the same sweep). arm64 passes each CGRect in
   four d registers and each CGPoint in two; x86-64 passes a CGRect on the stack and a CGPoint in two xmm registers, and
   returns a CGRect through a hidden pointer. Signatures read from both prologues. A threshold that follows two
   rectangles is the ninth double, so on arm64 it is on the stack (__arm_sp). */
#define A2X_R0 ((CGRect){ A2X_VD(0), A2X_VD(1), A2X_VD(2), A2X_VD(3) })
#define A2X_R4 ((CGRect){ A2X_VD(4), A2X_VD(5), A2X_VD(6), A2X_VD(7) })
#define A2X_R2 ((CGRect){ A2X_VD(2), A2X_VD(3), A2X_VD(4), A2X_VD(5) })
#define A2X_RET_POINT(r) do { memset(__arm2x86_rf + 80, 0, 64); A2X_VD(0) = (r).x; A2X_VD(1) = (r).y; } while (0)
#define A2X_RET_RECT(r)  do { memset(__arm2x86_rf + 80, 0, 64); A2X_VD(0) = (r).x; A2X_VD(1) = (r).y; A2X_VD(2) = (r).w; A2X_VD(3) = (r).h; } while (0)
void __arm2x86_abi_spi_AXFGeometryCenterOfRect(void) {
    static CGPoint (*f)(CGRect); if (!f) f = a2x_need("AXFGeometryCenterOfRect");
    CGPoint r = f(A2X_R0); A2X_RET_POINT(r);
}
void __arm2x86_abi_spi_AXFGeometryIsValidRect(void) {
    static unsigned char (*f)(CGRect); if (!f) f = a2x_need("AXFGeometryIsValidRect");
    A2X_RX(0) = (unsigned long)f(A2X_R0);
}
void __arm2x86_abi_spi_AXFGeometryDistanceBetweenRects(void) {
    static double (*f)(CGRect, CGRect); if (!f) f = a2x_need("AXFGeometryDistanceBetweenRects");
    double r = f(A2X_R0, A2X_R4); memset(__arm2x86_rf + 80, 0, 64); A2X_VD(0) = r;
}
void __arm2x86_abi_spi_AXFGeometryNormalizePointRelativeToRect(void) {
    static CGPoint (*f)(CGPoint, CGRect); if (!f) f = a2x_need("AXFGeometryNormalizePointRelativeToRect");
    CGPoint r = f((CGPoint){ A2X_VD(0), A2X_VD(1) }, A2X_R2); A2X_RET_POINT(r);
}
void __arm2x86_abi_spi_AXFGeometryShiftPointFullyWithinContainerRect(void) {
    static CGPoint (*f)(CGPoint, CGRect); if (!f) f = a2x_need("AXFGeometryShiftPointFullyWithinContainerRect");
    CGPoint r = f((CGPoint){ A2X_VD(0), A2X_VD(1) }, A2X_R2); A2X_RET_POINT(r);
}
void __arm2x86_abi_spi_AXFGeometryShiftRectFullyWithinContainerRect(void) {
    static CGRect (*f)(CGRect, CGRect); if (!f) f = a2x_need("AXFGeometryShiftRectFullyWithinContainerRect");
    CGRect r = f(A2X_R0, A2X_R4); A2X_RET_RECT(r);
}
void __arm2x86_abi_spi_AXFGeometryIsRectVerticallyAlignedWithRectWithThreshold(void) {
    static unsigned char (*f)(CGRect, CGRect, double); if (!f) f = a2x_need("AXFGeometryIsRectVerticallyAlignedWithRectWithThreshold");
    A2X_RX(0) = (unsigned long)f(A2X_R0, A2X_R4, *(const double *)__arm_sp);
}
/* arm64 (result x8, CGRect d0-d3) · x86-64 (sret rdi, the CGRect on the stack at [rbp+0x10..0x28]); read from both
   bodies in the 27.2 caches. The Dock builds every icon image's transform with it. */
void __arm2x86_abi_spi_RBImageSimpleTransform(void) {
    static CGAffineTransform (*f)(CGRect); if (!f) f = a2x_need("RBImageSimpleTransform");
    CGAffineTransform r = f((CGRect){ A2X_VD(0), A2X_VD(1), A2X_VD(2), A2X_VD(3) });
    __builtin_memcpy((void *)A2X_RX(8), &r, sizeof r);
}
/* UIKit (Catalyst): UIEdgeInsets is four doubles - an HFA in d0-d3 on arm64, MEMORY on x86-64, and a 32-byte return
   comes back through a hidden pointer there. arm64 (insets d0-d3, edges x0, replacement d4-d7) -> d0-d3 ·
   x86-64 (sret rdi, edges rsi, both structs on the stack at [rbp+0x10] and [rbp+0x30]); read from both bodies.
   Clock calls it while laying out its first view and took the fault inside it. */
typedef struct { double top, left, bottom, right; } A2XEdgeInsets;
void __arm2x86_abi_spi_UIEdgeInsetsReplace(void) {
    static A2XEdgeInsets (*f)(A2XEdgeInsets, unsigned long, A2XEdgeInsets); if (!f) f = a2x_need("UIEdgeInsetsReplace");
    A2XEdgeInsets r = f((A2XEdgeInsets){ A2X_VD(0), A2X_VD(1), A2X_VD(2), A2X_VD(3) }, (unsigned long)A2X_RX(0),
                        (A2XEdgeInsets){ A2X_VD(4), A2X_VD(5), A2X_VD(6), A2X_VD(7) });
    memset(__arm2x86_rf + 80, 0, 64);
    A2X_VD(0) = r.top; A2X_VD(1) = r.left; A2X_VD(2) = r.bottom; A2X_VD(3) = r.right;
}
/* UIEdgeInsetsAdd(insets, edges, more): the same shape as UIEdgeInsetsReplace. arm64 body: for each edge bit set in
   x0 (1, 2, 4, 8) the result's d0..d3 is d0..d3 + d4..d7, else d0..d3 unchanged; result in d0-d3. x86-64 returns
   through a hidden pointer in rdi: called through the untyped door it took the guest's x0 (the edge mask, 0xa)
   for that pointer and stored the result at address 0x1a (Books, laying out its store view). */
void __arm2x86_abi_spi_UIEdgeInsetsAdd(void) {
    static A2XEdgeInsets (*f)(A2XEdgeInsets, unsigned long, A2XEdgeInsets); if (!f) f = a2x_need("UIEdgeInsetsAdd");
    A2XEdgeInsets r = f((A2XEdgeInsets){ A2X_VD(0), A2X_VD(1), A2X_VD(2), A2X_VD(3) }, (unsigned long)A2X_RX(0),
                        (A2XEdgeInsets){ A2X_VD(4), A2X_VD(5), A2X_VD(6), A2X_VD(7) });
    memset(__arm2x86_rf + 80, 0, 64);
    A2X_VD(0) = r.top; A2X_VD(1) = r.left; A2X_VD(2) = r.bottom; A2X_VD(3) = r.right;
}

/* MapKit and CoreLocation C functions that take or return coordinates, map points and map rectangles by value.
   MKMapRect and MKCoordinateRegion are four doubles: arm64 passes and returns them in d registers (a homogeneous
   float aggregate), x86-64 passes them on the stack and returns them through a hidden pointer in rdi. Called
   through the untyped door, the x86 callee took the guest's x0 for that hidden pointer and wrote the 32-byte result
   over whatever x0 pointed at. Journal: an object's first word became a latitude, and the next retain of that
   object faulted. The two-double types (CLLocationCoordinate2D, MKMapPoint) travel in two d / two xmm registers
   on both sides; they get handlers too so that both result registers are always published.
   x86 shapes read from the 27.2 MapKit bodies; the arm64 shapes follow from the public declarations. */
#define A2X_P0 ((CGPoint){ A2X_VD(0), A2X_VD(1) })
#define A2X_RET_P2(r) do { memset(__arm2x86_rf + 80, 0, 64); A2X_VD(0) = (r).x; A2X_VD(1) = (r).y; A2X_XRV(1) = (r).y; } while (0)
#define A2X_RET_R4(r) do { memset(__arm2x86_rf + 80, 0, 64); A2X_VD(0) = (r).x; A2X_VD(1) = (r).y; A2X_VD(2) = (r).w; A2X_VD(3) = (r).h; \
                           A2X_XRV(1) = (r).y; A2X_XRV(2) = (r).w; A2X_XRV(3) = (r).h; } while (0)
void __arm2x86_abi_spi_MKCoordinateRegionMakeWithDistance(void) {
    static CGRect (*f)(CGPoint, double, double); if (!f) f = a2x_need("MKCoordinateRegionMakeWithDistance");
    CGRect r = f(A2X_P0, A2X_VD(2), A2X_VD(3)); A2X_RET_R4(r);
}
void __arm2x86_abi_spi_MKCoordinateRegionForMapRect(void) {
    static CGRect (*f)(CGRect); if (!f) f = a2x_need("MKCoordinateRegionForMapRect");
    CGRect r = f(A2X_R0); A2X_RET_R4(r);
}
void __arm2x86_abi_spi_MKMapRectUnion(void) {
    static CGRect (*f)(CGRect, CGRect); if (!f) f = a2x_need("MKMapRectUnion");
    CGRect r = f(A2X_R0, A2X_R4); A2X_RET_R4(r);
}
void __arm2x86_abi_spi_MKMapRectInset(void) {
    static CGRect (*f)(CGRect, double, double); if (!f) f = a2x_need("MKMapRectInset");
    CGRect r = f(A2X_R0, A2X_VD(4), A2X_VD(5)); A2X_RET_R4(r);
}
void __arm2x86_abi_spi_MKMapRectOffset(void) {
    static CGRect (*f)(CGRect, double, double); if (!f) f = a2x_need("MKMapRectOffset");
    CGRect r = f(A2X_R0, A2X_VD(4), A2X_VD(5)); A2X_RET_R4(r);
}
void __arm2x86_abi_spi_MKMapRectContainsPoint(void) {
    static unsigned char (*f)(CGRect, CGPoint); if (!f) f = a2x_need("MKMapRectContainsPoint");
    A2X_RX(0) = (unsigned long)f(A2X_R0, (CGPoint){ A2X_VD(4), A2X_VD(5) });
}
void __arm2x86_abi_spi_MKMapRectContainsRect(void) {
    static unsigned char (*f)(CGRect, CGRect); if (!f) f = a2x_need("MKMapRectContainsRect");
    A2X_RX(0) = (unsigned long)f(A2X_R0, A2X_R4);
}
void __arm2x86_abi_spi_MKMapRectIntersectsRect(void) {
    static unsigned char (*f)(CGRect, CGRect); if (!f) f = a2x_need("MKMapRectIntersectsRect");
    A2X_RX(0) = (unsigned long)f(A2X_R0, A2X_R4);
}
void __arm2x86_abi_spi_MKCoordinateForMapPoint(void) {
    static CGPoint (*f)(CGPoint); if (!f) f = a2x_need("MKCoordinateForMapPoint");
    CGPoint r = f(A2X_P0); A2X_RET_P2(r);
}
void __arm2x86_abi_spi_MKMapPointForCoordinate(void) {
    static CGPoint (*f)(CGPoint); if (!f) f = a2x_need("MKMapPointForCoordinate");
    CGPoint r = f(A2X_P0); A2X_RET_P2(r);
}
void __arm2x86_abi_spi_MKMetersBetweenMapPoints(void) {
    static double (*f)(CGPoint, CGPoint); if (!f) f = a2x_need("MKMetersBetweenMapPoints");
    double r = f(A2X_P0, (CGPoint){ A2X_VD(2), A2X_VD(3) }); memset(__arm2x86_rf + 80, 0, 64); A2X_VD(0) = r;
}
void __arm2x86_abi_spi_CLLocationCoordinate2DIsValid(void) {
    static unsigned char (*f)(CGPoint); if (!f) f = a2x_need("CLLocationCoordinate2DIsValid");
    A2X_RX(0) = (unsigned long)f(A2X_P0);
}
void __arm2x86_abi_spi_CLLocationCoordinate2DMake(void) {
    static CGPoint (*f)(double, double); if (!f) f = a2x_need("CLLocationCoordinate2DMake");
    CGPoint r = f(A2X_VD(0), A2X_VD(1)); A2X_RET_P2(r);
}
/* MapKit private: MKMapRect _MKMapRectThatFits(MKMapRect, CGSize, edge insets). arm64 (rect d0-d3, size d4-d5, the
   insets do not fit in d6-d7 and go on the stack) -> d0-d3 · x86-64 (sret rdi, rect at [rbp+0x10], size xmm0-xmm1,
   insets at [rbp+0x30]). Read from both bodies: each builds the view bounds {0, 0, size} from d4-d5 / xmm0-xmm1. */
void __arm2x86_abi_spi__MKMapRectThatFits(void) {
    static CGRect (*f)(CGRect, CGSize, A2XEdgeInsets); if (!f) f = a2x_need("_MKMapRectThatFits");
    const double *st = (const double *)__arm_sp;
    CGRect r = f(A2X_R0, (CGSize){ A2X_VD(4), A2X_VD(5) }, (A2XEdgeInsets){ st[0], st[1], st[2], st[3] });
    A2X_RET_R4(r);
}
/* CAFrameRateRange is three floats: s0-s2 and s3-s5 on arm64, each packed into xmm pairs on x86-64. */
void __arm2x86_abi_spi_CAFrameRateRangeIsEqualToRange(void) {
    static unsigned char (*f)(a2x_f3, a2x_f3); if (!f) f = a2x_need("CAFrameRateRangeIsEqualToRange");
    A2X_RX(0) = (unsigned long)f((a2x_f3){ A2X_VF(0), A2X_VF(1), A2X_VF(2) }, (a2x_f3){ A2X_VF(3), A2X_VF(4), A2X_VF(5) });
}

/* More MapKit entry points with the same four-double result, none of them in the SDK headers (so not in sigdb):
   each x86-64 body starts by saving rdi as the result pointer. Journal calls MKDefaultCoordinateRegion() right after
   objc_retainAutoreleasedReturnValue, so x0 was a live object and the 32-byte region landed on its first four words
   (the isa read back as a latitude). MKMapRectBoundingMapPoints takes (const MKMapPoint *, count). */
void __arm2x86_abi_spi_MKDefaultCoordinateRegion(void) {
    static CGRect (*f)(void); if (!f) f = a2x_need("MKDefaultCoordinateRegion");
    CGRect r = f(); A2X_RET_R4(r);
}
void __arm2x86_abi_spi_MKMapRectForCoordinateRegion(void) {
    static CGRect (*f)(CGRect); if (!f) f = a2x_need("MKMapRectForCoordinateRegion");
    CGRect r = f(A2X_R0); A2X_RET_R4(r);
}
void __arm2x86_abi_spi_MKMapRectMakeWithRadialDistance(void) {
    static CGRect (*f)(CGPoint, double); if (!f) f = a2x_need("MKMapRectMakeWithRadialDistance");
    CGRect r = f(A2X_P0, A2X_VD(2)); A2X_RET_R4(r);
}
void __arm2x86_abi_spi_MKMapRectBoundingMapPoints(void) {
    static CGRect (*f)(const void *, unsigned long); if (!f) f = a2x_need("MKMapRectBoundingMapPoints");
    CGRect r = f((const void *)A2X_RX(0), (unsigned long)A2X_RX(1)); A2X_RET_R4(r);
}
