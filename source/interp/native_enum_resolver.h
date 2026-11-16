#ifndef A2X_NATIVE_ENUM_RESOLVER_H
#define A2X_NATIVE_ENUM_RESOLVER_H
#include <stdint.h>
#include <stddef.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
struct a2x_native_enum_layout { unsigned width,alignment; uintptr_t metadata,descriptor; };
/* The resolver's native metadata pointers are read through bounded local Mach reads. */
static int a2x_enum_read(uintptr_t address,void *out,size_t size) {
    if(!address||size>24||address>UINTPTR_MAX-size)return 0;
    mach_vm_size_t got=0;
    return mach_vm_read_overwrite(mach_task_self(),address,size,(mach_vm_address_t)(uintptr_t)out,&got)==KERN_SUCCESS&&got==size;
}
static int a2x_nopayload_enum_metadata(const void *metadata,struct a2x_native_enum_layout *out) {
    uintptr_t m=(uintptr_t)metadata,words[2],vwt;
    if(!out||m<sizeof(uintptr_t)||!a2x_enum_read(m,words,sizeof words)||words[0]!=0x201)return 0;
    uint32_t descriptor_flags,payload;
    if(!a2x_enum_read(words[1],&descriptor_flags,4)||
       (descriptor_flags&0x1fu)!=0x12u||
       words[1]>UINTPTR_MAX-20||!a2x_enum_read(words[1]+20,&payload,4)||
       (payload&0x00ffffffu)!=0)return 0;
    if(!a2x_enum_read(m-sizeof(uintptr_t),&vwt,sizeof vwt)||vwt>UINTPTR_MAX-64)return 0;
    struct {uint64_t size,stride;uint32_t flags,extra;} layout;
    if(!a2x_enum_read(vwt+64,&layout,sizeof layout))return 0;
    unsigned align=(layout.flags&0xffu)+1u;
    if((layout.flags&0x30000u)||align>8||(align&(align-1))||layout.stride!=layout.size||
       (layout.size!=1&&layout.size!=2&&layout.size!=4&&layout.size!=8)||layout.size!=align)return 0;
    *out=(struct a2x_native_enum_layout){(unsigned)layout.size,align,m,words[1]};return 1;
}
/* Supply the verified imported mangling, e.g. So<length><identifier>V; no enum-name table. */
static int a2x_resolve_native_nopayload_enum(const char *mangled,size_t length,struct a2x_native_enum_layout *out) {
    if(!mangled||length<4||length>512||mangled[0]!='S'||mangled[1]!='o')return 0;
    typedef const void *(*resolve_fn)(const char*,size_t,const void*,const void *const*);
    resolve_fn resolve=(resolve_fn)dlsym(RTLD_DEFAULT,"swift_getTypeByMangledNameInContext");
    if(!resolve)return 0;
    return a2x_nopayload_enum_metadata(resolve(mangled,length,0,0),out);
}
#endif
