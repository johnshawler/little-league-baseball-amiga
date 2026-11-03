/* apu_verify: cycle-exact comparison of the APU channel level-change events against the instrumented Mesen (tools/discovery/mesen-apu.patch),
 * and rendering of the 48 kHz audio for comparison with Mesen's PCM.
 *   build: gcc -O2 -DRT_AUDIO=1 -Wno-unused -o apu_verify tools/apu_verify.c runtime/rt_core.c runtime/cpu6502.c runtime/ppu_ref.c runtime/mapper_mmc1.c runtime/apu.c
 *   usage: apu_verify rom.nes frames script.txt mesen_events.bin [out48k.raw]
 * mesen_events.bin: 8-byte records {u32 bus cycle, u8 channel, u8 level, u16 pad}, may be a FIFO (nothing big is written to disk).
 * The game runs on runtime/rt_core.c (interpreter mode) with the APU attached; at every frame end the events are drained. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../runtime/rt.h"
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static double t_render; static uint64_t n_samples;
typedef struct { uint32_t t; uint8_t l; } E;
typedef struct { E *v; size_t n, cap; } Vec;
static void push(Vec *x, uint32_t t, uint8_t l){   /* normalising push: same-cycle events collapse to the last level, no-change events vanish */
    if(x->n && x->v[x->n-1].t==t){ x->v[x->n-1].l=l; if(x->n>1 ? x->v[x->n-2].l==l : l==0){ x->n--; } return; }
    unsigned prev = x->n ? x->v[x->n-1].l : 0; if(prev==l) return;
    if(x->n==x->cap){ x->cap=x->cap?x->cap*2:1<<16; x->v=realloc(x->v,x->cap*sizeof(E)); }
    x->v[x->n].t=t; x->v[x->n].l=l; x->n++;
}
static Vec mine[4], mes[4];
static long sf[8192]; static unsigned sm[8192]; static int ns;
static FILE *pcm; static uint64_t rpos; static long maxev[4], frev[4];
#define RATE 48000
static uint8_t input(int port,long frame){ if(port) return 0; unsigned m=0; for(int i=0;i<ns;i++){ if(sf[i]<=frame) m=sm[i]; else break; }
    uint8_t b=0; if(m&0x100) b|=1; if(m&1) b|=2; if(m&4) b|=4; if(m&8) b|=8; if(m&0x10) b|=16; if(m&0x20) b|=32; if(m&0x40) b|=64; if(m&0x80) b|=128; return b; }
static void frame(long f){
    uint32_t t; int ch, l; long cnt[4]={0,0,0,0};
    if(pcm){   /* sample k covers the bus cycles [floor(k*HZ/RATE), floor((k+1)*HZ/RATE)); render the samples completed by this frame end */
        static uint64_t k0; static int16_t buf[8192];
        uint64_t k1=rt.cyc*RATE/APU_CPU_HZ;
        while(k0<k1){ uint64_t k=k0+(k1-k0>8192?8192:k1-k0);
            double t0=now(); apu_render16(&rt_apu,k0*APU_CPU_HZ/RATE,k*APU_CPU_HZ/RATE,buf,(unsigned)(k-k0)); t_render+=now()-t0; n_samples+=k-k0; fwrite(buf,2,(size_t)(k-k0),pcm); k0=k; }
        return;
    }
    apu_flush(&rt_apu, rt.cyc);
    while(apu_pop_event(&rt_apu, rt.cyc, &t,&ch,&l)){ push(&mine[ch],t,(uint8_t)l); cnt[ch]++; }
    for(int c=0;c<4;c++) if(cnt[c]>maxev[c]) maxev[c]=cnt[c];
}
int main(int argc,char**argv){
    if(argc<5){ fprintf(stderr,"usage\n"); return 2; }
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f); uint8_t*d=malloc(sz); if(fread(d,1,sz,f)!=(size_t)sz) return 1; fclose(f);
    long nf=atol(argv[2]); FILE*sc=fopen(argv[3],"r"); while(sc&&fscanf(sc,"%ld %x",&sf[ns],&sm[ns])==2) ns++;
    FILE*me=strcmp(argv[4],"-")?fopen(argv[4],"rb"):0; if(!me&&strcmp(argv[4],"-")){ perror("mesen events"); return 1; }
    if(me){ struct { uint32_t cc; uint8_t ch, lvl; uint16_t pad; } r; size_t raw=0; while(fread(&r,8,1,me)==1){ raw++; if(r.ch<4) push(&mes[r.ch],r.cc,r.lvl); } fprintf(stderr,"mesen: %zu raw events\n",raw); }
    if(getenv("PCMOUT")) pcm=fopen(getenv("PCMOUT"),"wb");   /* render mode: mono int16 48 kHz, no event comparison */
    rt_init(d+16,d+16+131072); rt.input_hook=input; rt.frame_hook=frame;
    rt_run_interp(nf);
    if(pcm){ fclose(pcm); fprintf(stderr,"rendered, dropped %u; apu_render16 (incl. event generation): %.3f s for %llu samples = %.1f ns/sample, %.2f ms per 60 Hz frame\n",rt_apu.dropped,t_render,(unsigned long long)n_samples,t_render/n_samples*1e9,t_render/nf*1e3); return 0; }
    { uint32_t t; int ch,l; apu_flush(&rt_apu,rt.cyc); while(apu_pop_event(&rt_apu,rt.cyc,&t,&ch,&l)) push(&mine[ch],t,(uint8_t)l); }
    if(argc>5){ /* dump my normalised events merged by time: {u32 cc,u8 ch,u8 lvl,u16 0} */
        FILE*o=fopen(argv[5],"wb"); size_t ix[4]={0,0,0,0};
        for(;;){ int b=-1; for(int c=0;c<4;c++) if(ix[c]<mine[c].n && (b<0||mine[c].v[ix[c]].t<mine[b].v[ix[b]].t)) b=c; if(b<0)break;
            struct { uint32_t cc; uint8_t ch,lvl; uint16_t pad; } r={mine[b].v[ix[b]].t,(uint8_t)b,mine[b].v[ix[b]].l,0}; fwrite(&r,8,1,o); ix[b]++; }
        fclose(o); }
    uint32_t horizon=(uint32_t)rt.cyc-40000; int bad=0; const char*nm[4]={"pulse1","pulse2","triangle","noise"};
    fprintf(stderr,"cycles %llu, horizon %u, dropped %u\n",(unsigned long long)rt.cyc,horizon,rt_apu.dropped);
    for(int c=0;c<4;c++){
        size_t i=0,j=0,ok=0; 
        for(;;){
            int ha = i<mine[c].n && mine[c].v[i].t<horizon, hb = j<mes[c].n && mes[c].v[j].t<horizon;
            if(!ha&&!hb) break;
            if(!ha||!hb||mine[c].v[i].t!=mes[c].v[j].t||mine[c].v[i].l!=mes[c].v[j].l){
                printf("%s: MISMATCH after %zu equal events: mine %s t=%u l=%u | mesen %s t=%u l=%u\n",nm[c],ok,ha?"":"(end)",ha?mine[c].v[i].t:0,ha?mine[c].v[i].l:0,hb?"":"(end)",hb?mes[c].v[j].t:0,hb?mes[c].v[j].l:0); bad=1; break; }
            i++;j++;ok++;
        }
        printf("%-8s events compared: %zu (mine total %zu, mesen total %zu), max events per frame %ld  %s\n",nm[c],ok,mine[c].n,mes[c].n,maxev[c],bad?"":"IDENTICAL");
    }
    return bad;
}
