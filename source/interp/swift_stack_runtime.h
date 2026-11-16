#ifndef A2X_SWIFT_STACK_RUNTIME_H
#define A2X_SWIFT_STACK_RUNTIME_H
#include <stdlib.h>
#include <dlfcn.h>
#include <pthread.h>
#include "swift_stack.h"
#include "swift_integer_metadata.h"
struct a2x_swift_stack_cache {
    unsigned long target; char *name; int supported;
    struct a2x_swift_stack_plan plan; struct a2x_swift_stack_cache *next;
};
static struct a2x_swift_stack_cache *a2x_swift_stacks[1024];
static pthread_mutex_t a2x_swift_stack_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned a2x_swift_stack_cache_count;
static __thread int a2x_swift_stack_resolving;
static char *(*a2x_swift_demangle_fn)(const char *,size_t,char *,size_t *,uint32_t);
static pthread_once_t a2x_swift_demangle_once = PTHREAD_ONCE_INIT;
static void a2x_swift_demangle_init(void) { a2x_swift_demangle_fn=dlsym(RTLD_DEFAULT,"swift_demangle"); }
static const char *a2x_swift_symbol(const char *s) { return s && s[0]=='_' && s[1]=='$' ? s+1:s; }
int a2x_swift_stack_prepare(const char *name,unsigned long target,const unsigned long *xs,
                           const unsigned char *vs,unsigned long armsp,unsigned char *out) {
    name=a2x_swift_symbol(name);
    if(!name || name[0]!='$' || name[1]!='s' || strnlen(name,8193)>8192)return 0;
    unsigned bucket=(unsigned)(target>>4)&1023u;
    for(struct a2x_swift_stack_cache *p=__atomic_load_n(&a2x_swift_stacks[bucket],__ATOMIC_ACQUIRE);p;p=p->next)
        if(p->target==target && !strcmp(p->name,name)) {
            if(!p->supported || !p->plan.moves)return 0;
            a2x_swift_stack_apply(&p->plan,xs,vs,(const void*)armsp,out);return 1;
        }
    if(a2x_swift_stack_resolving || __atomic_load_n(&a2x_swift_stack_cache_count,__ATOMIC_RELAXED)>=4096)return 0;
    struct a2x_swift_stack_cache *p=calloc(1,sizeof *p);if(!p)return 0;
    p->target=target;p->name=strdup(name);if(!p->name){free(p);return 0;}
    a2x_swift_stack_resolving=1;
    Dl_info info;
    int exact_export=0;
    if(dladdr((void*)target,&info)) {
        exact_export=dlsym(RTLD_DEFAULT,name)==(void*)target;
        if(!exact_export && info.dli_fname) {
            void *image=dlopen(info.dli_fname,RTLD_LAZY|RTLD_NOLOAD);
            if(image) {exact_export=dlsym(image,name)==(void*)target;dlclose(image);}
        }
    }
    /* Shared-cache dladdr may name only a nearby local symbol; the export lookup proves exact entry. */
    if(exact_export) {
        pthread_once(&a2x_swift_demangle_once,a2x_swift_demangle_init);
        if(a2x_swift_demangle_fn) {
            char *text=a2x_swift_demangle_fn(name,strlen(name),NULL,NULL,0);
            if(text) {p->supported=a2x_swift_stack_compile(text,a2x_swift_imported_integer,&p->plan);free(text);}
        }
    }
    a2x_swift_stack_resolving=0;
    /* Metadata/demangling can call native runtime code: no cache lock is held while resolving. */
    pthread_mutex_lock(&a2x_swift_stack_mutex);
    for(struct a2x_swift_stack_cache *old=a2x_swift_stacks[bucket];old;old=old->next)
        if(old->target==target && !strcmp(old->name,name)) {
            pthread_mutex_unlock(&a2x_swift_stack_mutex);free(p->name);free(p);
            if(!old->supported || !old->plan.moves)return 0;
            a2x_swift_stack_apply(&old->plan,xs,vs,(const void*)armsp,out);return 1;
        }
    if(a2x_swift_stack_cache_count>=4096) {
        pthread_mutex_unlock(&a2x_swift_stack_mutex);free(p->name);free(p);return 0;
    }
    p->next=a2x_swift_stacks[bucket];
    __atomic_store_n(&a2x_swift_stacks[bucket],p,__ATOMIC_RELEASE);
    __atomic_add_fetch(&a2x_swift_stack_cache_count,1,__ATOMIC_RELAXED);
    pthread_mutex_unlock(&a2x_swift_stack_mutex);
    if(!p->supported || !p->plan.moves)return 0;
    a2x_swift_stack_apply(&p->plan,xs,vs,(const void*)armsp,out);return 1;
}
#endif
