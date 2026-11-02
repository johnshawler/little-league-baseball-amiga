/* Runs the SingleStepTests/65x02 nes6502 vectors (tests/cpu/*.txt) against runtime/cpu6502.c: registers, flags, RAM and every bus cycle. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/cpu6502.h"
static uint8_t mem[65536]; static struct{uint16_t a;uint8_t v;char k;} cyc[64]; static int nc;
uint8_t bus_rd(uint16_t a){ cyc[nc].a=a; cyc[nc].v=mem[a]; cyc[nc].k='r'; nc++; return mem[a]; }
void bus_wr(uint16_t a,uint8_t v){ cyc[nc].a=a; cyc[nc].v=v; cyc[nc].k='w'; nc++; mem[a]=v; }
int main(int argc,char**argv){
  long tot=0,bad=0; char line[8192];
  for(int k=1;k<argc;k++){ FILE*f=fopen(argv[k],"r"); if(!f){perror(argv[k]);return 2;}
    int fails=0,n=0;
    while(fgets(line,sizeof line,f)){
      char name[64]; strcpy(name,line+2); name[strcspn(name,"\n")]=0;
      unsigned pc,s,a,x,y,p; unsigned epc,es,ea,ex,ey,ep; int cnt,off;
      fgets(line,sizeof line,f); sscanf(line,"I %u %u %u %u %u %u",&pc,&s,&a,&x,&y,&p);
      fgets(line,sizeof line,f); sscanf(line,"E %u %u %u %u %u %u",&epc,&es,&ea,&ex,&ey,&ep);
      memset(mem,0,sizeof mem);
      fgets(line,sizeof line,f); char*q=line+2; cnt=strtol(q,&q,10); unsigned ra[64],rv[64]; int nr=cnt;
      for(int i=0;i<cnt;i++){ ra[i]=strtol(q,&q,10); rv[i]=strtol(q,&q,10); mem[ra[i]]=rv[i]; }
      fgets(line,sizeof line,f); q=line+2; int nf=strtol(q,&q,10); unsigned fa[64],fv[64];
      for(int i=0;i<nf;i++){ fa[i]=strtol(q,&q,10); fv[i]=strtol(q,&q,10); }
      fgets(line,sizeof line,f); q=line+2; int ncy=strtol(q,&q,10); unsigned ca[64],cv[64]; char ck[64];
      for(int i=0;i<ncy;i++){ ca[i]=strtol(q,&q,10); cv[i]=strtol(q,&q,10); while(*q==' ')q++; ck[i]=*q; q++; }
      Cpu6502 c={ (uint16_t)pc,(uint8_t)a,(uint8_t)x,(uint8_t)y,(uint8_t)s,(uint8_t)p }; nc=0;
      int r=cpu_step(&c); int ok=(r==0)&&c.pc==epc&&c.s==es&&c.a==ea&&c.x==ex&&c.y==ey&&c.p==ep;
      for(int i=0;i<nf&&ok;i++) if(mem[fa[i]]!=fv[i]) ok=0;
      if(ok&&nc!=ncy) ok=0;
      for(int i=0;i<ncy&&ok;i++) if(cyc[i].a!=ca[i]||cyc[i].v!=cv[i]||cyc[i].k!=ck[i]) ok=0;
      n++; if(!ok){ if(fails++<2) printf("FAIL %s %s: pc %04x/%04x a %02x/%02x x %02x/%02x y %02x/%02x s %02x/%02x p %02x/%02x cycles %d/%d\n",argv[k],name,c.pc,epc,c.a,ea,c.x,ex,c.y,ey,c.s,es,c.p,ep,nc,ncy); }
      (void)off;(void)nr;
    }
    tot+=n; bad+=fails; if(fails) printf("%s: %d/%d failed\n",argv[k],fails,n); fclose(f);
  }
  printf("cpu6502 vs SingleStepTests nes6502: %ld cases, %ld failing opcode-files-cases\n",tot,bad); return bad!=0;
}
