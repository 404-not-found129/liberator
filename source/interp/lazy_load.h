#ifndef A2X_LAZY_LOAD_H
#define A2X_LAZY_LOAD_H
#include <stdint.h>
#include <stddef.h>
struct a2x_lazy_binding { uint64_t at; const char *symbol; };
struct a2x_lazy_info { const char *path; uint64_t flag; unsigned weak; size_t count; struct a2x_lazy_binding *bindings; };
/* Owns only bindings; strings refer to the caller's immutable image. */
int a2x_lazy_parse(const uint8_t *,size_t,uint64_t,uint64_t,struct a2x_lazy_info *);
void a2x_lazy_dispose(struct a2x_lazy_info *);
#ifdef A2X_LAZY_LOAD_IMPL
#include <stdlib.h>
#include <string.h>
static uint32_t a2x_lz_u32(const void *p){uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t a2x_lz_u64(const void *p){uint64_t v;memcpy(&v,p,8);return v;}
static const char *a2x_lz_str(const uint8_t*b,size_t n,uint32_t o){return o<n&&memchr(b+o,0,n-o)?(const char*)b+o:0;}
struct a2x_lz_seg{uint64_t vm,size,file,bytes;unsigned prot;};
static int a2x_lz_vm(const struct a2x_lz_seg*s,unsigned n,uint64_t a,size_t z,int file,size_t*out){
 for(unsigned i=0;i<n;i++)if(a>=s[i].vm&&a-s[i].vm<=s[i].size&&z<=s[i].size-(a-s[i].vm)){
  if(!(s[i].prot&2))return 0;
  if(file){if(a-s[i].vm>s[i].bytes||z>s[i].bytes-(a-s[i].vm))return 0;*out=(size_t)(s[i].file+a-s[i].vm);}return 1;
 }return 0;
}
void a2x_lazy_dispose(struct a2x_lazy_info*x){free(x->bindings);memset(x,0,sizeof*x);}
int a2x_lazy_parse(const uint8_t*b,size_t n,uint64_t slide,uint64_t flag,struct a2x_lazy_info*out){
 memset(out,0,sizeof*out);
 if(n<32||a2x_lz_u32(b)!=0xfeedfacf||a2x_lz_u32(b+4)!=0x0100000c)return -1;
 uint32_t nc=a2x_lz_u32(b+16),sz=a2x_lz_u32(b+20);if(sz>n-32)return -2;
 struct a2x_lz_seg seg[64];unsigned ns=0;uint64_t base=0;int have=0;size_t pos=32,end=32+(size_t)sz;
 for(uint32_t k=0;k<nc;k++){
  if(pos>end||end-pos<8)return -2;uint32_t cmd=a2x_lz_u32(b+pos),cs=a2x_lz_u32(b+pos+4);
  if(cs<8||(cs&3)||cs>end-pos)return -2;
  if(cmd==0x19){if(cs<72||ns==64)return -2;struct a2x_lz_seg*s=&seg[ns++];s->vm=a2x_lz_u64(b+pos+24);s->size=a2x_lz_u64(b+pos+32);s->file=a2x_lz_u64(b+pos+40);s->bytes=a2x_lz_u64(b+pos+48);s->prot=a2x_lz_u32(b+pos+60);
   if(s->vm>UINT64_MAX-s->size||s->bytes>s->size||s->file>n||s->bytes>n-s->file)return -2;
   if(!memcmp(b+pos+8,"__TEXT\0",7)){if(have)return -2;base=s->vm;have=1;}
  }pos+=cs;
 }
 if(!have||base>UINT64_MAX-slide||flag<slide)return -2;
 uint64_t unslidflag=flag-slide;size_t ignored=0;
 if((flag&3)||!a2x_lz_vm(seg,ns,unslidflag,4,0,&ignored))return -3;
 const uint8_t*blob=0;size_t blen=0;pos=32;
 for(uint32_t k=0;k<nc;k++){
  uint32_t cmd=a2x_lz_u32(b+pos),cs=a2x_lz_u32(b+pos+4);
  if(cmd==0x3a){if(cs!=16)return -4;uint32_t fo=a2x_lz_u32(b+pos+8),len=a2x_lz_u32(b+pos+12);if(fo>n||len>n-fo||len<24)return -4;const uint8_t*t=b+fo;uint32_t rel=a2x_lz_u32(t+4);
   if(base<=UINT64_MAX-rel&&base+rel==unslidflag){if(blob)return -4;blob=t;blen=len;}
  }pos+=cs;
 }
 if(!blob)return -5;
 uint32_t pathoff=a2x_lz_u32(blob),fo=a2x_lz_u32(blob+4),packed=a2x_lz_u32(blob+8),start=a2x_lz_u32(blob+12),count=a2x_lz_u32(blob+16),syms=a2x_lz_u32(blob+20);
 unsigned weak=packed&0xffff,fmt=packed>>16;const char*path=a2x_lz_str(blob,blen,pathoff);
 if(!path||!*path||(weak&~1U)||count>65536||syms>blen||count>(blen-syms)/4)return -6;
 int arm=fmt==1||fmt==9||fmt==12;if(!arm&&fmt!=2&&fmt!=6)return -7;
 for(uint32_t i=0;i<count;i++){const char*s=a2x_lz_str(blob,blen,a2x_lz_u32(blob+syms+4*(size_t)i));if(!s||!*s)return -6;}
 if(base>UINT64_MAX-start)return -8;uint64_t at=base+start;size_t used=0,cap=0;struct a2x_lazy_binding*items=0;
 for(size_t step=0;step<=n/8;step++){
  size_t off=0;if(!a2x_lz_vm(seg,ns,at,8,1,&off)||off>n||n-off<8)goto bad;
  uint64_t raw=a2x_lz_u64(b+off),next=(raw>>51)&(arm?0x7ff:0xfff);int auth=arm&&(raw>>63);int bind=(raw>>(arm?62:63))&1;
  if(bind){uint32_t ord=raw&((arm&&fmt!=12)?0xffff:0xffffff);if(ord>=count||(!auth&&((raw>>(arm?32:24))&(arm?0x7ffff:0xff))))goto bad;
   if(used==65536)goto bad;if(used==cap){size_t c=cap?cap*2:16;void*p=realloc(items,c*sizeof*items);if(!p)goto bad;items=p;cap=c;}
   if(at>UINT64_MAX-slide)goto bad;items[used].at=at+slide;items[used].symbol=a2x_lz_str(blob,blen,a2x_lz_u32(blob+syms+4*(size_t)ord));used++;
  }
  if(!next){out->path=path;out->flag=base+slide+fo;out->weak=weak;out->count=used;out->bindings=items;return 0;}
  uint64_t add=next*(arm?8:4);if(at>UINT64_MAX-add)goto bad;at+=add;
 }
 bad:free(items);return -8;
}
#endif
#endif
