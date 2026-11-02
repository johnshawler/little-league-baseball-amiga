#include <stdio.h>
#include <stdint.h>
#pragma pack(push,1)
typedef struct { char t; uint8_t v; uint16_t a; int32_t abs; uint32_t cyc; uint32_t fr; } R;
#pragma pack(pop)
int main(int c,char**v){ static R r[1<<20]; size_t n; FILE*f=fopen(v[1],"rb"),*o=fopen(v[2],"wb"); long cli=0,sei=0,irq=0,cld=0,sed=0,brk=0,rti=0;
 while((n=fread(r,sizeof(R),1<<20,f))>0) for(size_t i=0;i<n;i++){ R*x=&r[i]; if(x->t!='X'){ fwrite(x,sizeof(R),1,o);} else { if(x->v==0x58)cli++; if(x->v==0x78)sei++; if(x->a==0xcdf4)irq++; if(x->v==0xf8)sed++; if(x->v==0x00)brk++; if(x->v==0x40) rti++; } }
 printf("CLI %ld SEI %ld IRQ-vector-handler-entries %ld SED %ld BRK %ld RTI %ld\n",cli,sei,irq,sed,brk,rti); }
