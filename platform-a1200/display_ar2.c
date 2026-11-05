/* display_ar2.c - hardware side of arender stage 2b (docs/RENDER.md "Stage 2b", "Platform notes"): AGA dual playfield 4+4 planes, display pointers straight into the persistent
 * background bitmaps (playfield 1), the sprite layer as playfield 2, one Copper list per picture built from the regions.
 *
 * Mode (platform.c with -DARHW2): BPLCON0 0x0611 (8 planes, DBLPF), BPLCON2 0x0040 (playfield 2 in front), BPLCON3 PF2OF = 16, FMODE 0 (16-bit fetch), DDFSTRT 0x40 / DDFSTOP 0xC0:
 * 17 words per line = one extra word on the left that the horizontal scroll of playfield 1 hides.  Odd bitplanes (BPL1,3,5,7) = playfield 1 = background value
 * v = palette*4 + pattern -> colour register v (4, 8, 12 = backdrop); even bitplanes (BPL2,4,6,8) = playfield 2 = sprite value u -> register 16+u (u = 0 transparent).
 * Per region the Copper writes (in the horizontal blank before its first line, only what changed): bitplane pointers, BPL1MOD/BPL2MOD, BPLCON1 and the changed colour registers.
 *   background window: N = bytex*8 + fx; r = N & 15; r == 0: first word (N>>4)-1, scroll 0; else first word N>>4, scroll 16-r; pointer = row address + 2*word (the word before the window is hidden; N = 0 uses word 31, the guard bytes continue the window with words 0..), modulo = bitmap row stride - 34.  Playfield 2 (unscrolled): pointer = row - 2 (hidden pad word), modulo = 32 - 34.
 *   The window reads bytes 2*w0 .. 2*w0+33 of the row: with w0 up to 31 the row needs 32 guard bytes behind its 64 bytes (copy of bytes 0..31): stride >= 96. */
#include <stdint.h>
#include "platform.h"
#include "display_ar2.h"

extern const uint32_t nes_rgb[256];
#define BPLCON3_BASE2 0x1020
#define BPLCON3_LOCT2 0x0200
#define MOVE(reg, val) (*p++ = ((uint32_t)(reg) << 16) | (uint16_t)(val))
#define WAITL(vp, hp)  (*p++ = ((uint32_t)((((vp) & 0xFF) << 8) | ((hp) & 0xFE) | 1) << 16) | 0xFFFEu)

static uint8_t zero_row[40] __attribute__((aligned(4), section(".chipbuf")));

typedef struct { uint32_t ptr[8]; uint16_t mod1, mod2, con1; uint16_t hi[32], lo[32]; uint32_t valid; } St;

static void pal_words(const uint8_t pal[32], unsigned gray, unsigned emph, uint16_t hi[32], uint16_t lo[32])
{
    unsigned gm = gray ? 0x30 : 0x3F, em = (emph & 3) << 6;
    for (unsigned r = 0; r < 32; r++) {
        unsigned e = r < 16 ? ((r & 3) ? r : 0) : r;                     /* background colour v: entry v, 4/8/12 = backdrop; sprite colour 16+u: entry 16+u */
        uint32_t c = nes_rgb[(pal[e] & gm) | em], rr = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        hi[r] = (uint16_t)(((rr >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)); lo[r] = (uint16_t)(((rr & 15) << 8) | ((g & 15) << 4) | (b & 15));
    }
}

/* colour registers (bit r = register r) that a region with the palette-index mask `used` can show: registers 0..15 are background values (entry v, 4/8/12 = backdrop entry 0), 16..31 sprite entries */
static uint32_t regmask(uint32_t used)
{
    uint32_t m = 0;
    for (unsigned r = 0; r < 32; r++) if (used >> (r < 16 ? ((r & 3) ? r : 0) : r) & 1) m |= 1u << r;
    return m;
}

/* emit the colour registers in `mask` that differ from the state `st` (never written = differ); the state follows what was written */
static uint32_t *emit_pal(uint32_t *p, St *st, const uint16_t hi[32], const uint16_t lo[32], uint32_t mask)
{
    unsigned any = 0;
    for (unsigned i = 0; i < 32; i++) if ((mask >> i & 1) && (!(st->valid >> i & 1) || hi[i] != st->hi[i] || lo[i] != st->lo[i])) any |= 1;
    if (!any) return p;
    for (unsigned i = 0; i < 32; i++) if ((mask >> i & 1) && (!(st->valid >> i & 1) || hi[i] != st->hi[i])) MOVE(0x180 + 2 * i, hi[i]);
    MOVE(0x106, BPLCON3_BASE2 | BPLCON3_LOCT2);
    for (unsigned i = 0; i < 32; i++) if ((mask >> i & 1) && (!(st->valid >> i & 1) || lo[i] != st->lo[i])) MOVE(0x180 + 2 * i, lo[i]);
    MOVE(0x106, BPLCON3_BASE2);
    for (unsigned i = 0; i < 32; i++) if (mask >> i & 1) { st->hi[i] = hi[i]; st->lo[i] = lo[i]; }
    st->valid |= mask;
    return p;
}

/* after a region of h lines the display DMA stands where the next line of a continuing region starts: playfield 1 planes advance by adv1 per line, playfield 2 planes by adv2 */
static void advance(St *st, unsigned h, unsigned adv1, unsigned adv2)
{
    for (unsigned n = 0; n < 8; n++) st->ptr[n] += h * ((n & 1) ? adv2 : adv1);
}

/* bitplane pointers / modulo / scroll of a region; writes only the registers that differ from what the DMA continues with (a region that only changes the palette costs no pointer move) */
static uint32_t *emit_geom(uint32_t *p, St *st, const uint32_t np[8], uint16_t mod1, uint16_t mod2, uint16_t con1, int first)
{
    for (unsigned n = 0; n < 8; n++) {                     /* n = bitplane 1..8 -> index n */
        uint32_t o = st->ptr[n], v = np[n];
        if (first || (o >> 16) != (v >> 16)) MOVE(0x0E0 + 4 * n, v >> 16);
        if (first || (o & 0xFFFF) != (v & 0xFFFF)) MOVE(0x0E2 + 4 * n, v);
        st->ptr[n] = v;
    }
    if (first || mod1 != st->mod1) MOVE(0x108, mod1);
    if (first || mod2 != st->mod2) MOVE(0x10A, mod2);
    if (first || con1 != st->con1) MOVE(0x102, con1);
    st->mod1 = mod1; st->mod2 = mod2; st->con1 = con1;
    return p;
}

#define BG(cfg, k, row) (m->bmp + ((((unsigned)(cfg) * 4u + (k)) * 512u + (row)) * m->brow))
#define SP(k, row)      (m->spr + (((unsigned)(k) * 240u + (row)) * 32u))
#define X1(k, row)      (m->xpf1 + (((unsigned)(k) * 240u + (row)) * 32u))
#define X2(k, row)      (m->xpf2 + (((unsigned)(k) * 240u + (row)) * 32u))

/* Copper list of one picture from the regions of `f`; returns the number of longwords, or -1 if it does not fit */
int ar2_build(uint32_t *list, unsigned max, const ArFrame *f, const Ar2Mem *m)
{
    uint32_t *p = list, *end = list + max - 4; St st; uint16_t hi[32], lo[32]; int wrapped = 0;
    st.valid = 0; uint32_t prevmask = ~0u; unsigned prevh = 0;
    MOVE(0x106, BPLCON3_BASE2);                           /* BPLCON3 stays at this value outside the low-nibble pass */
    for (unsigned i = 0; i < 8; i++) { MOVE(0x120 + i * 4, (uint32_t)plat_null_sprite >> 16); MOVE(0x122 + i * 4, (uint32_t)plat_null_sprite); }
    for (int i = 0; i < f->nreg && p < end - 80; i++) {
        const ArRegion *r = &f->reg[i]; uint32_t np[8]; uint16_t mod1, mod2, con1 = 0; unsigned y0 = (unsigned)(r->y0 < 0 ? 0 : r->y0);
        if (r->kind == AR_BG) {
            unsigned N = (unsigned)r->bytex * 8 + r->fx, rr = N & 15, w0, s;
            if (rr == 0) { w0 = ((N >> 4) + 31) & 31; s = 0; } else { w0 = N >> 4; s = 16 - rr; }
            for (unsigned k = 0; k < 4; k++) { np[2 * k] = (uint32_t)BG(r->cfg, k, r->row0) + 2 * w0; np[2 * k + 1] = (uint32_t)SP(k, y0) - 2; }
            mod1 = (uint16_t)(m->brow - 34); mod2 = (uint16_t)(32 - 34); con1 = (uint16_t)s;
        } else if (r->kind == AR_EXACT) {
            for (unsigned k = 0; k < 4; k++) { np[2 * k] = (uint32_t)X1(k, y0) - 2; np[2 * k + 1] = (uint32_t)X2(k, y0) - 2; }
            mod1 = mod2 = (uint16_t)(32 - 34);
        } else {                                                           /* AR_BLANK: backdrop only: the same zero words on every line */
            for (unsigned k = 0; k < 8; k++) np[k] = (uint32_t)zero_row;
            mod1 = mod2 = (uint16_t)(-34);
        }
        pal_words(r->pal, r->gray, r->emph, hi, lo);
        unsigned h = (unsigned)(r->y1 - r->y0 + 1), adv1 = r->kind == AR_BG ? m->brow : r->kind == AR_EXACT ? 32 : 0, adv2 = r->kind == AR_BLANK ? 0 : 32;
        uint32_t mask = (m->used && r->kind == AR_BG) ? regmask(m->used[i]) : ~0u;
        if (i == 0) { p = emit_pal(p, &st, hi, lo, ~0u); p = emit_geom(p, &st, np, mod1, mod2, con1, 1); advance(&st, h, adv1, adv2); prevmask = mask; prevh = h; continue; }
        unsigned line = plat_vstart + (r->y0 > 0 ? (unsigned)r->y0 - 1 : 0);
        uint32_t early = prevh >= 2 ? mask & ~prevmask : 0;                 /* colours the previous region does not show: written one line earlier, in the hblank before the last line of that region */
        if (early) {
            unsigned l2 = line - 1;
            if (l2 > 255 && !wrapped) { *p++ = 0xFFDFFFFEu; wrapped = 1; }
            WAITL(l2 & 0xFF, 0xCA);
            p = emit_pal(p, &st, hi, lo, early);
        }
        if (line > 255 && !wrapped) { *p++ = 0xFFDFFFFEu; wrapped = 1; }
        WAITL(line & 0xFF, 0xCA);                                          /* after the last fetch of the previous line, before the first one of the region */
        p = emit_geom(p, &st, np, mod1, mod2, con1, 0);
        p = emit_pal(p, &st, hi, lo, mask);
        advance(&st, h, adv1, adv2);
        prevmask = mask; prevh = h;
    }
    *p++ = 0xFFFFFFFEu;
    return (int)(p - list);
}
