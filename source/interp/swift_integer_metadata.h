#ifndef A2X_SWIFT_INTEGER_METADATA_H
#define A2X_SWIFT_INTEGER_METADATA_H
#include <stdio.h>
#include "native_enum_resolver.h"
static int a2x_swift_imported_integer(const char *type,unsigned *width) {
    if(strncmp(type,"__C.",4))return 0;
    const char *name=type+4;size_t n=strlen(name);
    if(!n||n>400||!(isalpha((unsigned char)name[0])||name[0]=='_'))return 0;
    for(size_t i=1;i<n;i++)if(!(isalnum((unsigned char)name[i])||name[i]=='_'))return 0;
    char mangled[512];int size=snprintf(mangled,sizeof mangled,"So%zu%sV",n,name);
    if(size<0||(size_t)size>=sizeof mangled)return 0;
    struct a2x_native_enum_layout layout;
    if(!a2x_resolve_native_nopayload_enum(mangled,(size_t)size,&layout))return 0;
    *width=layout.width;return 1;
}
#endif
