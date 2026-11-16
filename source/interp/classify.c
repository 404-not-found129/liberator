#include <stdio.h>
#include <stdlib.h>
#include "aot_dependency.h"
int main(int argc,char **argv){for(int i=1;i<argc;i++){FILE*f=fopen(argv[i],"rb");if(!f)return 2;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);unsigned char*p=malloc(n);if(!p||fread(p,1,n,f)!=(size_t)n)return 3;fclose(f);printf("%d %s\n",a2x_aot_dependency_bytes(p,n),argv[i]);free(p);}return 0;}
