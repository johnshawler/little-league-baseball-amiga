/* rt_core.c - runtime time model.  The PPU is synchronised lazily: it is run only up to the dot count that is visible to a bus access, and the
 * NMI edge logic is evaluated from queued level transitions, which reproduces the reference emulator's per-cycle sampling exactly:
 *   access in CPU cycle c sees PPU dots executed = 3c+4 (StartCpuCycle runs 2 dots), the end of cycle c has run 3c+5 dots;
 *   the NMI line is sampled at the end of every cycle, an edge sets needNmi, the CPU takes the NMI after an instruction whose
 *   last cycle is L iff needNmi was set at the end of cycle <= L-1. */
#include "rt.h"
Rt rt;

static inline uint64_t sample_cycle_for_dots(uint64_t d){ return d<=5 ? 0 : (d-5+2)/3; }   /* smallest k with 3k+5 >= d */

static void q_push(uint64_t sample, uint64_t key, uint8_t lvl){
    int i=rt.nq; if(i==8){ i=7; }
    while(i>0 && rt.q[i-1].key>key){ rt.q[i]=rt.q[i-1]; i--; }
    rt.q[i].sample=sample; rt.q[i].key=key; rt.q[i].lvl=lvl; if(rt.nq<8) rt.nq++;
}
/* process queued level transitions whose sample cycle <= upto */
static void nmi_advance(uint64_t upto){
    while(rt.nq && rt.q[0].sample<=upto){
        uint64_t k=rt.q[0].sample; uint8_t lv=rt.q[0].lvl; int n=1;
        while(n<rt.nq && rt.q[n].sample==k){ lv=rt.q[n].lvl; n++; }          /* only the final level at the sample point counts */
        if(lv && !rt.lvl_sampled && rt.need_cycle==RT_NEVER) rt.need_cycle=k;
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

static void rt_sync(uint64_t dots){
    Ppu*p=&nes.ppu;
    while(p->dots<dots){
        uint8_t before=p->nmi_level; ppu_step(&nes);
        if(p->nmi_level!=before) q_push(sample_cycle_for_dots(p->dots), 2*p->dots, p->nmi_level);
        if(p->frame_done){ p->frame_done=0; rt.frames_done++; if(rt.frame_hook) rt.frame_hook(rt.frames_done-1); }
    }
}
void ppu_sync_rt(uint64_t dots){ rt_sync(dots); }

static uint8_t ppu_rd(uint16_t a, uint64_t c){
    rt_sync(3*c+4); uint8_t before=nes.ppu.nmi_level; uint8_t v=ppu_reg_read(&nes,a);
    if(nes.ppu.nmi_level!=before) q_push(c, 2*(3*c+4)+1, nes.ppu.nmi_level);
    return v;
}
static void ppu_wr(uint16_t a, uint8_t v, uint64_t c){
    rt_sync(3*c+4); uint8_t before=nes.ppu.nmi_level; ppu_reg_write(&nes,a,v);
    if(nes.ppu.nmi_level!=before) q_push(c, 2*(3*c+4)+1, nes.ppu.nmi_level);
}

static uint8_t mem_read(uint16_t a, uint64_t c){
    uint8_t v;
    if(a<0x2000) v=nes.ram[a&0x7FF];
    else if(a<0x4000) v=ppu_rd(a,c);
    else if(a==0x4016||a==0x4017){ int i=a&1; if(nes.strobe) nes.joy_shift[i]=nes.joy[i]; uint8_t bit=nes.joy_shift[i]&1; nes.joy_shift[i]=(nes.joy_shift[i]>>1)|0x80; v=(rt.openbus&0xE0)|bit; }
    else if(a==0x4015) v=0;                       /* APU status: see apu.c once integrated */
    else if(a>=0x8000) v= a<0xC000 ? nes.map.prg[nes.map.prg_lo+(a-0x8000)] : nes.map.prg[nes.map.prg_hi+(a-0xC000)];
    else v=rt.openbus;
    return v;
}
static void mem_write(uint16_t a, uint8_t v, uint64_t c){
    if(a<0x2000) nes.ram[a&0x7FF]=v;
    else if(a<0x4000) ppu_wr(a,v,c);
    else if(a==0x4014){ rt.need_halt=1; rt.dma_on=1; rt.dma_page=v; }
    else if(a==0x4016){ int s=v&1; if(nes.strobe && !s){ nes.joy_shift[0]=nes.joy[0]; nes.joy_shift[1]=nes.joy[1]; } nes.strobe=s; }
    else if(a>=0x8000) mmc1_write(&nes.map,a,v);
}
uint8_t rt_io_rd(uint16_t a, uint64_t c){ uint8_t v=mem_read(a,c); rt.openbus=v; return v; }
void    rt_io_wr(uint16_t a, uint8_t v, uint64_t c){ mem_write(a,v,c); rt.openbus=v; }

static void process_dma(uint16_t read_addr){
    rt.cyc++; if(read_addr!=0x4016&&read_addr!=0x4017) (void)mem_read(read_addr,rt.cyc); rt.need_halt=0;
    int counter=0; uint8_t rv=0, ra=0;
    while(rt.dma_on){
        int get=((rt.cyc&1)==0);
        rt.cyc++;
        if(get){ rv=mem_read((uint16_t)(rt.dma_page*0x100+ra),rt.cyc); ra++; counter++; }
        else if(counter&1){ if(rt.trace_hook) rt.trace_hook('W',0x2004,rv,-1); ppu_wr(0x2004,rv,rt.cyc); counter++; if(counter==0x200) rt.dma_on=0; }
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
    rt_sync(26); rt.cyc=7;
    rt.cpu.pc=(uint16_t)(prg[7*16384+0x3FFC]|prg[7*16384+0x3FFD]<<8);
}

int rt_step(void){
    rt.op_fetch=1; int r=cpu_step(&rt.cpu); rt.op_fetch=0;
    if(r<0) return r;
    uint64_t L=rt.cyc; rt_sync(3*(L-1)+5); nmi_advance(L-1);
    if(rt.need_cycle!=RT_NEVER && rt.need_cycle<=L-1){ rt.need_cycle=RT_NEVER; cpu_nmi(&rt.cpu); }
    return 0;
}
void rt_run_interp(long frames){ while(rt.frames_done<frames){ if(rt_step()<0) return; } }
