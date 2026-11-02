#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#pragma pack(push,1)
typedef struct { char t; uint8_t v; uint16_t a; int32_t abs; uint32_t cyc; uint32_t fr; } R;
#pragma pack(pop)
int main(int c,char**v){ static uint8_t seen[131072]; static R r[1<<20]; size_t n; for(int k=1;k<c;k++){FILE*f=fopen(v[k],"rb"); while((n=fread(r,sizeof(R),1<<20,f))>0) for(size_t i=0;i<n;i++) if(r[i].t=='X'&&r[i].abs>=0) seen[r[i].abs]=1; fclose(f);} for(int i=0;i<131072;i++) if(seen[i]) printf("%d\n",i); }
