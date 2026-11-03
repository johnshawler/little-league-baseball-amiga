/* rt_host: host test driver for the runtime in pure-interpreter mode.  usage: rt_host game.nes frames script.txt [trace.out]
 * Must produce a trace identical to ref/nesref (and the instrumented reference). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/rt.h"
#include "../runtime/audio_words.h"
#pragma pack(push,1)
typedef struct { char t; uint8_t v; uint16_t a; int32_t abs; uint32_t cyc; uint32_t fr; } TRec;
#pragma pack(pop)
static FILE *trc, *fbf; static long sf[8192]; static unsigned sm[8192]; static int ns;
static int rd_rec(FILE*f,TRec*r){ uint8_t b[16]; if(fread(b,1,16,f)!=16) return 0; r->t=(char)b[0]; r->v=b[1]; r->a=(uint16_t)(b[2]|b[3]<<8); r->abs=(int32_t)(b[4]|b[5]<<8|b[6]<<16|(uint32_t)b[7]<<24); r->cyc=b[8]|b[9]<<8|b[10]<<16|(uint32_t)b[11]<<24; r->fr=b[12]|b[13]<<8|b[14]<<16|(uint32_t)b[15]<<24; return 1; }
static FILE *cmpf; static long cmpn; static int cmp_fail;
/* on-line comparison against a reference trace (no big output file): X,W and R of $2002/$4016/$4017 */
static int nox, no2002; static int wanted(TRec*r){ if(no2002&&r->t=='R'&&r->a==0x2002) return 0; return (r->t=='X'&&!nox)||r->t=='W'||(r->t=='R'&&(r->a==0x2002||r->a==0x4016||r->a==0x4017||(r->a==0x4015&&getenv("CMP4015")))); }
static void trace(char t, uint16_t a, uint8_t v, int32_t ab){
    TRec r={t,v,a,ab,(uint32_t)rt.cyc,nes.ppu.frame}; if(t=='X'&&nox) return; if(t=='R'&&a==0x2002&&no2002) return;
    if(cmpf){ if(!wanted(&r)) return; TRec m; do{ if(!rd_rec(cmpf,&m)){ if(!cmp_fail){ fprintf(stderr,"reference trace ended at record %ld\n",cmpn); cmp_fail=1; exit(1);} } }while(!wanted(&m));
        if(m.t!=r.t||m.a!=r.a||m.v!=r.v||m.cyc!=r.cyc){ fprintf(stderr,"DIVERGE at record %ld: ref %c a=%04x v=%02x cyc=%u | mine %c a=%04x v=%02x cyc=%u\n",cmpn,m.t,m.a,m.v,m.cyc,r.t,r.a,r.v,r.cyc); exit(1); } cmpn++; return; }
    if(trc) fwrite(&r,sizeof r,1,trc); }
static uint8_t input(int port, long frame){
    unsigned m=0; for(int i=0;i<ns;i++){ if(sf[i]<=frame) m=sm[i]; else break; } if(port) m>>=16;
    uint8_t b=0; if(m&0x100) b|=1; if(m&1) b|=2; if(m&4) b|=4; if(m&8) b|=8; if(m&0x10) b|=16; if(m&0x20) b|=32; if(m&0x40) b|=64; if(m&0x80) b|=128; return b;
}
static long fbsel[2048]; static int nfbsel;     /* FBFRAMES=a,b,c : dump only these frames (hook index) */
static FILE *pcmf; static AudioWords aw; static uint64_t a_from;   /* PCMOUT=file PCMSTD=pal|ntsc: per frame the PCM chunk the Amiga build produces: u16 words, then 2*words signed bytes */
static void pcm_frame(void){
    static int8_t buf[1024]; unsigned w=aw_next(&aw); uint64_t to=nes.ppu.dots/3-2; apu_render8(&rt_apu,a_from,to,buf,2*w); a_from=to;
    fputc(w>>8,pcmf); fputc(w,pcmf); fwrite(buf,1,2*w,pcmf);
}
static long drawpat;   /* DRAWPAT=n: only every n-th frame is drawn (nes.ppu.draw), to test the state-only rendering */
static long oamdump=-1;   /* OAMDUMP=f : print OAM and PPU registers after frame f */
static long palvar_frames;
static void frame(long f){ if(nes.ppu.palvar){ palvar_frames++; if(getenv("PALVARLIST")) fprintf(stderr,"palvar frame %ld\n",f); nes.ppu.palvar=0; } if(f==oamdump){ Ppu*p=&nes.ppu; fprintf(stderr,"frame %ld ctrl %02x mask %02x fx %d\n",f,p->ctrl,p->mask,p->fx); for(int i=0;i<64;i++) fprintf(stderr,"%d:%02x,%02x,%02x,%02x ",i,p->oam[i*4],p->oam[i*4+1],p->oam[i*4+2],p->oam[i*4+3]); fprintf(stderr,"\npal:"); for(int i=0;i<32;i++) fprintf(stderr," %02x",p->pal[i]); fprintf(stderr,"\nfb[row][149..160]:"); for(int r=0;r<240;r++) { int any=0; for(int i=0;i<12;i++) if(p->fb[r*256+149+i]!=p->fb[(r?r-1:0)*256+149+i]) any=1; if(any||r==105){ fprintf(stderr,"\n r%d:",r); for(int i=0;i<12;i++) fprintf(stderr," %02x",(unsigned)p->fb[r*256+149+i]); } } fprintf(stderr,"\n"); } if(drawpat) nes.ppu.draw=((f+1)%drawpat==0); if(pcmf) pcm_frame(); if(fbf){ if(nfbsel){ int ok=0; for(int i=0;i<nfbsel;i++) if(fbsel[i]==f) ok=1; if(!ok) return; } static uint8_t buf[240*256*2]; for(int i=0;i<240*256;i++){
#ifdef PPU_FB5     /* palette index framebuffer: apply the palette / emphasis / gray state at the end of the frame (what the Amiga build does) */
        unsigned v=(nes.ppu.pal[nes.ppu.fb[i]&31]&(nes.ppu.gray?0x30:0x3F))|(nes.ppu.emph<<6); buf[2*i]=(uint8_t)v; buf[2*i+1]=(uint8_t)(v>>8);
#else
        buf[2*i]=(uint8_t)nes.ppu.fb[i]; buf[2*i+1]=(uint8_t)(nes.ppu.fb[i]>>8);
#endif
    }
    uint8_t h[8]; for(int i=0;i<8;i++) h[i]=(uint8_t)((uint64_t)f>>(8*i)); fwrite(h,1,8,fbf); fwrite(buf,1,sizeof buf,fbf); } }
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f); uint8_t*d=malloc(sz); if(fread(d,1,sz,f)!=(size_t)sz) return 1; fclose(f);
    long nf=atol(argv[2]); FILE*sc=fopen(argv[3],"r"); while(sc&&fscanf(sc,"%ld %x",&sf[ns],&sm[ns])==2) ns++;
    if(argc>4 && strcmp(argv[4],"-")) trc=fopen(argv[4],"wb"); nox=getenv("CMP_NOX")!=0; no2002=getenv("CMP_NO2002")!=0; if(getenv("TRACECMP")) cmpf=fopen(getenv("TRACECMP"),"rb"); if(getenv("FBDUMP")) fbf=fopen(getenv("FBDUMP"),"wb"); if(getenv("FBFRAMES")){ char*p=getenv("FBFRAMES"); while(*p&&nfbsel<2048){ fbsel[nfbsel++]=strtol(p,&p,10); if(*p==',') p++; } }
    if(getenv("OAMDUMP")) oamdump=atol(getenv("OAMDUMP"));
    if(getenv("DRAWPAT")) drawpat=atol(getenv("DRAWPAT"));
    if(getenv("PCMOUT")){ pcmf=fopen(getenv("PCMOUT"),"wb"); const char*s=getenv("PCMSTD"); int pal=!s||s[0]=='p'; aw_init(&aw,pal?11803:11912,pal?21423809u:7815439u,39375000u,124); }
    extern int rt_fast_ppu; if(getenv("RT_NOFAST")||getenv("PPU_REF")) rt_fast_ppu=0;
    extern int rt_ppu_check; extern long rt_chk_n; if(getenv("PPU_CHECK")) rt_ppu_check=1;   /* PPU_CHECK=1: compare the fast PPU model with the per-dot model at every access and frame end */
    rt_init(d+16,d+16+131072); rt.input_hook=input; rt.frame_hook=frame; if(trc||cmpf) rt.trace_hook=trace;
#ifdef XRT
    { extern void xrt_run(long); xrt_run(nf); }
#else
    rt_run_interp(nf);
#endif
     if(cmpf) fprintf(stderr,"IDENTICAL to the reference trace over %ld records\n",cmpn); { extern long ppu_fast_lines, ppu_exotic; if(rt_ppu_check) fprintf(stderr,"PPU_CHECK: %ld comparisons passed, %ld exotic situations\n",rt_chk_n,ppu_exotic); fprintf(stderr,"frames with a palette/emphasis change during visible lines: %ld\n",palvar_frames); fprintf(stderr,"fast lines %ld\n",ppu_fast_lines); } fprintf(stderr,"done: %ld frames, %llu cycles\n",rt.frames_done,(unsigned long long)rt.cyc); return 0;
}
