/* MMC1 (SLROM, 128 KB PRG, 128 KB CHR ROM): behaviour from MAME mmc1.cpp (sxrom) and checked against the reference trace. */
#include "nes.h"
static void update(Mapper *m){
    uint8_t ctl=m->reg[0], pm=(ctl>>2)&3;
    uint32_t b=m->reg[3]&7;
    switch(pm){
    case 0: case 1: m->prg_lo=(b&6)*16384u; m->prg_hi=((b&6)+1)*16384u; break;   /* 32 KB mode */
    case 2: m->prg_lo=0; m->prg_hi=b*16384u; break;                              /* fix first */
    default: m->prg_lo=b*16384u; m->prg_hi=7*16384u; break;                       /* fix last */
    }
    if(ctl&0x10){ m->chr_lo=(m->reg[1]&31)*4096u; m->chr_hi=(m->reg[2]&31)*4096u; }
    else { uint32_t b8=(m->reg[1]&0x1E); m->chr_lo=b8*4096u; m->chr_hi=(b8+1)*4096u; }
    m->mirror=ctl&3;
}
void mmc1_reset(Mapper *m){ m->shift=0; m->count=0; m->reg[0]=0x0F; m->reg[1]=m->reg[2]=m->reg[3]=0; update(m); }
void mmc1_write(Mapper *m, uint16_t addr, uint8_t v){
    if(v&0x80){ m->shift=0; m->count=0; m->reg[0]|=0x0C; update(m); return; }
    m->shift=(uint8_t)((m->shift>>1)|((v&1)<<4)); m->count++;
    if(m->count==5){ m->reg[(addr>>13)&3]=m->shift; m->shift=0; m->count=0; update(m); }
}
