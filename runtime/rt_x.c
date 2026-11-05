/* rt_x.c - C side of the glue between translated code and the runtime: I/O helpers with exact access cycles, the slow path (interpreter) and the
 * computation of the cycle limit up to which translated code may run without looking at the PPU. */
#ifdef AMIGA     /* freestanding Amiga build: no libc, no environment, no files; debug code is compiled out; an illegal opcode ends in rt_fatal() */
extern void rt_fatal(int code) __attribute__((noreturn));
#else
#include <stdlib.h>
#include <stdio.h>
#endif
#include "rt.h"
#include "rt_x.h"
XCtx xrt_ctx;
#include <stddef.h>
#ifdef __mc68000__     /* offsets used by the assembler fast paths in rt_x.s */
_Static_assert(offsetof(Nes,map.shift)==8 && offsetof(Nes,map.count)==9 && offsetof(Nes,joy)==32 && offsetof(Nes,joy_shift)==36 && offsetof(Nes,strobe)==38, "nes layout used by rt_x.s");
_Static_assert(offsetof(Rt,openbus)==XRT_OFF_OPENBUS, "rt layout used by rt_x.s");
#endif
extern uint32_t tab_win0[], tab_win1[], tab_win2[], tab_win3[], tab_win4[], tab_win5[], tab_win6[], tab_fix[];
extern void rt_unk_ent(void);
void ppu_sync_rt(uint64_t dots);
void rt_nmi_check(void);
static uint8_t nztab[16]={0,0,0,0,2,0,0,0,0x80,0,0,0,0x82,0,0,0};
static uint32_t *wintabs[7]={tab_win0,tab_win1,tab_win2,tab_win3,tab_win4,tab_win5,tab_win6};

static void load_mapper_ptrs(void){
    XCtx *c=&xrt_ctx; unsigned bank=nes.map.prg_lo>>14;
    c->win=(uint8_t*)nes.map.prg+nes.map.prg_lo; c->tabwin=(uint8_t*)wintabs[bank<7?bank:0]-0x20000;
}
static void trace_c(char t,uint16_t a,uint8_t v,uint64_t cyc){ if(rt.trace_hook){ rt.cyc=cyc; rt.trace_hook(t,a,v,-1); } }
/* trace records for the assembler fast paths (host verification builds only: rt_x.s -DXRT_TRACE) */
void xrt_trace_c(uint32_t type, uint32_t addr, uint32_t val, int32_t d5, int32_t adj){ if(rt.trace_hook) trace_c((char)type,(uint16_t)addr,(uint8_t)val,rt.limit-(int64_t)d5+adj); }
extern long xrt_cnt_rd[12], xrt_cnt_wr[12], xrt_hist_rd[256], xrt_hist_wr[256];       /* XRT_STATS: accesses that reach the C helpers: PPU registers 0..7, 8 = APU/pads, 9 = $4014, 10 = $4016, 11 = other / mapper */
#ifdef PPU_COUNT
#define CNTX2(x) ((x)++)
#else
#define CNTX2(x) ((void)0)
#endif
static inline int cnt_idx(uint16_t a){ return a<0x2000?11 : a<0x4000 ? (a&7) : a==0x4014?9 : a==0x4016?10 : a<0x4020?8 : 11; }
uint32_t xrt_rd_c(uint32_t addr, int32_t d5, int32_t adj){
    uint64_t c=rt.limit-(int64_t)d5+adj; uint16_t a=(uint16_t)addr; CNTX2(xrt_cnt_rd[cnt_idx(a)]); CNTX2(xrt_hist_rd[a>>8]);
    rt.openbus=xrt_ctx.pad;
    uint8_t v=rt_io_rd(a,c);
    if(a>=0x2000 && a<0x8000) trace_c('R',a,v,c);
    return v;
}
#ifdef AMIGA
#define XCHK 0
#else
extern int rt_ppu_check;
#define XCHK rt_ppu_check
#endif
int32_t xrt_wr_c(uint32_t addr, uint32_t val, int32_t d5, int32_t adj){
    uint64_t c=rt.limit-(int64_t)d5+adj; uint16_t a=(uint16_t)addr; CNTX2(xrt_cnt_wr[cnt_idx(a)]); CNTX2(xrt_hist_wr[a>>8]);
    if(a>=0x8000){          /* mapper write (the fifth of a serial load, or a reset): no layers in between */
        trace_c('W',a,(uint8_t)val,c);
        if(mmc1_write_ppu_effect(&nes.map,a,(uint8_t)val)){ ppu_sync_rt(3*c+4); nes.ppu.ar_wr=1; }
        mmc1_write(&nes.map,a,(uint8_t)val); rt.openbus=(uint8_t)val; load_mapper_ptrs();
#ifndef AMIGA
        if(XCHK){ extern Nes nes_ref; nes_ref.map=nes.map; }
#endif
        if(!rt.ldirty) return 0;
        rt.ldirty=0; uint64_t nl=rt_compute_limit_at(c); int32_t delta=(int32_t)(nl-rt.limit); rt.limit=nl; return delta;
    }
    if(a>=0x2000) trace_c('W',a,(uint8_t)val,c);
    rt_io_wr(a,(uint8_t)val,c);
    if(a>=0x8000) load_mapper_ptrs();
    if(!rt.ldirty) return 0;                     /* nothing that determines the cycle limit has changed */
    rt.ldirty=0;
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
extern long xrt_st_steps, xrt_st_entries, xrt_st_dma;
extern uint64_t lazy_limit; extern int rt_lazy;
int32_t xrt_slow_c(int32_t pc_in){ xrt_st_entries++;
    XCtx *c=&xrt_ctx; Cpu6502 *cpu=&rt.cpu;
    if(pc_in>=0){
        cpu->a=(uint8_t)c->d[0]; cpu->x=(uint8_t)c->d[1]; cpu->y=(uint8_t)c->d[2]; cpu->s=(uint8_t)c->d[3];
        cpu->p=(uint8_t)((c->nzc&0x82)|((c->d[4]&0xFF)?1:0)|(c->vflag&0x40)|(c->misc&0x0C)|0x20); cpu->pc=(uint16_t)pc_in;
        rt.cyc=rt.limit-(int64_t)(int32_t)c->d[5];
        if(rt.ldirty){ rt.limit=rt_compute_limit_at(rt.cyc); rt.ldirty=0; }      /* inputs of the limit changed since it was computed (queue, DMA) */
        rt_lazy=1; lazy_limit=rt.limit;
        rt_nmi_check();
    }
#ifndef AMIGA
    static uint64_t dlo=0,dhi=0; if(!dhi&&getenv("XDBGLO")){ dlo=strtoull(getenv("XDBGLO"),0,10); dhi=strtoull(getenv("XDBGHI"),0,10); } int dbg=dhi&&rt.cyc>dlo&&rt.cyc<dhi; if(dbg) fprintf(stderr,"slow in pc=%d cyc=%llu limit=%llu d5=%d\n",pc_in,(unsigned long long)rt.cyc,(unsigned long long)rt.limit,(int)c->d[5]);
#endif
    long st0=xrt_st_steps; uint64_t cyc0=rt.cyc, lim0=rt.limit; int pc0=pc_in;
    for(;;){
#ifndef AMIGA
        if(dbg) fprintf(stderr,"  step pc=%04x cyc=%llu\n",cpu->pc,(unsigned long long)rt.cyc);
        xrt_st_steps++; if(rt.need_halt) xrt_st_dma++; if(rt_step()<0){ fprintf(stderr,"illegal opcode at %04x\n",cpu->pc); exit(3); }
#else
        if(rt_step()<0) rt_fatal(3);          /* undocumented opcode: never executed by this game */
#endif
        rt.limit=lazy_limit;                       /* maintained by rt_step: recomputed only when an event or an access can have changed it */
        if(rt.frames_done>=c->frames_target) break;
        if(xl_is_entry(cpu->pc) && rt.limit>rt.cyc+24 && !rt.need_halt) break;
    }
#ifndef AMIGA
    if(getenv("XRT_LONG") && xrt_st_steps-st0>0 && rt.frames_done>=2000 && rt.frames_done<2003) fprintf(stderr,"long slow: %ld steps pc_in=%d cyc %llu limit %llu -> exit cyc %llu limit %llu pc %04x\n",xrt_st_steps-st0,pc0,(unsigned long long)cyc0,(unsigned long long)lim0,(unsigned long long)rt.cyc,(unsigned long long)rt.limit,cpu->pc);
#endif
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
    load_mapper_ptrs(); rt.limit=rt_compute_limit_at(rt.cyc); rt.ldirty=0; rt_lazy=1; lazy_limit=rt.limit;
    extern void xrt_enter(void); xrt_enter();
}

#ifndef AMIGA
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
#else
void xrt_dbg_c(uint32_t pc, int32_t d5, int32_t tot){ (void)pc; (void)d5; (void)tot; }
#endif

/* skip the polls of a $2002 wait loop that cannot see a change yet.  kind 0: BIT $2002 / BVS self (waits for the sprite-0 flag to clear, 7 cycles per
 * iteration); kind 1: BIT $2002 / BMI out / BVC self (waits for vblank or the sprite-0 hit, 9 cycles).  Called when the loop is about to be taken
 * again; returns the number of cycles to skip (a whole number of iterations, all of which would have read "not yet"). */
extern int rt_fast_ppu;
uint64_t ppu_predict_hit(Nes *n);
extern long xrt_cnt_skip, xrt_cnt_skip_taken;
#ifdef PPU_COUNT
#define CNTX(x) ((x)++)
#else
#define CNTX(x) ((void)0)
#endif
int32_t xrt_skip_c(int32_t kind, int32_t d5){ CNTX(xrt_cnt_skip);
    Ppu *p=&nes.ppu; uint64_t now=rt.limit-(int64_t)d5;
    if(!rt_fast_ppu) return 0;
    ppu_sync_rt(3*now+5);
    if(p->rend!=p->rend_prev||p->need_update) return 0;
    if(kind==1 && p->line>=240) return 0;               /* the hit of the next frame is not predicted from inside vblank */
    int64_t cur=(int64_t)(p->line+1)*341+p->cx; uint64_t T=0; int Li=(kind==1)?10:7;
    uint64_t tvbl=p->dots+((242LL*341+1)-cur>0 ? (242LL*341+1)-cur : (242LL*341+1)-cur+89341);
    if(kind==0){
        if(!p->sp0hit) return 0;                        /* flag already clear: the next poll exits */
        T=p->dots+(89342-cur);                          /* dot that clears the flag: first dot of the pre-render line */
        if(tvbl<T) T=tvbl;                              /* (never skip over the vblank start) */
    } else if(kind==2){
        if(p->vbl) return 0;                            /* flag already set: the next poll exits */
        T=tvbl;                                         /* BIT $2002 / BPL self: waits for the vblank flag */
    } else {
        uint64_t th=ppu_predict_hit(&nes); T=tvbl; if(th && th<T) T=th;
    }
    uint64_t c0=now+4, cmin=(T<=4)?0:rt_div3(T-2);          /* first read that can see the change */
    if(cmin<=c0) return 0;
#ifndef AMIGA
    if(getenv("XSKIPDBG")&&now>10273000&&now<10290000) fprintf(stderr,"skip kind %d now %llu line %d cx %d T %llu tvbl %llu cmin %llu\n",(int)kind,(unsigned long long)now,p->line,p->cx,(unsigned long long)T,(unsigned long long)tvbl,(unsigned long long)cmin);
#endif
    uint64_t n=(cmin-c0+Li-1)/Li;
    if(rt.limit<=now+64) return 0;
    uint64_t nmax=(rt.limit-now-64)/Li; if(n>nmax) n=nmax;
    CNTX(xrt_cnt_skip_taken); return (int32_t)(n*Li);
}
