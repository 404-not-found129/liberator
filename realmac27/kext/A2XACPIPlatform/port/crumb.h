// crumb.h - lock-free on-screen breadcrumbs (diagnostic build); see crumb.cpp.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void a2x_crumb(uint32_t code);
void a2x_crumb_ring(uint32_t a, uint32_t b);   /* push a row, marker yellow */
void a2x_crumb_ring_done(void);
void a2x_crumb_pause(int on);                        /* screen off (configure pass): count, do not draw */
uint32_t a2x_crumb_seq(uint32_t *lastCode);    /* breadcrumb count, last code */
void a2x_crumb_rebase(uint64_t fbPhys);              /* console moved: map the new base, redraw */                /* newest row returned, marker green */
void a2x_crumb_init(uint64_t fbPhys, uint32_t rowBytes, uint32_t width, uint32_t height, uint32_t depth);
#ifdef __cplusplus
}
struct A2XCrumbScope {      // ENTER on construction, EXIT (site + 1) on every return path
    uint32_t c;
    A2XCrumbScope(uint32_t site, uint32_t detail) : c((site << 24) | (detail & 0xffffffu)) { a2x_crumb(c); }
    ~A2XCrumbScope() { a2x_crumb(c + 0x01000000u); }
};
#endif
