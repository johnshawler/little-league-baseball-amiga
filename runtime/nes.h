/* nes.h - NES (NTSC, SLROM/MMC1) board model shared by the reference emulator and the translated runtime.  Target independent. */
#ifndef NES_H
#define NES_H
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- cartridge / mapper (MMC1, SLROM) */
typedef struct {
    const uint8_t *prg;        /* 8 x 16 KB */
    const uint8_t *chr;        /* 32 x 4 KB (CHR ROM) */
    uint8_t  shift, count;     /* serial shift register */
    uint8_t  reg[4];           /* control, chr0, chr1, prg */
    uint32_t prg_lo, prg_hi;   /* byte offsets of the banks mapped at $8000 / $C000 */
    uint32_t chr_lo, chr_hi;   /* byte offsets of the 4 KB banks at PPU $0000 / $1000 */
    uint8_t  mirror;           /* 0 one-screen low, 1 one-screen high, 2 vertical, 3 horizontal */
} Mapper;
void mmc1_reset(Mapper *m);
void mmc1_write(Mapper *m, uint16_t addr, uint8_t v);   /* $8000-$FFFF */

/* ---------------------------------------------------------------- PPU */
#ifdef PPU_FB8
#define FBVAL(c,e) ((uint8_t)((c)|(((e)&3)<<6)))
#else
#define FBVAL(c,e) ((uint16_t)((c)|((e)<<6)))
#endif
typedef struct { uint16_t addr; uint8_t paloff, lo, hi, offy; } Tile;
typedef struct { uint8_t lo, hi, x, paloff, bgprio, hflip, vflip; } SpriteTile;
typedef struct {
    /* registers */
    uint8_t  ctrl, mask, oamaddr;
    uint8_t  vbl, sp0hit, spovf;      /* status flags */
    uint16_t v, t; uint8_t fx, w;
    uint8_t  rdbuf, openbus;
    uint16_t bgpat, sppat; uint8_t large, vinc;   /* decoded ctrl */
    uint16_t min_bg, min_sp, min_sp_std;  /* minimum draw cycles (8 or 0, 300 = disabled) */
    uint8_t  bg_en, sp_en, gray, emph;
    /* position */
    int      line, cx;                /* line -1..260, cx 0..340 */
    uint32_t frame;                   /* starts at 1, incremented at line 240 */
    uint64_t dots;
    int      rend, rend_prev, need_update;
    int      prevent_vbl;
    int      upd_delay; uint16_t upd_addr;
    uint8_t  pal[32], nt[2048], oam[256], oam2[32];
    /* background pipeline */
    Tile     tprev, tcur, tnext; uint16_t sh_lo, sh_hi;
    /* sprites */
    SpriteTile st[8]; uint8_t has_sprite[264];
    int      sp_count, sp_index, s0_visible, s0_added, sp_inrange, copy_done, ovf_bug;
    uint8_t  oamcopy, sec_addr, addrH, addrL;
#ifdef PPU_FB8
    uint8_t  fb[240*256];             /* Amiga build: NES colour (6 bit) | (red,green emphasis)<<6; blue emphasis is not displayed */
#else
    uint16_t fb[240*256];             /* colour per pixel: NES colour (6 bit) | emphasis<<6 (| gray applied) */
#endif
    uint8_t  rowdirty[240];           /* rows written to fb since the platform last cleared the flag (display change tracking) */
    int      oam_dirty;               /* OAM written since the sprite coverage table of the state-only renderer was built */
    int      draw;                    /* 1: write the framebuffer (the picture will be displayed); 0: compute only the state effects (platform sets it per frame) */
    int      frame_done;
    int      nmi_level;               /* NMI line level (Mesen: NMIFlag) */
} Ppu;

typedef struct {
    Mapper map; Ppu ppu;
    uint8_t ram[2048];
    uint64_t cyc;
    uint8_t joy[2]; uint8_t joy_latch[2]; uint8_t joy_shift[2]; uint8_t strobe;
} Nes;
extern Nes nes;

void ppu_reset(Ppu *p);
void ppu_step(Nes *n);                                  /* execute one dot */
uint8_t ppu_reg_read(Nes *n, uint16_t a);                /* $2000-$3FFF, called with the PPU run up to the end of the previous CPU cycle */
void    ppu_reg_write(Nes *n, uint16_t a, uint8_t v);
#endif
