/* rt_x.c - C side of the glue between translated code and the runtime: I/O helpers with exact access cycles, the slow path (interpreter) and the
 * computation of the cycle limit up to which translated code may run without looking at the PPU. */
#include <stdlib.h>
#include <stdio.h>
#include "rt.h"
#include "rt_x.h"
XCtx xrt_ctx;
extern uint32_t tab_win0[], tab_win1[], tab_win2[], tab_win3[], tab_win4[], tab_win5[], tab_win6[], tab_fix[];
extern void rt_unk_ent(void);
void ppu_sync_rt(uint64_t dots);
void rt_nmi_check(void);
static uint8_t nztab[16]={0,0,0,0,2,0,0,0,0x80,0,0,0,0x82,0,0,0};
static uint32_t *wintabs[7]={tab_win0,tab_win1,tab_win2,tab_win3,tab_win4,tab_win5,tab_win6};

static inline uint64_t sample_cycle_for_dots(uint64_t d){ return d<=5 ? 0 : (d-5+2)/3; }
#define FRAME_DOTS 89342u
/* cycle limit for translated code (see FACTS/DESIGN): blocks whose last cycle is <= limit cannot be interrupted by an NMI or miss a frame event */
uint64_t rt_compute_limit_at(uint64_t now){
    Ppu *p=&nes.ppu; uint64_t E=RT_NEVER;
    if(rt.need_halt) E=now;
    if(rt.need_cycle!=RT_NEVER && rt.need_cycle<E) E=rt.need_cycle;
    for(int i=0;i<rt.nq;i++) if(rt.q[i].lvl && rt.q[i].sample<E) E=rt.q[i].sample;
    /* PPU position of the last executed dot, as a linear index into the frame */
    int64_t cur=(int64_t)(p->line+1)*341+p->cx;
    int64_t iv=242LL*341+1;          /* the dot that sets the vblank flag */
    int64_t ifr=241LL*341;           /* the dot that completes a frame (line 240, cx 0) */
    int64_t dv=iv-cur, df=ifr-cur;
    if(dv<=0) dv+=FRAME_DOTS-1;      /* next frame; -1: an odd frame may be one dot shorter */
    if(df<=0) df+=FRAME_DOTS-1;
    uint64_t ev=sample_cycle_for_dots(p->dots+dv), ef=sample_cycle_for_dots(p->dots+df);
    if(ev<E) E=ev; if(ef<E) E=ef;
    return E;
}
static void load_mapper_ptrs(void){
    XCtx *c=&xrt_ctx; unsigned bank=nes.map.prg_lo>>14;
    c->win=(uint8_t*)nes.map.prg+nes.map.prg_lo; c->tabwin=(uint8_t*)wintabs[bank<7?bank:0]-0x20000;
}
static void trace_c(char t,uint16_t a,uint8_t v,uint64_t cyc){ if(rt.trace_hook){ rt.cyc=cyc; rt.trace_hook(t,a,v,-1); } }
uint32_t xrt_rd_c(uint32_t addr, int32_t d5, int32_t adj){
    uint64_t c=rt.limit-(int64_t)d5+adj; uint16_t a=(uint16_t)addr;
    rt.openbus=xrt_ctx.pad;
    uint8_t v=rt_io_rd(a,c);
    if(a>=0x2000 && a<0x8000) trace_c('R',a,v,c);
    return v;
}
int32_t xrt_wr_c(uint32_t addr, uint32_t val, int32_t d5, int32_t adj){
    uint64_t c=rt.limit-(int64_t)d5+adj; uint16_t a=(uint16_t)addr;
    if(0) fprintf(stderr,"wr %04x c=%llu limit=%llu d5=%d adj=%d\n",a,(unsigned long long)c,(unsigned long long)rt.limit,d5,adj);
    if(a>=0x2000) trace_c('W',a,(uint8_t)val,c);
    rt_io_wr(a,(uint8_t)val,c);
    if(a>=0x8000) load_mapper_ptrs();
    uint64_t nl=rt_compute_limit_at(c);
    int32_t delta=(int32_t)(nl-rt.limit); rt.limit=nl; return delta;
}
int xl_is_entry(uint16_t pc){
    if(pc>=0xC000) return (void*)tab_fix[pc-0xC000]!=(void*)rt_unk_ent;
    if(pc>=0x8000){ unsigned b=nes.map.prg_lo>>14; return b<7 && (void*)wintabs[b][pc-0x8000]!=(void*)rt_unk_ent; }
    return 0;
}
/* slow path: called with the translated registers saved in xrt_ctx (or d0==-1: initial entry).  Runs the interpreter for at least one
 * instruction and until the pc is a translated entry with some budget left; returns the pc, or -1 to leave the translated program. */
int32_t xrt_slow_c(int32_t pc_in){
    XCtx *c=&xrt_ctx; Cpu6502 *cpu=&rt.cpu;
    if(pc_in>=0){
        cpu->a=(uint8_t)c->d[0]; cpu->x=(uint8_t)c->d[1]; cpu->y=(uint8_t)c->d[2]; cpu->s=(uint8_t)c->d[3];
        cpu->p=(uint8_t)((c->nzc&0x82)|((c->d[4]&0xFF)?1:0)|(c->vflag&0x40)|(c->misc&0x0C)|0x20); cpu->pc=(uint16_t)pc_in;
        rt.cyc=rt.limit-(int64_t)(int32_t)c->d[5];
        rt_nmi_check();
    }
    int steps=0; static uint64_t dlo=0,dhi=0; if(!dhi&&getenv("XDBGLO")){ dlo=strtoull(getenv("XDBGLO"),0,10); dhi=strtoull(getenv("XDBGHI"),0,10); } int dbg=dhi&&rt.cyc>dlo&&rt.cyc<dhi; if(dbg) fprintf(stderr,"slow in pc=%d cyc=%llu limit=%llu d5=%d\n",pc_in,(unsigned long long)rt.cyc,(unsigned long long)rt.limit,(int)c->d[5]);
    for(;;){
        if(dbg) fprintf(stderr,"  step pc=%04x cyc=%llu\n",cpu->pc,(unsigned long long)rt.cyc);
        if(rt_step()<0){ fprintf(stderr,"illegal opcode at %04x\n",cpu->pc); exit(3); }
        steps++;
        rt.limit=rt_compute_limit_at(rt.cyc);
        if(rt.frames_done>=c->frames_target) break;
        if(xl_is_entry(cpu->pc) && rt.limit>rt.cyc+24 && !rt.need_halt) break;
    }
    c->d[0]=cpu->a; c->d[1]=cpu->x; c->d[2]=cpu->y; c->d[3]=cpu->s; c->d[4]=(cpu->p&1)?0xFF:0;
    c->nzc=cpu->p&0x82; c->vflag=cpu->p&0x40; c->misc=cpu->p&0x0C;
    c->d[5]=(uint32_t)(int32_t)(rt.limit-rt.cyc);
    load_mapper_ptrs();
    if(rt.frames_done>=c->frames_target) return -1;
    return cpu->pc;
}
void xrt_run(long frames){
    XCtx *c=&xrt_ctx;
    c->frames_target=frames; c->ram=nes.ram; c->tabfix=(uint8_t*)tab_fix-0x30000; c->nztab=nztab; c->fixbase=(uint8_t*)nes.map.prg+7*16384-0x4000;
    load_mapper_ptrs(); rt.limit=rt_compute_limit_at(rt.cyc);
    extern void xrt_enter(void); xrt_enter();
}

/* debug: check every block entry against the reference trace (XCHK=trace file): the first instruction of the block must be fetched at cycle now0+1 */
static FILE *xchk; static uint64_t xchk_cyc; static long xchk_n;
void xrt_dbg_c(uint32_t pc, int32_t d5, int32_t tot){
    if(!xchk){ const char*f=getenv("XCHK"); if(!f) return; xchk=fopen(f,"rb"); }
    uint64_t now0=rt.limit-(int64_t)d5-tot;
    uint8_t b[16];
    for(;;){
        if(fread(b,1,16,xchk)!=16){ fprintf(stderr,"xchk: trace end\n"); exit(1); }
        if(b[0]!='X') continue;
        uint16_t a=b[2]|b[3]<<8; uint32_t cy=b[8]|b[9]<<8|b[10]<<16|(uint32_t)b[11]<<24;
        if(cy<now0+1) continue;
        if(cy!=now0+1||a!=pc){ fprintf(stderr,"xchk: block %04x entered at cycle %llu, reference has X %04x at %u (blocks checked %ld)\n",pc,(unsigned long long)now0+1,a,cy,xchk_n); exit(1); }
        break;
    }
    xchk_n++;
}
