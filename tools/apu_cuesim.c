/* apu_cuesim: host simulation of the cue-driven Paula mode (runtime/apu.c apu_cue_frame + the channel/mapping of platform-a1200/audio.c) next to the exact mixer.
 *   build: gcc -O2 -Wno-unused -DPPU_FB8 -DRT_AUDIO=1 -o apu_cuesim tools/apu_cuesim.c runtime/rt_core.c runtime/cpu6502.c runtime/ppu_ref.c runtime/mapper_mmc1.c runtime/apu.c
 *   usage: apu_cuesim rom.nes frames script.txt exact.raw cue.raw [pal|ntsc]
 * exact.raw: int8 samples of apu_render8 (the PCM the exact mixer plays), cue.raw: int16, the cue mode's four Paula channels (waveform x volume/64, in the same counts) summed,
 * both at Paula's period 124 (same number of samples per frame).  Prints pitch / envelope statistics; tools/apu_cuecmp.py compares the two signals. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../runtime/rt.h"
#include "../runtime/audio_words.h"
static long sf[8192]; static unsigned sm[8192]; static int ns;
static uint8_t input(int port,long frame){ if(port) return 0; unsigned m=0; for(int i=0;i<ns;i++){ if(sf[i]<=frame) m=sm[i]; else break; }
    uint8_t b=0; if(m&0x100) b|=1; if(m&1) b|=2; if(m&4) b|=4; if(m&8) b|=8; if(m&0x10) b|=16; if(m&0x20) b|=32; if(m&0x40) b|=64; if(m&0x80) b|=128; return b; }
static FILE *fex, *fcu; static AudioWords aw; static uint64_t a_from; static unsigned pal=1;
/* Paula channel model: loops its waveform, latches AUDxLC/LEN at the loop end; one sample every `per` colour clocks */
typedef struct { const int8_t *cur; unsigned curlen; const int8_t *nxt; unsigned nxtlen; unsigned idx; int cnt; unsigned per, vol; int started; } Pc;
static Pc pc[4]; static const int8_t zerow[2]={0,0};
static const int hw[4]={0,1,3,2};      /* NES channel -> Paula channel: pulse1 0, pulse2 1, triangle 3, noise 2 (same as audio.c) */
static Apu tmp;
static double cents_sum[4], cents_max[4]; static long cents_n[4], hi_frames[4], act_frames[4];
static double env_err_sum[4], env_err_max[4]; static long env_n[4];
static const double K_PAL=3546895.0/1789773.0, K_NTSC=3579545.0/1789773.0;
static void frame(long f){
    unsigned w=aw_next(&aw), n=2*w; uint64_t to=nes.ppu.dots/3-2; static int8_t buf[1024]; static int16_t cb[1024]; ApuCue cue; int c;
    /* per channel mean/peak level from the exact events of this frame (copy of the APU: popping consumes them) */
    memcpy(&tmp,&rt_apu,sizeof tmp); apu_flush(&tmp,to);
    { uint32_t t; int ch,l; double peak[4]={0,0,0,0}; int lv[4]={0,0,0,0};
      for(c=0;c<4;c++) lv[c]=(c==0)?(tmp.rsq>>4):(c==1)?(tmp.rsq&15):(c==2)?(tmp.rtn>>4):(tmp.rtn&15);
      for(c=0;c<4;c++) peak[c]=lv[c];
      while(apu_pop_event(&tmp,to,&t,&ch,&l)){ if(l>peak[ch]) peak[ch]=l; }
      (void)peak; }
    apu_render8(&rt_apu,a_from,to,buf,n);
    a_from=to;
    for(unsigned i=0;i<n;i++){
        double acc=0; int left=124;
        while(left>0){ int sum=0; (void)sum;
            int step=left; for(c=0;c<4;c++){ Pc*p=&pc[hw[c]]; if(p->cnt<=0){ p->cnt=(int)p->per; } if(p->cnt<step) step=p->cnt; }
            for(c=0;c<4;c++){ Pc*p=&pc[hw[c]]; acc+=(double)step*p->cur[p->idx]*p->vol/64.0; p->cnt-=step;
                if(p->cnt<=0){ if(++p->idx>=p->curlen){ p->idx=0; p->cur=p->nxt; p->curlen=p->nxtlen; } } }
            left-=step; }
        cb[i]=(int16_t)lrint(acc/124.0*16.0);
    }
    fwrite(buf,1,n,fex); fwrite(cb,2,n,fcu);
    /* the cues of this frame's end are applied now and sound from the next frame on (the exact path would add its buffering latency on top) */
    apu_cue_frame(&rt_apu,to,pal,&cue);
    for(c=0;c<4;c++){
        int h=hw[c]; const ApuCueWave *wv=apu_cue_wave(cue.ch[c].wave);
        if(cue.ch[c].vol||(cue.hi>>c&1)) act_frames[c]++;
        if(cue.hi>>c&1) hi_frames[c]++;
        if(!pc[h].started){ pc[h].cur=wv->data; pc[h].curlen=wv->words*2; pc[h].idx=0; pc[h].cnt=0; pc[h].started=1; }   /* Paula starts a channel when DMA is enabled: the first wave plays at once */
        pc[h].nxt=wv->data; pc[h].nxtlen=wv->words*2; pc[h].per=cue.ch[c].per; pc[h].vol=cue.ch[c].vol;
    }

    /* pitch: rounding of the Paula period (and clamping) against the ideal period */
    { double K=pal?K_PAL:K_NTSC; ApuCueCh*cc=cue.ch; (void)cc;
      for(c=0;c<2;c++){ const ApuPulse*s=&rt_apu.sq[c]; if(!cue.ch[c].vol) continue; unsigned d=cue.ch[c].wave%3; double ideal=(s->period+1.0)*(1<<d)*K, r=1200*log2(ideal/cue.ch[c].per); cents_sum[c]+=fabs(r); if(fabs(r)>cents_max[c]) cents_max[c]=fabs(r); cents_n[c]++; }
      if(cue.ch[2].vol){ unsigned d=cue.ch[2].wave-12; double ideal=(rt_apu.tri.period+1.0)*(1<<d)*K, r=1200*log2(ideal/cue.ch[2].per); cents_sum[2]+=fabs(r); if(fabs(r)>cents_max[2]) cents_max[2]=fabs(r); cents_n[2]++; }
      if(cue.ch[3].vol){ unsigned d=(cue.ch[3].wave-16)%5; double ideal=(rt_apu.noi.period+1.0)*(1<<d)*K, r=1200*log2(ideal/cue.ch[3].per); cents_sum[3]+=fabs(r); if(fabs(r)>cents_max[3]) cents_max[3]=fabs(r); cents_n[3]++; } }
    (void)f;
}
int main(int argc,char**argv){
    if(argc<6){ fprintf(stderr,"usage: apu_cuesim rom frames script exact.raw cue.raw [pal|ntsc]\n"); return 2; }
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f); uint8_t*d=malloc(sz); if(fread(d,1,sz,f)!=(size_t)sz) return 1; fclose(f);
    long nf=atol(argv[2]); FILE*sc=fopen(argv[3],"r"); while(sc&&fscanf(sc,"%ld %x",&sf[ns],&sm[ns])==2) ns++;
    fex=fopen(argv[4],"wb"); fcu=fopen(argv[5],"wb"); pal=!(argc>6&&argv[6][0]=='n');
    aw_init(&aw,pal?11803:11912,pal?21423809u:7815439u,39375000u,124);
    for(int c=0;c<4;c++){ pc[c].cur=pc[c].nxt=zerow; pc[c].curlen=pc[c].nxtlen=2; pc[c].per=124; }
    rt_init(d+16,d+16+131072); rt.input_hook=input; rt.frame_hook=frame; apu_cue_init();
    rt_run_interp(nf);
    fclose(fex); fclose(fcu);
    { const char*nm[4]={"pulse1","pulse2","triangle","noise"}; for(int c=0;c<4;c++) printf("%-8s active frames %6ld  silenced (too high for Paula) %5ld  period rounding error: mean %.2f cents, max %.2f cents (%ld frames)\n",nm[c],act_frames[c],hi_frames[c],cents_n[c]?cents_sum[c]/cents_n[c]:0.0,cents_max[c],cents_n[c]); }
    return 0;
}
