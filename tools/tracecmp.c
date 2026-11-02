/* tracecmp a.trc b.trc [types]  - first divergence between two 16-byte-record traces (types default "XW"); compares address, value, cycle. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#pragma pack(push,1)
typedef struct { char t; uint8_t v; uint16_t a; int32_t abs; uint32_t cyc; uint32_t fr; } R;
#pragma pack(pop)
static unsigned lo=0,hi=0xFFFF;
static int next(FILE*f,const char*ty,R*r,long*idx){ while(fread(r,sizeof *r,1,f)==1){ if(strchr(ty,r->t)&&r->a>=lo&&r->a<=hi){ (*idx)++; return 1; } } return 0; }
int main(int c,char**v){ FILE*a=fopen(v[1],"rb"),*b=fopen(v[2],"rb"); const char*ty=c>3?v[3]:"XW"; if(c>5){ lo=strtoul(v[4],0,16); hi=strtoul(v[5],0,16);}  R ra,rb; long ia=0,ib=0,n=0; R hist[8]; 
 for(;;){ int ea=next(a,ty,&ra,&ia), eb=next(b,ty,&rb,&ib); if(!ea&&!eb){ printf("IDENTICAL over %ld records\n",n); return 0; }
   if(!ea||!eb||ra.t!=rb.t||ra.a!=rb.a||ra.v!=rb.v||ra.cyc!=rb.cyc){ printf("DIVERGE at record %ld\n",n); 
     for(int i=0;i<8&&i<n;i++){ R h=hist[(n-1-i)&7]; printf("  prev: %c a=%04x v=%02x cyc=%u\n",h.t,h.a,h.v,h.cyc); if(i>3)break; }
     if(ea) printf("  A: %c a=%04x v=%02x cyc=%u fr=%u\n",ra.t,ra.a,ra.v,ra.cyc,ra.fr); else printf("  A: end\n");
     if(eb) printf("  B: %c a=%04x v=%02x cyc=%u fr=%u\n",rb.t,rb.a,rb.v,rb.cyc,rb.fr); else printf("  B: end\n"); return 1; }
   hist[n&7]=ra; n++; } }
