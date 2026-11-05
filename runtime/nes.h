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
int  mmc1_write_ppu_effect(const Mapper *m, uint16_t addr, uint8_t v);   /* 1 if the write changes CHR banks / mirroring (PPU must be synchronised first) */

/* ---------------------------------------------------------------- PPU */
#if defined(PPU_FB5)
#define FBVAL(c,e) ((uint8_t)(c))      /* Amiga 5-plane build: the framebuffer holds the palette RAM index (0..31); colours/emphasis/gray are applied to the 32 colour registers by the platform */
#elif defined(PPU_FB8)
#define FBVAL(c,e) ((uint8_t)((c)|(((e)&3)<<6)))
#else
#define FBVAL(c,e) ((uint16_t)((c)|((e)<<6)))
#endif
typedef struct { uint16_t addr; uint8_t paloff, lo, hi, offy; } Tile;
typedef struct { uint8_t lo, hi, x, paloff, bgprio, hflip, vflip; } SpriteTile;
/* fast PPU model (ppu_ref.c): fetch context of one output line (tile and sprite data fetched at their dots with the parameters in force then) */
typedef struct {
    int16_t  y;                       /* output line this context belongs to, -1: none */
    uint8_t  jlo, jhi;                /* tiles j (0..32) that are tracked */
    uint8_t  t01ok;                   /* bit j: tile j (0,1) was fetched by the previous line (generic path); 0: the whole-line path fetches them when needed */
    uint8_t  mode, nspr, s0;          /* mode 1: sprite 0 only (hit test), 2: complete line; sprites on the line (0..8); slot 0 is sprite 0 */
    uint8_t  tlo[34], thi[34], tpal[34]; uint16_t tta[34];
    uint8_t  spx[8], slo[8], shi[8], spal[8], sprio[8], shf[8];
} FpCtx;
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
#if defined(PPU_FB8) || defined(PPU_FB5)
    uint8_t  fb[240*256];             /* Amiga build: NES colour (6 bit) | (red,green emphasis)<<6; blue emphasis is not displayed */
#else
    uint16_t fb[240*256];             /* colour per pixel: NES colour (6 bit) | emphasis<<6 (| gray applied) */
#endif
    uint8_t  rowok[240]; uint32_t rowsig[240];   /* signature of the inputs of the row now in the framebuffer (renderer: rows with unchanged inputs are not drawn again) */
    FpCtx    fc[2]; int fcur;                     /* fast model: fetch contexts of the line being output (fc[fcur]) and of the next line */
    uint32_t ntver_all;               /* fast model: write counter of the whole name table memory */
    uint32_t ntv[64];                 /* fast model: write counters of the 32-byte rows of the two physical name tables (a row of the background cache is valid while its source rows are unchanged) */
    uint8_t  lt_cnt[240];             /* fast model: sprites in range per evaluation line (saturated at 255) */
    uint8_t  lt_idx[240][8];          /* first 8 sprites (OAM index) in range per evaluation line (valid when lt_full) */
    int16_t  lt_ocx[240];             /* dot (cx) at which the sprite overflow flag is set on this evaluation line: 0 none, -1 not computed yet */
    int      ar_on;                   /* the scene log of arender is being recorded for this frame */
    int      fp;                      /* this instance is driven by the fast model (the OAM address during sprite evaluation is then computed on demand) */
    int16_t  oa_line; uint8_t oa_start;
    int      lt_valid, lt_full, lt_large;  /* tables above built for the current OAM / sprite size (oam_dirty clears lt_valid) */
    uint8_t  rowblank[240];           /* fast model: row is uniformly the backdrop colour (value+1), 0 = not known */
    uint8_t  rowdirty[240];           /* rows written to fb since the platform last cleared the flag (display change tracking) */
    int      palvar;                  /* palette RAM / emphasis / gray changed during visible lines (set by the PPU, cleared by the platform at the frame hook) */
    int      oam_dirty;               /* OAM written since the sprite coverage table of the state-only renderer was built */
    int      draw;                    /* 1: write the framebuffer (the picture will be displayed); 0: compute only the state effects (platform sets it per frame) */
    int      frame_done;
    int      nmi_level;               /* NMI line level (Mesen: NMIFlag) */
} Ppu;

typedef struct {
    Mapper map;
    uint8_t joy[2]; uint8_t joy_latch[2]; uint8_t joy_shift[2]; uint8_t strobe, pad_;   /* joypads: at fixed offsets, the assembler fast paths of rt_x.s access them (see the static asserts in rt_x.c) */
    Ppu ppu;
    uint8_t ram[2048];
    uint64_t cyc;
} Nes;
extern Nes nes;

void ppu_reset(Ppu *p);
void ppu_step(Nes *n);                                  /* execute one dot */
uint8_t ppu_reg_read(Nes *n, uint16_t a);                /* $2000-$3FFF, called with the PPU run up to the end of the previous CPU cycle */
void    ppu_reg_write(Nes *n, uint16_t a, uint8_t v);
#endif
