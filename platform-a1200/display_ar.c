/* display_ar.c - ARENDER_HW display: the 5-plane picture composed by runtime/arender.c (ar_out) is copied into the back display buffer (plane-major, 5 x 7680 bytes in chip RAM)
 * and a Copper list per buffer carries everything that belongs to the picture: sprite pointers (null), the 32 colour registers of the first region, the bitplane pointers,
 * and for every further region the colour registers that differ from the previous region, written just before the region's first line.  The vertical-blank interrupt
 * switches COP1LC to the list of the new buffer (plat_show).  Replaces display.c / c2p.s.  See docs/AMIGA.md "ARENDER_HW". */
#include <stdint.h>
#include "platform.h"
#include "display.h"
#include "../runtime/nes.h"
#include "../runtime/arender.h"

extern const uint32_t nes_rgb[256];
#define COPW 4096                                   /* longwords per Copper list (16 KB): header 36 + 66 per region with a full palette change */
static uint32_t cop[2][COPW] __attribute__((aligned(4), section(".chipbuf")));
uint32_t display_pairs_last;
static uint8_t back, frozen;

unsigned display_back(void) { return back; }
void display_invalidate(void) { }
void display_freeze(void) { frozen = 1; }
void display_c2p_pair(const uint8_t *src, uint8_t *dst) { (void)src; (void)dst; }

#define BPLCON3_BASE 0x0C20
#define BPLCON3_LOCT 0x0200
#define MOVE(reg, val) (*p++ = ((uint32_t)(reg) << 16) | (uint16_t)(val))
#define WAITL(vp, hp)  (*p++ = ((uint32_t)((((vp) & 0xFF) << 8) | ((hp) & 0xFE) | 1) << 16) | 0xFFFEu)

static void pal_words(const ArRegion *r, uint16_t hi[32], uint16_t lo[32])
{
    unsigned gm = r->gray ? 0x30 : 0x3F, em = (r->emph & 3) << 6;
    for (unsigned i = 0; i < 32; i++) {
        uint32_t c = nes_rgb[(r->pal[i] & gm) | em], rr = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
#ifdef TELEM
        if (i == 0) { rr = g = b = 0x66; } else if (i == 31) { rr = g = b = 0; }      /* telemetry pages are drawn with indices 0 / 31 */
#endif
        hi[i] = (uint16_t)(((rr >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)); lo[i] = (uint16_t)(((rr & 15) << 8) | ((g & 15) << 4) | (b & 15));
    }
}

static uint32_t *emit_colours(uint32_t *p, const uint16_t hi[32], const uint16_t lo[32], const uint16_t *ohi, const uint16_t *olo)
{
    int n = 0;
    for (unsigned i = 0; i < 32; i++) if (!ohi || hi[i] != ohi[i] || lo[i] != olo[i]) { n = 1; break; }
    if (!n) return p;
    MOVE(0x106, BPLCON3_BASE);
    for (unsigned i = 0; i < 32; i++) if (!ohi || hi[i] != ohi[i]) MOVE(0x180 + 2 * i, hi[i]);
    MOVE(0x106, BPLCON3_BASE | BPLCON3_LOCT);
    for (unsigned i = 0; i < 32; i++) if (!ohi || lo[i] != olo[i]) MOVE(0x180 + 2 * i, lo[i]);
    MOVE(0x106, BPLCON3_BASE);
    return p;
}

/* A region whose palette differs in many colours needs more Copper MOVEs than fit into the horizontal blank (about 97 colour clocks, 4 per MOVE): the first line(s) of such a region then
 * show some of their colours late (screen transitions, about 1 frame in 1000, at most about 2 lines at the left edge).  Option: -DAR_SKIP_BIG_PALETTE=n shows no picture whose region switch
 * needs more than n MOVEs instead (the previous picture stays). */
static int build_list(unsigned b)
{
    uint32_t *p = cop[b], *end = cop[b] + COPW - 8;
    uint16_t hi[32], lo[32], ohi[32], olo[32];
    for (unsigned i = 0; i < 8; i++) { MOVE(0x120 + i * 4, (uint32_t)plat_null_sprite >> 16); MOVE(0x122 + i * 4, (uint32_t)plat_null_sprite); }
    for (unsigned k = 0; k < PLAT_DEPTH; k++) { uint32_t a = (uint32_t)plat_buf[b] + k * PLAT_PLANE_STRIDE; MOVE(0x0E0 + k * 4, a >> 16); MOVE(0x0E2 + k * 4, a); }
    int n = ar_out.nreg;
    if (n < 1) { ArRegion z; for (unsigned i = 0; i < 32; i++) z.pal[i] = nes.ppu.pal[i]; z.gray = nes.ppu.gray; z.emph = nes.ppu.emph; pal_words(&z, hi, lo); p = emit_colours(p, hi, lo, 0, 0); }
    else pal_words(&ar_out.reg[0], hi, lo), p = emit_colours(p, hi, lo, 0, 0);
    int wrapped = 0;
    for (int i = 1; i < n && p < end - 70; i++) {
        const ArRegion *r = &ar_out.reg[i], *q = &ar_out.reg[i - 1];
        if (r->gray == q->gray && r->emph == q->emph) { unsigned same = 1; for (unsigned j = 0; j < 32; j++) if (r->pal[j] != q->pal[j]) { same = 0; break; } if (same) continue; }
        for (unsigned j = 0; j < 32; j++) { ohi[j] = hi[j]; olo[j] = lo[j]; }
        pal_words(r, hi, lo);
        unsigned line = plat_vstart + (r->y0 > 0 ? r->y0 - 1 : 0);          /* the line before the region: the colours change in its horizontal blank */
        if (line > 255 && !wrapped) { *p++ = 0xFFDFFFFEu; wrapped = 1; }      /* WAIT line 255, end: the vertical position counter wraps */
        { unsigned nm = 3; for (unsigned j = 0; j < 32; j++) nm += (hi[j] != ohi[j]) + (lo[j] != olo[j]); 
#ifdef AR_SKIP_BIG_PALETTE
            if (nm > AR_SKIP_BIG_PALETTE) return 0;
#endif
        }
        WAITL(line & 0xFF, 0xD2);
        p = emit_colours(p, hi, lo, ohi, olo);
    }
    *p++ = 0xFFFFFFFEu;
    return 1;
}

int display_frame(const uint8_t *fb, uint8_t *rowdirty)
{
    (void)fb; (void)rowdirty;
    if (frozen || plat_show_pending()) return 0;
    unsigned b = back;
    for (unsigned k = 0; k < PLAT_DEPTH; k++) {
        const uint32_t *s = (const uint32_t *)ar_out.plane[k]; uint32_t *d = (uint32_t *)(plat_buf[b] + k * PLAT_PLANE_STRIDE);
        for (unsigned i = 0; i < 240 * AR_STRIDE / 16; i++) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d += 4; s += 4; }
    }
    if (!build_list(b)) return 0;
    plat_cop_ptr[b] = (uint32_t)cop[b];
    plat_show(b);
    back = (uint8_t)(b ^ 1);
    return 1;
}

void display_init(void)
{
    for (unsigned b = 0; b < 2; b++) { uint32_t *d = (uint32_t *)plat_buf[b]; for (unsigned i = 0; i < PLAT_BUF_SIZE / 4; i++) d[i] = 0; plat_cop_ptr[b] = 0; }
    back = 1;
    plat_show(0);
}
