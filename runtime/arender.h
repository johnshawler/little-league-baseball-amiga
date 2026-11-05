/* arender.h - Amiga-style rendering of the NES picture: scene log in, planar picture out.  Target independent C (host + 68020).
 *
 * The PPU model (ppu_ref.c) runs state-only (timing, flags, v/t/x, sprite-0 hit, overflow, NMI).  On frames that are displayed it records a SCENE LOG:
 * a few REGIONS per frame (runs of whole lines that share scroll progression and rendering parameters) and EXACT lines (lines the model could not describe
 * as a plain region: an access landed inside them or inside the line before; their pixels are produced by the exact line renderer as chunky rows and are
 * handed over as such).  ar_render() turns the log + VRAM + OAM snapshot + CHR into a 5-plane picture:
 *   planes 0..3 = background colour index (pattern bit 0, pattern bit 1, palette bit 0, palette bit 1) or sprite colour (same layout, sprite palette),
 *   plane 4     = 1 where the pixel is a sprite pixel (palette RAM index 16..31).
 * i.e. the value (plane4<<4|plane3<<3|plane2<<2|plane1<<1|plane0) is the PALETTE RAM INDEX 0..31 of the pixel (index 0 for transparent background / backdrop),
 * exactly the value of the PPU_FB5 chunky framebuffer.  The display loads the 32 colour registers from the region's palette state.
 *
 * Interface contract (what the platform layer needs):
 *   - ar_begin_frame(): called when a displayed frame starts (line -1); clears the log, snapshots OAM.
 *   - the PPU model calls ar_log_region / ar_log_exact / ar_log_palette while the frame runs.
 *   - ar_end_frame(&out): called at the frame hook; renders the log into out->plane[0..4] (rows of AR_STRIDE bytes, 240 rows) and fills out->reg[] with the
 *     display state per region (first/last line, palette[32], mask).  Rows of exact lines are converted from their chunky bytes.
 *   - ar_expand(&out, fb): host verification: planar -> chunky palette indices (240*256 bytes), compared with the reference frame buffer.
 * Stage 2 (platform): out->reg[] also carries the pointers/offsets the Copper list needs (bitmap row address, bit offset = BPLCON1 + byte offset). */
#ifndef ARENDER_H
#define ARENDER_H
#include <stdint.h>
#include "nes.h"

#define AR_STRIDE      32          /* bytes per plane row (256 pixels) */
#define AR_MAX_REGIONS 24
#define AR_MAX_PAL     16
#define AR_MAX_CFG     8           /* persistent background bitmaps (one per CHR bank pair / pattern table / mirroring configuration) */

/* a run of whole lines [y0,y1] drawn from the same configuration; line y0 starts with v = vl (coarse X/Y, nametable, fine Y: bits as the PPU's v register),
 * following lines follow the PPU progression (increment Y, copy the horizontal bits of tx) */
typedef struct {
    int16_t  y0, y1;
    uint16_t vl;                   /* v at the output start of line y0 (tile 0 of the line is fetched with vl) */
    uint16_t tx;                   /* t & 0x041F at the time: horizontal bits copied at dot 257 of every line */
    uint8_t  fx;                   /* fine X */
    uint8_t  rend;                 /* rendering enabled (0: forced blank, backdrop only; backdrop index in blank_idx) */
    uint8_t  blank_idx;            /* palette index shown when !rend (0 or the 3Fxx address trick) */
    uint8_t  bg_en, sp_en;
    uint16_t min_bg, min_sp;       /* leftmost visible x is >= min_bg / min_sp (0: all, 8: left 8 pixels clipped, 300: disabled) */
    uint8_t  bgpat, sppat, large;  /* BG pattern table select (0/1), sprite pattern table (0x1000 or 0), 8x16 sprites */
    uint8_t  mirror;               /* nametable mirroring in force */
    uint8_t  chr_lo, chr_hi;       /* 4 KB CHR bank numbers at PPU $0000 / $1000 */
    uint8_t  gray, emph;           /* mask bits */
    uint8_t  pal[32];              /* palette RAM at the start of the region (palette writes inside a region split it) */
} ArRegion;


typedef struct {
    ArRegion reg[AR_MAX_REGIONS]; int nreg;
    uint8_t  exact[240];           /* 1: row comes from the exact chunky renderer */
    uint8_t  oam[256];             /* OAM as it was when the frame started */
    int      overflow;             /* log capacity exceeded or OAM changed during the frame: the whole frame is exact (chunky rows) */
} ArScene;

typedef struct {
    uint8_t *plane[5];             /* 5 planes, 240 rows of AR_STRIDE bytes each (display buffer) */
    int      nreg; ArRegion reg[AR_MAX_REGIONS];   /* display state per region */
} ArFrame;

void ar_init(void);                               /* tables: CHR planar conversion etc. (needs nes.map.chr) */
void ar_begin_frame(Nes *n);
void ar_log_region(Nes *n, const ArRegion *r);    /* appends; merges with the previous region when the parameters match and the progression continues */
void ar_log_exact(Nes *n, int y);
void ar_flush(Nes *n, int oam_changes);          /* render the log so far (before a mid-frame name table / OAM change); oam_changes: take a new OAM snapshot afterwards */
void ar_end_frame(Nes *n, ArFrame *out);
void ar_expand(const ArFrame *f, uint8_t *fb);    /* host: planar -> palette index per pixel (240*256 bytes), compared with the reference frame buffer */
extern ArScene ar_scene; extern ArFrame ar_out; extern int ppu_ar_enable;   /* ar_out: result of the last frame rendered while ppu_ar_enable != 0 */
#endif
