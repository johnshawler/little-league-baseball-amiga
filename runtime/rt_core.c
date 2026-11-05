/* rt_core.c - runtime time model.  The PPU is synchronised lazily: it is run only up to the dot count that is visible to a bus access, and the
 * NMI edge logic is evaluated from queued level transitions, which reproduces the reference emulator's per-cycle sampling exactly:
 *   access in CPU cycle c sees PPU dots executed = 3c+4 (StartCpuCycle runs 2 dots), the end of cycle c has run 3c+5 dots;
 *   the NMI line is sampled at the end of every cycle, an edge sets needNmi, the CPU takes the NMI after an instruction whose
 *   last cycle is L iff needNmi was set at the end of cycle <= L-1. */
#include "rt.h"
#include "arender.h"
Rt rt;
long xrt_st_steps, xrt_st_entries, xrt_st_dma;     /* slow path statistics (rt_x.c, printed by tools/rt_host.c with XRT_STATS=1) */
Apu rt_apu;
#ifdef AMIGA
#define RT_CHK 0
#else
#define RT_CHK rt_ppu_check
#endif
#ifndef RT_AUDIO
#define RT_AUDIO 0     /* 1: simulate the channel sequencers and queue audio events (the audio consumer must then call apu_render*() regularly) */
#endif

static inline uint64_t sample_cycle_for_dots(uint64_t d){ return d<=5 ? 0 : rt_div3(d-3); }   /* smallest k with 3k+5 >= d */

static void q_push(uint64_t sample, uint64_t key, uint8_t lvl){
    rt.ldirty=1;
    int i=rt.nq; if(i==8){ i=7; }
    while(i>0 && rt.q[i-1].key>key){ rt.q[i]=rt.q[i-1]; i--; }
    rt.q[i].sample=sample; rt.q[i].key=key; rt.q[i].lvl=lvl; if(rt.nq<8) rt.nq++;
}
/* process queued level transitions whose sample cycle <= upto */
static void nmi_advance(uint64_t upto){
    while(rt.nq && rt.q[0].sample<=upto){
        uint64_t k=rt.q[0].sample; uint8_t lv=rt.q[0].lvl; int n=1;
        while(n<rt.nq && rt.q[n].sample==k){ lv=rt.q[n].lvl; n++; }          /* only the final level at the sample point counts */
        if(lv && !rt.lvl_sampled && rt.need_cycle==RT_NEVER){ rt.need_cycle=k; rt.ldirty=1; }
        rt.lvl_sampled=lv;
        for(int i=n;i<rt.nq;i++) rt.q[i-n]=rt.q[i];
        rt.nq-=n;
    }
}

void nes_input_poll(void){
    uint8_t b0=0,b1=0;
    if(rt.input_hook){ b0=rt.input_hook(0,rt.frames_done); b1=rt.input_hook(1,rt.frames_done); }
    nes.joy[0]=b0; nes.joy[1]=b1;
}

void ppu_fp_run(Nes*n, uint64_t dots);
int rt_fast_ppu=1;     /* 1: closed-form PPU model (ppu_fp_run); 0: the per-dot reference model alone (host: RT_NOFAST=1) */
#ifndef AMIGA
/* host self-check (PPU_CHECK=1): a second PPU instance runs the per-dot reference model; both are compared at every synchronisation, register access and
 * frame end (state, flags, OAM, palette, name tables, and the frame buffer of drawn frames) */
#include <stdio.h>
#include <stdlib.h>
extern int ppu_noinput;
Nes nes_ref; int rt_ppu_check; long rt_chk_n; long ar_chk_frames, ar_chk_exact;
static void chk_fail(const char*what,const char*field,long a,long b){
    Ppu*p=&nes.ppu; fprintf(stderr,"PPU_CHECK FAIL (%s): %s fast=%ld ref=%ld at frame %u line %d cx %d dots %llu cyc %llu (check #%ld)\n",what,field,a,b,p->frame,p->line,p->cx,(unsigned long long)p->dots,(unsigned long long)rt.cyc,rt_chk_n); exit(4);
}
#define CK(f) do{ if((long)p->f!=(long)q->f) chk_fail(what,#f,(long)p->f,(long)q->f); }while(0)
static void chk_cmp(const char*what,int fbcmp){
    Ppu*p=&nes.ppu,*q=&nes_ref.ppu; rt_chk_n++;
    CK(dots); CK(line); CK(cx); CK(frame); CK(ctrl); CK(mask); CK(vbl); CK(sp0hit); CK(spovf); CK(v); CK(t); CK(fx); CK(w); CK(rdbuf); CK(openbus); CK(nmi_level);
    CK(prevent_vbl); CK(rend); CK(rend_prev); CK(need_update); CK(upd_delay); if(p->upd_delay) CK(upd_addr); CK(frame_done);
    if(!(p->rend && p->line>=0 && p->line<240 && p->cx>=65 && p->cx<=256)) CK(oamaddr);
    if(memcmp(p->pal,q->pal,32)) chk_fail(what,"pal",0,1);
    if(memcmp(p->nt,q->nt,2048)) chk_fail(what,"nt",0,1);
    if(memcmp(p->oam,q->oam,256)) chk_fail(what,"oam",0,1);
    if(fbcmp && p->draw){ int nd=0; for(int i=0;i<240*256;i++) if((!p->ar_on || ar_scene.exact[i>>8]) && p->fb[i]!=q->fb[i]){ if(nd<12) fprintf(stderr,"PPU_CHECK FAIL (%s): framebuffer differs at row %d x %d: fast=%02x ref=%02x (frame %u) ctrl %02x mask %02x fx %d\n",what,i>>8,i&255,(unsigned)p->fb[i],(unsigned)q->fb[i],p->frame,p->ctrl,p->mask,p->fx); nd++; } if(nd){ fprintf(stderr,"%d pixels differ\n",nd); exit(4); } }
}
static void chk_advance(uint64_t to){
    Ppu*q=&nes_ref.ppu; q->draw=nes.ppu.draw; ppu_noinput=1; while(q->dots<to) ppu_step(&nes_ref); ppu_noinput=0;
}
#endif
static void rt_sync(uint64_t dots){
    Ppu*p=&nes.ppu;
    while(p->dots<dots){
        uint8_t before=p->nmi_level;
        if(rt_fast_ppu) ppu_fp_run(&nes,dots); else ppu_step(&nes);
#ifndef AMIGA
        if(rt_ppu_check){ chk_advance(p->dots); chk_cmp("sync",p->frame_done); nes_ref.ppu.frame_done=0; }
#endif
#ifdef PREDICT_STRESS
        { extern uint64_t ppu_predict_hit(Nes*); static uint64_t last; uint64_t h=ppu_predict_hit(&nes); if(h) last=h; }
#endif
        if(p->nmi_level!=before) q_push(sample_cycle_for_dots(p->dots), 2*p->dots, p->nmi_level);
        if(p->frame_done && p->ar_on){      /* the scene log of the finished frame becomes the planar picture */
            p->ar_on=0; ar_end_frame(&nes,&ar_out);
#ifndef AMIGA
            if(rt_ppu_check){ static uint8_t fbx[240*256]; if(ar_hw_enable) ar_expand_hw(&ar_out,fbx); else ar_expand(&ar_out,fbx);
                { int cov[240]; for(int y=0;y<240;y++) cov[y]=0;     /* display state: every line is covered by exactly one region and carries the palette / mask of the reference at the start of the line */
                  for(int i=0;i<ar_out.nreg;i++){ const ArRegion*r=&ar_out.reg[i]; for(int y=r->y0;y<=r->y1;y++){ cov[y]++;
                      if(memcmp(r->pal,nes_ref.ppu.linepal[y],32) || r->gray!=(nes_ref.ppu.linemask[y]&1) || r->emph!=((nes_ref.ppu.linemask[y]>>5)&7)){ fprintf(stderr,"ARENDER FAIL: frame %u line %d: region %d palette/mask differs from the reference\n",p->frame,y,i); exit(5); } } }
                  for(int y=0;y<240;y++) if(cov[y]!=1){ fprintf(stderr,"ARENDER FAIL: frame %u line %d covered by %d regions (nreg %d)\n",p->frame,y,cov[y],ar_out.nreg); exit(5); } } extern long ar_chk_frames, ar_chk_exact; ar_chk_frames++; for(int y=0;y<240;y++) ar_chk_exact+=ar_scene.exact[y]; if(getenv("ARDBG2") && p->frame%1000==500){ fprintf(stderr,"F%u:",p->frame); for(int i=0;i<ar_out.nreg;i++){ const ArRegion*r=&ar_out.reg[i]; fprintf(stderr," [%d-%d k%d]",r->y0,r->y1,r->kind);} fprintf(stderr,"\n"); } if(getenv("ARDBG")){ int ne=0; for(int y=0;y<240;y++) ne+=ar_scene.exact[y]; fprintf(stderr,"frame %u regions %d exact %d\n",p->frame,ar_scene.nreg,ne); }
                for(int i=0;i<240*256;i++) if(fbx[i]!=(uint8_t)nes_ref.ppu.fb[i]){ fprintf(stderr,"ARENDER FAIL: frame %u row %d x %d: planar %02x ref %02x (regions %d, exact rows %d, overflow %d)\n",p->frame,i>>8,i&255,fbx[i],(unsigned)nes_ref.ppu.fb[i],ar_scene.nreg,(int)ar_chk_exact,ar_scene.overflow); exit(5); } if(ar_hw_enable) ar_hw_shown(); }
#endif
        }
        if(p->frame_done){ p->frame_done=0; rt.frames_done++; if(rt.frame_hook) rt.frame_hook(rt.frames_done-1); }
    }
}
void ppu_sync_rt(uint64_t dots){ rt_sync(dots); }
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
    if(p->line==-1){ dv--; df--; }     /* an odd pre-render line can be one dot shorter (conservative) */
    if(dv<=0) dv+=FRAME_DOTS-1;      /* next frame; -1: an odd frame may be one dot shorter */
    if(df<=0) df+=FRAME_DOTS-1;
    uint64_t ev=sample_cycle_for_dots(p->dots+dv), ef=sample_cycle_for_dots(p->dots+df);
    if(ev<E) E=ev; if(ef<E) E=ef;
    return E;
}


static uint8_t ppu_rd(uint16_t a, uint64_t c){
    rt_sync(3*c+4); uint8_t before=nes.ppu.nmi_level; uint8_t v=ppu_reg_read(&nes,a);
#ifndef AMIGA
    if(rt_ppu_check){ uint8_t v2=ppu_reg_read(&nes_ref,a); if(v!=v2) chk_fail("read",a==0x2002?"$2002":"$200x value",v,v2); chk_cmp("read",0); }
#endif
    if(nes.ppu.nmi_level!=before) q_push(c, 2*(3*c+4)+1, nes.ppu.nmi_level);
    return v;
}
static void ppu_wr(uint16_t a, uint8_t v, uint64_t c){
    rt_sync(3*c+4); uint8_t before=nes.ppu.nmi_level; ppu_reg_write(&nes,a,v);
#ifndef AMIGA
    if(rt_ppu_check){ ppu_reg_write(&nes_ref,a,v); chk_cmp("write",0); }
#endif
    if(nes.ppu.nmi_level!=before) q_push(c, 2*(3*c+4)+1, nes.ppu.nmi_level);
}

static uint8_t mem_read(uint16_t a, uint64_t c){
    uint8_t v;
    if(a<0x2000) v=nes.ram[a&0x7FF];
    else if(a<0x4000) v=ppu_rd(a,c);
    else if(a==0x4016||a==0x4017){ int i=a&1; if(nes.strobe) nes.joy_shift[i]=nes.joy[i]; uint8_t bit=nes.joy_shift[i]&1; nes.joy_shift[i]=(nes.joy_shift[i]>>1)|0x80; v=(rt.openbus&0xE0)|bit; }
    else if(a==0x4015) v=apu_read_status(&rt_apu,c)|(rt.openbus&0x20);   /* APU status (runtime/apu.c); bit 5 = open bus */
    else if(a>=0x8000) v= a<0xC000 ? nes.map.prg[nes.map.prg_lo+(a-0x8000)] : nes.map.prg[nes.map.prg_hi+(a-0xC000)];
    else v=rt.openbus;
    return v;
}
static void mem_write(uint16_t a, uint8_t v, uint64_t c){
    if(a<0x2000) nes.ram[a&0x7FF]=v;
    else if(a<0x4000) ppu_wr(a,v,c);
    else if(a==0x4014){ rt.need_halt=1; rt.dma_on=1; rt.dma_page=v; rt.ldirty=1; }
    else if(a==0x4016){ int s=v&1; if(nes.strobe && !s){ nes.joy_shift[0]=nes.joy[0]; nes.joy_shift[1]=nes.joy[1]; } nes.strobe=s; }
    else if(a<0x4018) apu_write(&rt_apu,a,v,c);   /* $4000-$4013, $4015, $4017 (4014/4016 handled above) */
    else if(a>=0x8000){
        if(mmc1_write_ppu_effect(&nes.map,a,v)){ rt_sync(3*c+4); nes.ppu.ar_wr=1; }      /* the write changes what the PPU fetches: the PPU must be at this dot first */
        mmc1_write(&nes.map,a,v);
#ifndef AMIGA
        if(rt_ppu_check) nes_ref.map=nes.map;      /* the reference instance sees the result (the assembler fast path only moves the serial register of the real one) */
#endif
    }
}
uint8_t rt_io_rd(uint16_t a, uint64_t c){ uint8_t v=mem_read(a,c); rt.openbus=v; return v; }
void    rt_io_wr(uint16_t a, uint8_t v, uint64_t c){ mem_write(a,v,c); rt.openbus=v; }

static void process_dma(uint16_t read_addr){
    rt.cyc++; if(read_addr!=0x4016&&read_addr!=0x4017) (void)mem_read(read_addr,rt.cyc); rt.need_halt=0;
    int counter=0; uint8_t rv=0, ra=0; int fast=-1;     /* fast: -1 undecided, 0 per-write path, 1 block path (see below) */
    while(rt.dma_on){
        int get=((rt.cyc&1)==0);
        rt.cyc++;
        if(get){ rv=mem_read((uint16_t)(rt.dma_page*0x100+ra),rt.cyc); ra++; counter++; }
        else if(counter&1){
            if(rt.trace_hook) rt.trace_hook('W',0x2004,rv,-1);
            ppu_wr(0x2004,rv,rt.cyc); counter++; if(counter==0x200) rt.dma_on=0;
            if(fast<0){   /* after the first synchronised write: block mode if every remaining write stores into OAM and nothing else can observe the PPU in between:
                           * source in RAM (no read side effects), no trace, and rendering off, or the whole rest of the transfer inside vertical blank */
                Ppu*p=&nes.ppu; int rem=(0x200-counter)*3+6;
                fast = (!rt.trace_hook && !RT_CHK && rt.dma_page<0x20 && !p->need_update && !p->upd_delay &&
                        (!p->rend || (p->line>=240 && (p->line-240)*341+p->cx+rem < 21*341))) ? 1 : 0;
                if(fast && rt.dma_on){   /* the remaining pairs of get/put cycles in one go: bytes ra..255 of the RAM page into OAM at the OAM address, 2 cycles each */
                    const uint8_t*src=nes.ram+((rt.dma_page<<8)&0x7FF); unsigned oa=p->oamaddr; uint8_t*oam=p->oam; unsigned i=ra;
                    if(p->ar_on && p->line>=0 && p->line<240){ ar_flush(&nes,1); p->ar_wr=1; }
                    uint8_t diff=0;
                    for(;i<256 && (oa&3);i++){ uint8_t b=src[i]; if((oa&3)==2) b&=0xE3; diff|=(uint8_t)(oam[oa]^b); oam[oa]=b; oa=(oa+1)&255; }
                    for(;i+4<=256;i+=4,oa=(oa+4)&255){ uint8_t b0=src[i],b1=src[i+1],b2=src[i+2]&0xE3,b3=src[i+3]; diff|=(uint8_t)((oam[oa]^b0)|(oam[oa+1]^b1)|(oam[oa+2]^b2)|(oam[oa+3]^b3)); oam[oa]=b0; oam[oa+1]=b1; oam[oa+2]=b2; oam[oa+3]=b3; }
                    for(;i<256;i++){ uint8_t b=src[i]; if((oa&3)==2) b&=0xE3; diff|=(uint8_t)(oam[oa]^b); oam[oa]=b; oa=(oa+1)&255; }
                    p->oamaddr=(uint8_t)oa; if(diff) p->oam_dirty=1; p->openbus=src[255]; rt.cyc+=2u*(256-ra); rt.dma_on=0; rt_sync(3*rt.cyc+4);
                }
            }
        }
        else { if(read_addr!=0x4016&&read_addr!=0x4017) (void)mem_read(read_addr,rt.cyc); }
    }
}
uint8_t bus_rd(uint16_t a){
    if(rt.need_halt) process_dma(a);
    rt.cyc++;
    uint8_t v=mem_read(a,rt.cyc);
    if(rt.trace_hook){
        if(rt.op_fetch){ int32_t ab=-1; if(a>=0x8000) ab= a<0xC000 ? (int32_t)(nes.map.prg_lo+(a-0x8000)) : (int32_t)(nes.map.prg_hi+(a-0xC000)); rt.trace_hook('X',a,v,ab); }
        else if(a>=0x2000 && a<0x8000) rt.trace_hook('R',a,v,-1);
    }
    rt.op_fetch=0; rt.openbus=v; return v;
}
void bus_wr(uint16_t a, uint8_t v){
    rt.cyc++;
    if(a>=0x2000 && rt.trace_hook) rt.trace_hook('W',a,v,-1);
    mem_write(a,v,rt.cyc); rt.openbus=v;
}

void rt_init(const uint8_t *prg, const uint8_t *chr){
    memset(&rt,0,sizeof rt); rt.need_cycle=RT_NEVER; rt.ram=nes.ram;
    memset(&nes,0,sizeof nes); nes.map.prg=prg; nes.map.chr=chr;
    ppu_reset(&nes.ppu); mmc1_reset(&nes.map); cpu_reset(&rt.cpu);
    apu_reset(&rt_apu); apu_set_audio(&rt_apu,RT_AUDIO);
#ifndef AMIGA
    if(rt_ppu_check){ memset(&nes_ref,0,sizeof nes_ref); nes_ref.map=nes.map; ppu_reset(&nes_ref.ppu); }
#endif
    ar_init(); nes.ppu.fp=rt_fast_ppu; nes.ppu.oa_line=-2;
    rt_sync(26); rt.cyc=7;
    rt.cpu.pc=(uint16_t)(prg[7*16384+0x3FFC]|prg[7*16384+0x3FFD]<<8);
}

int rt_lazy;      /* host test option (RT_LAZY=1): the interpreter synchronises the PPU only when an access or an event needs it, like the translated program */
uint64_t lazy_limit;     /* cycle limit maintained by rt_step in lazy mode (also read by the slow path of the translated program) */
int rt_step(void){
    rt.op_fetch=1; int r=cpu_step(&rt.cpu); rt.op_fetch=0;
    if(r<0) return r;
    uint64_t L=rt.cyc;
    if(rt_lazy){
        if(rt.ldirty){ lazy_limit=rt_compute_limit_at(L); rt.ldirty=0; }
        if(L<=lazy_limit && !rt.need_halt) return 0;
        lazy_limit=rt_compute_limit_at(L); rt.ldirty=0;
    }
    rt_sync(3*(L-1)+5); nmi_advance(L-1);
    if(rt.need_cycle!=RT_NEVER && rt.need_cycle<=L-1){ rt.need_cycle=RT_NEVER; cpu_nmi(&rt.cpu); if(rt_lazy){ lazy_limit=rt_compute_limit_at(rt.cyc); rt.ldirty=0; } }
    return 0;
}
void rt_run_interp(long frames){ while(rt.frames_done<frames){ if(rt_step()<0) return; } }

/* NMI check at the end of the last completed instruction (used when the translated code hands over to the slow path: the instruction that
 * just finished in translated code may have been the one after which the NMI has to be taken) */
void rt_nmi_check(void){
    uint64_t L=rt.cyc; rt_sync(3*(L-1)+5); nmi_advance(L-1);
    if(rt.need_cycle!=RT_NEVER && rt.need_cycle<=L-1){ rt.need_cycle=RT_NEVER; cpu_nmi(&rt.cpu); }
}
