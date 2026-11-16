/* Recognize an embedded interpreter predecessor by defined runtime symbols.
   Ordinary universal libraries retain native loading. All offsets are file-relative. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static uint32_t a2x_dep_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static uint32_t a2x_dep_le32(const uint8_t *p) {
    return ((uint32_t)p[3]<<24)|((uint32_t)p[2]<<16)|((uint32_t)p[1]<<8)|p[0];
}
static int a2x_dep_span(size_t n, uint64_t off, uint64_t len) { return off<=n && len<=n-off; }
static int a2x_aot_dependency_bytes(const uint8_t *p, size_t n) {
    if(n<8 || a2x_dep_be32(p)!=0xcafebabe) return 0;
    uint32_t count=a2x_dep_be32(p+4);
    if(count>(n-8)/20) return 0;
    const uint8_t *x=0; size_t xn=0; int arm=0;
    for(uint32_t i=0;i<count;i++) {
        const uint8_t *a=p+8+20*i;
        uint32_t cpu=a2x_dep_be32(a), off=a2x_dep_be32(a+8), len=a2x_dep_be32(a+12);
        if(!a2x_dep_span(n,off,len) || len<32) return 0;
        const uint8_t *s=p+off;
        if(a2x_dep_le32(s)!=0xfeedfacf || a2x_dep_le32(s+4)!=cpu) return 0;
        if(cpu==0x0100000c) arm=1;
        if(cpu==0x01000007) {x=s;xn=len;}
    }
    if(!arm || !x) return 0;
    uint32_t nc=a2x_dep_le32(x+16), lcbytes=a2x_dep_le32(x+20);
    if(!a2x_dep_span(xn,32,lcbytes)) return 0;
    size_t at=32,end=32+lcbytes; unsigned found=0;
    for(uint32_t i=0;i<nc;i++) {
        if(at>end || end-at<8) return 0;
        uint32_t cmd=a2x_dep_le32(x+at), sz=a2x_dep_le32(x+at+4);
        if(sz<8 || (sz&3) || sz>end-at) return 0;
        if(cmd==2) {
            if(sz<24) return 0;
            uint32_t so=a2x_dep_le32(x+at+8), ns=a2x_dep_le32(x+at+12);
            uint32_t st=a2x_dep_le32(x+at+16), sn=a2x_dep_le32(x+at+20);
            if(!a2x_dep_span(xn,so,(uint64_t)ns*16)||!a2x_dep_span(xn,st,sn)) return 0;
            for(uint32_t j=0;j<ns;j++) {
                const uint8_t *s=x+so+16*j; uint32_t ix=a2x_dep_le32(s);
                if((s[4]&0xe0) || (s[4]&0x0e)!=0x0e || !(s[4]&1) || !s[5] || ix>=sn) continue;
                const char *name=(const char *)(x+st+ix);
                if(!memchr(name,0,sn-ix)) return 0;
                if(!strcmp(name,"___arm2x86_lookup")) found|=1;
                if(!strcmp(name,"___arm2x86_cb_door")) found|=2;
                if(!strcmp(name,"___arm2x86_native_adapter")) found|=4;
            }
        }
        at+=sz;
    }
    return at==end && found==7;
}
