/* display.c - NES chunky frame -> AGA bitplanes.  See display.h. */
#include <stdint.h>
#include "platform.h"
#include "display.h"
#ifdef PPU_FB5
#include "../runtime/nes.h"
#endif

extern const uint32_t nes_rgb[256];
extern uint16_t c2p_uni_words[256][8];
extern long c2p_frame(const uint8_t *src, uint8_t *dst, uint8_t *shadow, uint8_t *rowflags);
extern long c2p_frame_all(const uint8_t *src, uint8_t *dst);
extern void c2p_pair(const uint8_t *src, uint8_t *dst);

uint16_t c2p_uni_words[256][8] __attribute__((aligned(4)));   /* plane words of 16 pixels of one colour (all ones / all zeros) */
uint8_t display_shadow[2][240 * 256] __attribute__((aligned(4)));
uint32_t display_pairs_last;
static uint8_t back;
static uint8_t force_all[2], frozen;
static uint8_t rowflag[2][240];            /* per buffer: rows that changed since the buffer was last converted */

unsigned display_back(void) { return back; }
void display_c2p_pair(const uint8_t *src, uint8_t *dst) { c2p_pair(src, dst); }

#ifdef PPU_FB5
/* 5 planes: the framebuffer holds palette RAM indices 0..31; the 32 colour registers are loaded from a palette snapshot (palette RAM, gray, emphasis) when a picture is shown.
 * RGB = nes_rgb[(pal & (gray ? 0x30 : 0x3F)) | (red emphasis << 6) | (green emphasis << 7)] (blue emphasis is not displayed, as before). */
static void apply_palette(void)
{
    uint32_t rgb[32]; const Ppu *p = &nes.ppu; unsigned gm = p->gray ? 0x30 : 0x3F, em = (p->emph & 3) << 6;
    for (unsigned i = 0; i < 32; i++) rgb[i] = nes_rgb[(p->pal[i] & gm) | em];
#ifdef TELEM
    rgb[0] = 0x666666; rgb[31] = 0x000000;        /* telemetry pages are drawn with colour indices 0 / 31: make them readable (test builds only) */
#endif
    plat_set_palette32(rgb);
}
#endif

void display_init(void)
{
    for (unsigned v = 0; v < 256; v++) for (unsigned p = 0; p < 8; p++) c2p_uni_words[v][p] = ((v >> p) & 1) ? 0xFFFF : 0;
#ifdef PPU_FB5
    apply_palette();
#else
    plat_set_palette256(nes_rgb);
#endif
    for (unsigned b = 0; b < 2; b++) {
        uint32_t *s = (uint32_t *)display_shadow[b], *d = (uint32_t *)plat_buf[b];
        for (unsigned i = 0; i < 240 * 256 / 4; i++) s[i] = 0;
        for (unsigned i = 0; i < PLAT_BUF_SIZE / 4; i++) d[i] = 0;        /* colour index 0 everywhere: planes all zero */
        force_all[b] = 0;
    }
    back = 1;
    plat_show(0);
}

void display_invalidate(void) { force_all[0] = force_all[1] = 1; }

int display_frame(const uint8_t *fb, uint8_t *rowdirty)
{
    if (frozen || plat_show_pending()) return 0;
    unsigned b = back;
    for (unsigned y = 0; y < 240; y++) {          /* rows the renderer wrote since the last display (all rows if it does not track them) */
        if (!rowdirty || rowdirty[y]) { rowflag[0][y] = 1; rowflag[1][y] = 1; if (rowdirty) rowdirty[y] = 0; }
    }
    if (force_all[b]) {
        force_all[b] = 0;
        c2p_frame_all(fb, plat_buf[b]);
        uint32_t *s = (uint32_t *)display_shadow[b]; const uint32_t *f = (const uint32_t *)fb;
        for (unsigned i = 0; i < 240 * 256 / 4; i++) s[i] = f[i];
        display_pairs_last = 240 * 16;
    } else {
        display_pairs_last = (uint32_t)c2p_frame(fb, plat_buf[b], display_shadow[b], rowflag[b]);
    }
#ifdef PPU_FB5
    apply_palette();                              /* loaded by the vertical-blank interrupt that also flips the buffer */
#endif
    plat_show(b);
    back = (uint8_t)(b ^ 1);
    return 1;
}
void display_freeze(void) { frozen = 1; }
