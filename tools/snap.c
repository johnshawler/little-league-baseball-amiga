/* snap.c - state snapshot for the Amiga test builds (build_adf.sh with SNAPSHOT=file): runs the reference interpreter (rt_run_interp) for N frames with a frame
 * script and writes the runtime state (nes, rt, rt_apu) as raw bytes, so that an Amiga build can start directly in a later scene (menu, "Watch Game") instead of
 * spending minutes of real A1200 time on the way there.  Must be compiled like the Amiga build (m68k, -DPPU_FB8 -DRT_AUDIO=1 -DAPU_Q_*): the structures are
 * copied byte for byte.  usage: snap game.nes frames script.txt out.snap
 * format: 'SNP1', sizeof(nes), sizeof(rt), sizeof(rt_apu) (big endian 32 bit), frames, then nes, rt, rt_apu */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/rt.h"
static long sf[8192]; static unsigned sm[8192]; static int ns;
static uint8_t input(int port, long frame){
    if(port) return 0; unsigned m=0; for(int i=0;i<ns;i++){ if(sf[i]<=frame) m=sm[i]; else break; }
    uint8_t b=0; if(m&0x100) b|=1; if(m&1) b|=2; if(m&4) b|=4; if(m&8) b|=8; if(m&0x10) b|=16; if(m&0x20) b|=32; if(m&0x40) b|=64; if(m&0x80) b|=128; return b;
}
static int8_t scratch[2048];
static uint64_t a_from;
static void frame(long f){   /* keep the APU event queues empty like the Amiga frame loop does (audio is not part of the snapshot's output) */
    uint64_t to=nes.ppu.dots/3-2; apu_render8(&rt_apu,a_from,to,scratch,480); a_from=to; (void)f;
}
static void put32(FILE*o,uint32_t v){ fputc(v>>24,o); fputc(v>>16,o); fputc(v>>8,o); fputc(v,o); }
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f); uint8_t*d=malloc(sz); if(fread(d,1,sz,f)!=(size_t)sz) return 1; fclose(f);
    long nf=atol(argv[2]); FILE*sc=fopen(argv[3],"r"); while(sc&&fscanf(sc,"%ld %x",&sf[ns],&sm[ns])==2) ns++;
    rt_init(d+16,d+16+131072); rt.input_hook=input; rt.frame_hook=frame;
    rt_run_interp(nf);
    /* leave the render position consistent: the Amiga build starts rendering at rt_apu.rpos */
    FILE*o=fopen(argv[4],"wb"); fwrite("SNP1",1,4,o); put32(o,sizeof nes); put32(o,sizeof rt); put32(o,sizeof rt_apu); put32(o,(uint32_t)rt.frames_done);
    fwrite(&nes,1,sizeof nes,o); fwrite(&rt,1,sizeof rt,o); fwrite(&rt_apu,1,sizeof rt_apu,o); put32(o,(uint32_t)(a_from>>32)); put32(o,(uint32_t)a_from); fclose(o);
    fprintf(stderr,"snapshot at frame %ld: nes %zu rt %zu apu %zu bytes\n",rt.frames_done,sizeof nes,sizeof rt,sizeof rt_apu);
    return 0;
}
