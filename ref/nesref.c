/* nesref.c - reference whole-board emulator (NTSC NES, SLROM/MMC1): cycle-exact 2A03 core + per-dot PPU + mapper + controllers.
 * Host only.  Writes an instruction/IO trace in the same 16-byte record format as the instrumented reference emulator so the two can be diffed.
 * usage: nesref game.nes frames script.txt [trace.out] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/cpu6502.h"
#include "../runtime/nes.h"

#pragma pack(push,1)
typedef struct { char t; uint8_t v; uint16_t a; int32_t abs; uint32_t cyc; uint32_t fr; } TRec;
#pragma pack(pop)
static FILE *trc;
static Cpu6502 cpu;
static int need_nmi, prev_need_nmi, prev_nmi_flag, need_halt, dma_on, dma_page;
static uint8_t openbus; static int op_fetch;
static long frames_done; static FILE *fbf;
static unsigned script_mask(void);
static uint8_t *rom_prg, *rom_chr;

static void trace(char t, uint16_t a, uint8_t v, int32_t abs_){
    if(!trc) return; TRec r={t,v,a,abs_,(uint32_t)nes.cyc,nes.ppu.frame}; fwrite(&r,sizeof r,1,trc);
}
/* Reference emulator timing: StartCpuCycle runs the PPU to the middle of the cycle (2 dots), the bus access happens, EndCpuCycle runs 1 more dot. */
static void start_cycle(void){ nes.cyc++; ppu_step(&nes); ppu_step(&nes); }
static void end_cycle(void){
    ppu_step(&nes);
    prev_need_nmi=need_nmi;
    if(!prev_nmi_flag && nes.ppu.nmi_level) need_nmi=1;
    prev_nmi_flag=nes.ppu.nmi_level;
    if(nes.ppu.frame_done){ nes.ppu.frame_done=0; if(fbf){ fwrite(&frames_done,sizeof frames_done,1,fbf); fwrite(nes.ppu.fb,2,240*256,fbf);} frames_done++; }
}
void nes_input_poll(void){
    unsigned m=script_mask(); uint8_t b=0;           /* libretro ids: B0 Sel2 St3 U4 D5 L6 R7 A8 ; report order A B Sel Start U D L R */
    if(m&0x100) b|=1; if(m&1) b|=2; if(m&4) b|=4; if(m&8) b|=8; if(m&0x10) b|=16; if(m&0x20) b|=32; if(m&0x40) b|=64; if(m&0x80) b|=128;
    nes.joy[0]=b; nes.joy[1]=0;
}
static void joy_refresh(int i){ nes.joy_shift[i]=nes.joy[i]; }

static uint8_t mem_read(uint16_t a){
    uint8_t v;
    if(a<0x2000) v=nes.ram[a&0x7FF];
    else if(a<0x4000) v=ppu_reg_read(&nes,a);
    else if(a==0x4016||a==0x4017){ int i=a&1; if(nes.strobe) joy_refresh(i); uint8_t bit=nes.joy_shift[i]&1; nes.joy_shift[i]=(nes.joy_shift[i]>>1)|0x80; v=(openbus&0xE0)|bit; }
    else if(a==0x4015) v=0;   /* APU status: not modelled yet */
    else if(a>=0x8000) v= a<0xC000 ? nes.map.prg[nes.map.prg_lo+(a-0x8000)] : nes.map.prg[nes.map.prg_hi+(a-0xC000)];
    else v=openbus;
    return v;
}
static void mem_write(uint16_t a, uint8_t v){
    if(a<0x2000) nes.ram[a&0x7FF]=v;
    else if(a<0x4000) ppu_reg_write(&nes,a,v);
    else if(a==0x4014){ need_halt=1; dma_on=1; dma_page=v; }
    else if(a==0x4016){ int s=v&1; if(nes.strobe && !s){ joy_refresh(0); joy_refresh(1); } nes.strobe=s; }
    else if(a>=0x8000) mmc1_write(&nes.map,a,v);
}
static void process_dma(uint16_t read_addr){
    uint16_t prev=read_addr; (void)prev;
    start_cycle(); if(read_addr!=0x4016&&read_addr!=0x4017) { (void)mem_read(read_addr); } end_cycle(); need_halt=0;   /* halt cycle */
    int counter=0; uint8_t rv=0, ra=0;
    while(dma_on){
        int get=((nes.cyc&1)==0);
        start_cycle();
        if(get){ rv=mem_read((uint16_t)(dma_page*0x100+ra)); end_cycle(); ra++; counter++; }
        else if(counter&1){ trace('W',0x2004,rv,-1); ppu_reg_write(&nes,0x2004,rv); end_cycle(); counter++; if(counter==0x200) dma_on=0; }
        else { if(read_addr!=0x4016&&read_addr!=0x4017) (void)mem_read(read_addr); end_cycle(); }
    }
}
uint8_t bus_rd(uint16_t a){
    if(need_halt) process_dma(a);
    start_cycle();
    uint8_t v=mem_read(a);
    if(op_fetch){ op_fetch=0; int32_t ab=-1; if(a>=0x8000) ab= a<0xC000 ? (int32_t)(nes.map.prg_lo+(a-0x8000)) : (int32_t)(nes.map.prg_hi+(a-0xC000)); trace('X',a,v,ab); }
    else if(a>=0x2000 && a<0x8000) trace('R',a,v,-1);
    openbus=v; end_cycle(); return v;
}
void bus_wr(uint16_t a, uint8_t v){
    start_cycle();
    if(a>=0x2000) trace('W',a,v,-1);
    mem_write(a,v); openbus=v; end_cycle();
}

/* input script */
static long sf[8192]; static unsigned sm[8192]; static int ns;
static unsigned script_mask(void){ unsigned m=0; for(int i=0;i<ns;i++){ if(sf[i]<=frames_done) m=sm[i]; else break; } return m; }

int main(int argc,char**argv){
    if(argc<4){ fprintf(stderr,"usage\n"); return 2; }
    FILE*f=fopen(argv[1],"rb"); if(!f){perror("rom");return 1;} fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    uint8_t*d=malloc(sz); if(fread(d,1,sz,f)!=(size_t)sz) return 1; fclose(f);
    rom_prg=d+16; rom_chr=d+16+131072; nes.map.prg=rom_prg; nes.map.chr=rom_chr;
    long nf=atol(argv[2]); FILE*sc=fopen(argv[3],"r"); while(sc&&fscanf(sc,"%ld %x",&sf[ns],&sm[ns])==2) ns++;
    if(argc>4 && strcmp(argv[4],"-")) trc=fopen(argv[4],"wb"); if(getenv("FBDUMP")) fbf=fopen(getenv("FBDUMP"),"wb");
    memset(nes.ram,0,sizeof nes.ram); ppu_reset(&nes.ppu); mmc1_reset(&nes.map);
    cpu_reset(&cpu);
    int idots=getenv("IDOTS")?atoi(getenv("IDOTS")):2; for(int i=0;i<idots;i++) ppu_step(&nes);
    for(int i=0;i<8;i++){ nes.cyc=(uint64_t)i; ppu_step(&nes); ppu_step(&nes); end_cycle(); } nes.cyc=7;
    cpu.pc=(uint16_t)(rom_prg[7*16384+0x3FFC]|rom_prg[7*16384+0x3FFD]<<8);
    int polled=0; long last_poll_frame=-1;
    while(frames_done<nf){
        /* input poll at line 241 cycle 0 */
        op_fetch=1; int r=cpu_step(&cpu); op_fetch=0;
        if(r<0){ fprintf(stderr,"illegal opcode at %04x cyc %llu\n",cpu.pc,(unsigned long long)nes.cyc); return 3; }
        if(prev_need_nmi){ need_nmi=0; cpu_nmi(&cpu); }
    }
    if(trc) fclose(trc);
    fprintf(stderr,"done: %ld frames, %llu cpu cycles\n",frames_done,(unsigned long long)nes.cyc);
    return 0;
}
