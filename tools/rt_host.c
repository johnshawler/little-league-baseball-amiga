/* rt_host: host test driver for the runtime in pure-interpreter mode.  usage: rt_host game.nes frames script.txt [trace.out]
 * Must produce a trace identical to ref/nesref (and the instrumented reference). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/rt.h"
#pragma pack(push,1)
typedef struct { char t; uint8_t v; uint16_t a; int32_t abs; uint32_t cyc; uint32_t fr; } TRec;
#pragma pack(pop)
static FILE *trc, *fbf; static long sf[8192]; static unsigned sm[8192]; static int ns;
static int rd_rec(FILE*f,TRec*r){ uint8_t b[16]; if(fread(b,1,16,f)!=16) return 0; r->t=(char)b[0]; r->v=b[1]; r->a=(uint16_t)(b[2]|b[3]<<8); r->abs=(int32_t)(b[4]|b[5]<<8|b[6]<<16|(uint32_t)b[7]<<24); r->cyc=b[8]|b[9]<<8|b[10]<<16|(uint32_t)b[11]<<24; r->fr=b[12]|b[13]<<8|b[14]<<16|(uint32_t)b[15]<<24; return 1; }
static FILE *cmpf; static long cmpn; static int cmp_fail;
/* on-line comparison against a reference trace (no big output file): X,W and R of $2002/$4016/$4017 */
static int nox; static int wanted(TRec*r){ return (r->t=='X'&&!nox)||r->t=='W'||(r->t=='R'&&(r->a==0x2002||r->a==0x4016||r->a==0x4017||(r->a==0x4015&&getenv("CMP4015")))); }
static void trace(char t, uint16_t a, uint8_t v, int32_t ab){
    TRec r={t,v,a,ab,(uint32_t)rt.cyc,nes.ppu.frame}; if(t=='X'&&nox) return;
    if(cmpf){ if(!wanted(&r)) return; TRec m; do{ if(!rd_rec(cmpf,&m)){ if(!cmp_fail){ fprintf(stderr,"reference trace ended at record %ld\n",cmpn); cmp_fail=1; exit(1);} } }while(!wanted(&m));
        if(m.t!=r.t||m.a!=r.a||m.v!=r.v||m.cyc!=r.cyc){ fprintf(stderr,"DIVERGE at record %ld: ref %c a=%04x v=%02x cyc=%u | mine %c a=%04x v=%02x cyc=%u\n",cmpn,m.t,m.a,m.v,m.cyc,r.t,r.a,r.v,r.cyc); exit(1); } cmpn++; return; }
    if(trc) fwrite(&r,sizeof r,1,trc); }
static uint8_t input(int port, long frame){
    if(port) return 0; unsigned m=0; for(int i=0;i<ns;i++){ if(sf[i]<=frame) m=sm[i]; else break; }
    uint8_t b=0; if(m&0x100) b|=1; if(m&1) b|=2; if(m&4) b|=4; if(m&8) b|=8; if(m&0x10) b|=16; if(m&0x20) b|=32; if(m&0x40) b|=64; if(m&0x80) b|=128; return b;
}
static void frame(long f){ if(fbf){ fwrite(&f,sizeof f,1,fbf); fwrite(nes.ppu.fb,2,240*256,fbf); } }
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f); uint8_t*d=malloc(sz); if(fread(d,1,sz,f)!=(size_t)sz) return 1; fclose(f);
    long nf=atol(argv[2]); FILE*sc=fopen(argv[3],"r"); while(sc&&fscanf(sc,"%ld %x",&sf[ns],&sm[ns])==2) ns++;
    if(argc>4 && strcmp(argv[4],"-")) trc=fopen(argv[4],"wb"); nox=getenv("CMP_NOX")!=0; if(getenv("TRACECMP")) cmpf=fopen(getenv("TRACECMP"),"rb"); if(getenv("FBDUMP")) fbf=fopen(getenv("FBDUMP"),"wb");
    rt_init(d+16,d+16+131072); rt.input_hook=input; rt.frame_hook=frame; if(trc||cmpf) rt.trace_hook=trace;
#ifdef XRT
    { extern void xrt_run(long); xrt_run(nf); }
#else
    rt_run_interp(nf);
#endif
     if(cmpf) fprintf(stderr,"IDENTICAL to the reference trace over %ld records\n",cmpn); fprintf(stderr,"done: %ld frames, %llu cycles\n",rt.frames_done,(unsigned long long)rt.cyc); return 0;
}
