#ifndef A2X_SWIFT_STACK_H
#define A2X_SWIFT_STACK_H
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#define A2X_SWIFT_STACK_BYTES 512u
#define A2X_SWIFT_LEAVES 64u
struct a2x_swift_move { uint16_t source, target; uint8_t kind, width; };
struct a2x_swift_stack_plan { unsigned moves, leaves; struct a2x_swift_move move[A2X_SWIFT_LEAVES]; };
typedef int (*a2x_swift_integer_resolver)(const char *, unsigned *);
static int a2x_swift_leaf(struct a2x_swift_stack_plan *p, unsigned width, int fp,
                          unsigned *gp, unsigned *fr, unsigned *armsp, unsigned *natsp) {
    if (++p->leaves > A2X_SWIFT_LEAVES) return 0;
    unsigned *reg = fp ? fr : gp, i = (*reg)++, source, kind;
    if (i < 8) { source = i; kind = fp ? 1 : 0; }
    else { *armsp = (*armsp + width - 1) & ~(width - 1); source = *armsp; *armsp += width; kind = 2; }
    if (*armsp > A2X_SWIFT_STACK_BYTES) return 0;
    if (i >= (fp ? 8u : 6u)) {
        if (*natsp + 8 > A2X_SWIFT_STACK_BYTES) return 0;
        p->move[p->moves++] = (struct a2x_swift_move){source, *natsp, kind, width}; *natsp += 8;
    }
    return 1;
}
static int a2x_swift_type(struct a2x_swift_stack_plan *p, const char *t,
                         a2x_swift_integer_resolver imported, unsigned *gp, unsigned *fr,
                         unsigned *armsp, unsigned *natsp) {
    unsigned width = 0; int fp = 0, words = 1;
    if (!strcmp(t,"Swift.Bool") || !strcmp(t,"Swift.Bool?") || !strcmp(t,"Swift.Optional<Swift.Bool>") || !strcmp(t,"Swift.UInt8") || !strcmp(t,"Swift.Int8")) width=1;
    else if (!strcmp(t,"Swift.UInt16") || !strcmp(t,"Swift.Int16")) width=2;
    else if (!strcmp(t,"Swift.UInt32") || !strcmp(t,"Swift.Int32")) width=4;
    else if (!strcmp(t,"Swift.UInt64") || !strcmp(t,"Swift.Int64") || !strcmp(t,"Swift.UInt") || !strcmp(t,"Swift.Int")) width=8;
    else if (!strcmp(t,"Swift.Float")) { width=4; fp=1; }
    else if (!strcmp(t,"Swift.Double") || !strcmp(t,"CoreGraphics.CGFloat")) { width=8; fp=1; }
    else if (!strcmp(t,"Swift.String")) { width=8; words=2; }
    else if ((t[0]=='[' && strlen(t)>2 && t[strlen(t)-1]==']') ||
             (!strncmp(t,"Swift.Array<",12) && strlen(t)>13 && t[strlen(t)-1]=='>')) width=8;
    else if (!strncmp(t,"__C.",4) && imported && imported(t,&width) && (width==1||width==2||width==4||width==8)) { }
    else return 0;
    for (int i=0;i<words;i++) if (!a2x_swift_leaf(p,width,fp,gp,fr,armsp,natsp)) return 0;
    return 1;
}
/* Parse only complete ordinary, nongeneric function signatures; unknown shapes retain the old door. */
static int a2x_swift_stack_compile(const char *s, a2x_swift_integer_resolver imported,
                                  struct a2x_swift_stack_plan *result) {
    if (!s || strnlen(s,16385)>16384) return 0;
    const char *open=strchr(s,'('); if (!open || open==s) return 0;
    for (const char *q=s;q<open;q++) if (!(isalnum((unsigned char)*q)||*q=='_'||*q=='.'||*q=='$')) return 0;
    struct a2x_swift_stack_plan p={0}; unsigned gp=0,fp=0,armsp=0,natsp=0;
    const char *start=open+1, *q=start; unsigned square=0,angle=0;
    for (;;) {
        char c=*q;
        if (!c) return 0;
        if (c=='[') ++square;
        else if (c==']') { if (!square) return 0; --square; }
        else if (c=='<') ++angle;
        else if (c=='>') { if (!angle) return 0; --angle; }
        else if (c=='(') return 0; /* tuple/function-valued parameters need a different lowering */
        if ((c==','||c==')') && !square && !angle) {
            const char *a=start,*end=q,*colon=NULL;
            while(a<end && *a==' ') ++a;
            while(end>a && end[-1]==' ') --end;
            for(const char *r=a;r<end;r++) if(*r==':') {colon=r;break;}
            if(a==end) {if(start!=open+1 || c!=')') return 0;}
            else {
                if(!colon || colon==a) return 0;
                for(const char *r=a;r<colon;r++) if(!(isalnum((unsigned char)*r)||*r=='_')) return 0;
                a=colon+1;while(a<end && *a==' ')++a;
                if(end-a<=0 || end-a>=512) return 0;
                char type[512];memcpy(type,a,(size_t)(end-a));type[end-a]=0;
                if(!a2x_swift_type(&p,type,imported,&gp,&fp,&armsp,&natsp)) return 0;
            }
            if(c==')') {if(strncmp(q+1," -> ",4)||!q[5]||strstr(q+5," where "))return 0;break;}
            start=q+1;
        }
        ++q;
    }
    *result=p; return 1;
}
static void a2x_swift_stack_apply(const struct a2x_swift_stack_plan *p, const unsigned long *xs,
                                 const unsigned char *vs, const void *armstack, unsigned char *out) {
    memset(out,0,A2X_SWIFT_STACK_BYTES);
    for(unsigned i=0;i<p->moves;i++) {
        const struct a2x_swift_move *m=&p->move[i];
        const void *source=m->kind==0?(const void*)(xs+m->source):m->kind==1?(const void*)(vs+16*m->source):(const void*)((const unsigned char*)armstack+m->source);
        memcpy(out+m->target,source,m->width);
    }
}
#endif
