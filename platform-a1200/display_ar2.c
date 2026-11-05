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
#include "../runtime/nes.h"

extern const uint32_t nes_rgb[256];
#define BPLCON3_BASE2 0x1020
#define BPLCON3_LOCT2 0x0200
#define MOVE(reg, val) (*p++ = ((uint32_t)(reg) << 16) | (uint16_t)(val))
#define WAITL(vp, hp)  (*p++ = ((uint32_t)((((vp) & 0xFF) << 8) | ((hp) & 0xFE) | 1) << 16) | 0xFFFEu)

static uint8_t zero_row[40] __attribute__((aligned(4), section(".chipbuf")));

typedef struct { uint16_t hi[32], lo[32]; uint32_t valid; } Pal;                 /* what one colour bank holds (bank 0: registers 0..31, bank 1: registers 128..159) */
typedef struct { uint32_t ptr[8]; uint16_t mod1, mod2, con1; Pal pal[2]; unsigned act, lastwait; } St;   /* act: bank shown (BPLCON4 XOR 0x80 selects bank 1), lastwait: line of the last Copper wait */
#define BANK1 0x8000                                                               /* BPLCON3 BANK field = 4: registers 128..159 */

static void pal_words(const uint8_t pal[32], unsigned gray, unsigned emph, uint16_t hi[32], uint16_t lo[32])
{
    unsigned gm = gray ? 0x30 : 0x3F, em = (emph & 3) << 6;
    for (unsigned r = 0; r < 32; r++) {
        uint32_t c = nes_rgb[(pal[r] & gm) | em], rr = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
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

/* colour registers of `bank` in `mask` that differ from the bank state (never written = differ): hi nibbles, then lo nibbles; the state follows */
static unsigned pal_items(const Pal *pl, const uint16_t hi[32], const uint16_t lo[32], uint32_t mask)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 32; i++) if (mask >> i & 1) { unsigned nv = !(pl->valid >> i & 1); n += (nv || hi[i] != pl->hi[i]) + (nv || lo[i] != pl->lo[i]); }
    return n;
}

static uint32_t *emit_pal(uint32_t *p, Pal *pl, unsigned bank, const uint16_t hi[32], const uint16_t lo[32], uint32_t mask)
{
    if (!pal_items(pl, hi, lo, mask)) return p;
    unsigned b3 = BPLCON3_BASE2 | (bank ? BANK1 : 0);
    if (bank) MOVE(0x106, b3);
    for (unsigned i = 0; i < 32; i++) if ((mask >> i & 1) && (!(pl->valid >> i & 1) || hi[i] != pl->hi[i])) MOVE(0x180 + 2 * i, hi[i]);
    MOVE(0x106, b3 | BPLCON3_LOCT2);
    for (unsigned i = 0; i < 32; i++) if ((mask >> i & 1) && (!(pl->valid >> i & 1) || lo[i] != pl->lo[i])) MOVE(0x180 + 2 * i, lo[i]);
    MOVE(0x106, BPLCON3_BASE2);
    for (unsigned i = 0; i < 32; i++) if (mask >> i & 1) { pl->hi[i] = hi[i]; pl->lo[i] = lo[i]; }
    pl->valid |= mask;
    return p;
}

#define PRE_CAP 16                                   /* colour MOVEs per horizontal blank in the pre-writes of the hidden bank (22 fit; the BPLCON3 moves take 2-3) */

/* write the hidden bank over the hblanks of lines [from, to) (one WAIT per line), PRE_CAP colour MOVEs each */
static uint32_t *prewrite(uint32_t *p, Pal *pl, unsigned bank, const uint16_t hi[32], const uint16_t lo[32], uint32_t mask, unsigned from, unsigned to, int *wrapped, unsigned *lastwait)
{
    unsigned pass = 0, i = 0, ln = from, left = pal_items(pl, hi, lo, mask);
    while (left) {
        if (ln > 255 && !*wrapped) { *p++ = 0xFFDFFFFEu; *wrapped = 1; }
        WAITL(ln & 0xFF, 0xCA); *lastwait = ln;
        unsigned cnt = 0, open = 0;
        while (left && cnt < PRE_CAP) {
            if (i == 32) { i = 0; pass++; open = 0; }
            if (!(mask >> i & 1)) { i++; continue; }
            unsigned nv = !(pl->valid >> i & 1), need = pass ? (nv || lo[i] != pl->lo[i]) : (nv || hi[i] != pl->hi[i]);
            if (need) {
                if (!open) { MOVE(0x106, BPLCON3_BASE2 | (bank ? BANK1 : 0) | (pass ? BPLCON3_LOCT2 : 0)); open = 1; }
                MOVE(0x180 + 2 * i, pass ? lo[i] : hi[i]); cnt++; left--;
                if (pass) pl->lo[i] = lo[i]; else pl->hi[i] = hi[i];
                if (pass) pl->valid |= 1u << i;
            }
            i++;
        }
        if (open) MOVE(0x106, BPLCON3_BASE2);
        ln++;
    }
    (void)to;
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

#if defined(TELEM) && defined(ARENDER_HW2)
/* telemetry (test builds): a 4-plane page drawn by telem_draw() (bits -> value 15 / 0) replaces the picture from line 224 (mode 1) or 0 (mode 2: profile page); value 0 = grey, 15 = black */
static uint8_t ar2_tel_raw[4 + 4 * 240 * 32] __attribute__((aligned(4), section(".chipbuf")));
uint8_t *const ar2_tel = ar2_tel_raw + 4; unsigned ar2_tel_mode = 1;
static uint32_t *emit_tel(uint32_t *p, St *st, unsigned y0)
{
    uint32_t np[8];
    for (unsigned k = 0; k < 4; k++) { np[2 * k] = (uint32_t)(ar2_tel + (k * 240 + y0) * 32) - 2; np[2 * k + 1] = (uint32_t)zero_row; }
    p = emit_geom(p, st, np, (uint16_t)(32 - 34), (uint16_t)-34, 0, 0);
    MOVE(0x180, 0x666); MOVE(0x19E, 0x000); MOVE(0x106, BPLCON3_BASE2 | BPLCON3_LOCT2); MOVE(0x180, 0x666); MOVE(0x19E, 0x000); MOVE(0x106, BPLCON3_BASE2);
    return p;
}
#endif

/* Copper list of one picture from the regions of `f`; returns the number of longwords, or -1 if it does not fit */
int ar2_build(uint32_t *list, unsigned max, const ArFrame *f, const Ar2Mem *m)
{
    uint32_t *p = list, *end = list + max - 4; St st; uint16_t hi[32], lo[32]; int wrapped = 0;
    st.pal[0].valid = st.pal[1].valid = 0; st.act = 0; st.lastwait = plat_vstart; uint32_t prevmask = ~0u; unsigned prevh = 0;
    MOVE(0x10C, 0x0011);                                  /* BPLCON4: no bitplane XOR (colour bank 0), sprite banks default */
    MOVE(0x106, BPLCON3_BASE2);                           /* BPLCON3 stays at this value outside the low-nibble pass */
    for (unsigned i = 0; i < 8; i++) { MOVE(0x120 + i * 4, (uint32_t)plat_null_sprite >> 16); MOVE(0x122 + i * 4, (uint32_t)plat_null_sprite); }
#if defined(TELEM) && defined(ARENDER_HW2)
    if (ar2_tel_mode == 2) { p = emit_tel(p, &st, 0); *p++ = 0xFFFFFFFEu; return (int)(p - list); }   /* only the page */
#endif
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
        { uint8_t pv[32]; ar_hw_palette(r, pv); pal_words(pv, r->gray, r->emph, hi, lo); }
        unsigned h = (unsigned)(r->y1 - r->y0 + 1), adv1 = r->kind == AR_BG ? m->brow : r->kind == AR_EXACT ? 32 : 0, adv2 = r->kind == AR_BLANK ? 0 : 32;
        uint32_t mask = (m->used && r->kind == AR_BG) ? regmask(m->used[i]) : ~0u;
        if (i == 0) { p = emit_pal(p, &st.pal[0], 0, hi, lo, ~0u); p = emit_geom(p, &st, np, mod1, mod2, con1, 1); advance(&st, h, adv1, adv2); prevmask = mask; prevh = h; continue; }
        unsigned line = plat_vstart + (r->y0 > 0 ? (unsigned)r->y0 - 1 : 0);
        unsigned usebank = 0, nlate = pal_items(&st.pal[st.act], hi, lo, mask);
        if (nlate > 6) {                                                   /* more colours than the hblank takes: write them into the hidden colour bank over the previous lines, switch with BPLCON4 */
            unsigned n = pal_items(&st.pal[st.act ^ 1], hi, lo, mask), avail = line > st.lastwait ? line - st.lastwait - 1 : 0;
            if (n <= avail * PRE_CAP) {
                usebank = 1; unsigned lw = st.lastwait;
                p = prewrite(p, &st.pal[st.act ^ 1], st.act ^ 1, hi, lo, mask, st.lastwait + 1, line, &wrapped, &lw); st.lastwait = lw;
            }
        }
        uint32_t early = (!usebank && prevh >= 2) ? mask & ~prevmask : 0;   /* colours the previous region does not show: written one line earlier, in the hblank before the last line of that region */
        if (early) {
            unsigned l2 = line - 1;
            if (l2 > 255 && !wrapped) { *p++ = 0xFFDFFFFEu; wrapped = 1; }
            WAITL(l2 & 0xFF, 0xCA); st.lastwait = l2;
            p = emit_pal(p, &st.pal[st.act], st.act, hi, lo, early);
        }
        if (line > 255 && !wrapped) { *p++ = 0xFFDFFFFEu; wrapped = 1; }
        WAITL(line & 0xFF, 0xCA); st.lastwait = line;                       /* after the last fetch of the previous line, before the first one of the region */
        p = emit_geom(p, &st, np, mod1, mod2, con1, 0);
        if (usebank) { st.act ^= 1; WAITL(line & 0xFF, 0xD4); MOVE(0x10C, st.act ? 0x8011 : 0x0011); }   /* the last pixels of the previous line are still being displayed at 0xCA */
        else p = emit_pal(p, &st.pal[st.act], st.act, hi, lo, mask);
        advance(&st, h, adv1, adv2);
        prevmask = mask; prevh = h;
    }
#if defined(TELEM) && defined(ARENDER_HW2)
    if (f->nreg > 0) {
        unsigned line = plat_vstart + 223;
        if (line > 255 && !wrapped) { *p++ = 0xFFDFFFFEu; wrapped = 1; }
        WAITL(line & 0xFF, 0xCA);
        p = emit_tel(p, &st, 224);
    }
#endif
    *p++ = 0xFFFFFFFEu;
    return (int)(p - list);
}

#ifdef ARENDER_HW2
/* ---- the live display: replaces display.c / display_ar.c (ARENDER_HW=2) ---- */
#include "display.h"
#define COPW 4096
static uint32_t cop[2][COPW] __attribute__((aligned(4), section(".chipbuf")));
static uint8_t hwmem[AR_HW_BYTES + 4] __attribute__((aligned(4), section(".chipbuf")));
uint32_t display_pairs_last;
static uint8_t back, frozen;
unsigned display_back(void) { return back; }
void display_invalidate(void) { }
void display_freeze(void) { frozen = 1; }
void display_c2p_pair(const uint8_t *src, uint8_t *dst) { (void)src; (void)dst; }
extern int ar_hw_enable;

static int build(unsigned b)
{
    static uint32_t used[AR_MAX_REGIONS];
    uint32_t *p = cop[b];
    *p++ = (0x1FCu << 16) | 0x0000; *p++ = (0x100u << 16) | 0x0611; *p++ = (0x104u << 16) | 0x0040; *p++ = (0x092u << 16) | 0x0040; *p++ = (0x094u << 16) | 0x00C0;
    for (int i = 0; i < ar_out.nreg; i++) used[i] = ar_out.reg[i].used;
    Ar2Mem m = { ar_hw.bmp, ar_hw.spr, ar_hw.xpf1, ar_hw.xpf2, AR_BROW, used };
    return ar2_build(p, COPW - 5, &ar_out, &m);
}

int display_frame(const uint8_t *fb, uint8_t *rowdirty)
{
    (void)fb; (void)rowdirty;
    if (frozen || plat_show_pending()) return 0;
    unsigned b = back;
    if (build(b) < 0) return 0;
    plat_cop_ptr[b] = (uint32_t)cop[b];
    plat_show(b);
    ar_hw_shown();
    back = (uint8_t)(b ^ 1);
    return 1;
}

void display_init(void)
{
    ar_hw_setmem(hwmem);
    ar_hw_enable = 1;
    for (unsigned b = 0; b < 2; b++) plat_cop_ptr[b] = 0;
    back = 1;
    plat_show(0);
}
#endif
