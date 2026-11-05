/* arhw_test.c - ARHW_TEST: hardware test of the stage 2b display (display_ar2.c) without emulation.  Synthetic persistent bitmaps (stride 96, guard bytes), sprite layer and
 * exact layers are filled with pixel formulas, a region list covering the cases of the Copper builder (scroll 0 / N = 0 / N = 511 / odd fine X, row wrap restart, exact, blank, palette
 * changes, line > 255) is turned into a Copper list and shown.  tests/amiga/arhw_expect.py computes the same picture; the screenshot must equal it pixel for pixel. */
#include <stdint.h>
#include "platform.h"
#include "display.h"
#include "../runtime/arender.h"
#include "display_ar2.h"

#define BROW 96
#define NCFG 2
static uint8_t bmp[NCFG * 4 * 512 * BROW] __attribute__((aligned(4), section(".chipbuf")));
static uint8_t spr[4 * 240 * 32 + 4] __attribute__((aligned(4), section(".chipbuf")));
static uint8_t xp1[4 * 240 * 32 + 4] __attribute__((aligned(4), section(".chipbuf")));
static uint8_t xp2[4 * 240 * 32 + 4] __attribute__((aligned(4), section(".chipbuf")));
static uint32_t list[2048] __attribute__((aligned(4), section(".chipbuf")));
static ArFrame frame;

/* pixel formulas (mirrored in tests/amiga/arhw_expect.py) */
static unsigned bgv(unsigned c, unsigned r, unsigned x) { return (x + 3 * r + 5 * c + ((x >> 4) ^ (r >> 1))) & (c ? 7 : 15); }   /* configuration 1 shows only values 0..7 */
static unsigned spv(unsigned y, unsigned x) { return ((x + y) % 5 < 2) ? ((x * 3 + y) & 7) : 0; }
static unsigned x1v(unsigned y, unsigned x) { return (x * 5 + y * 3) & 15; }
static unsigned x2v(unsigned y, unsigned x) { return (x % 3) ? ((x + 2 * y) & 15) : 0; }

static void put(uint8_t *row, unsigned x, unsigned bit) { if (bit) row[x >> 3] |= (uint8_t)(0x80 >> (x & 7)); }

void arhw_test_main(void)
{
    display_init();
    for (unsigned c = 0; c < NCFG; c++) for (unsigned r = 0; r < 512; r++) {
        for (unsigned x = 0; x < 512; x++) { unsigned v = bgv(c, r, x); for (unsigned k = 0; k < 4; k++) put(bmp + ((c * 4 + k) * 512 + r) * BROW, x, (v >> k) & 1); }
        for (unsigned k = 0; k < 4; k++) { uint8_t *row = bmp + ((c * 4 + k) * 512 + r) * BROW; for (unsigned i = 0; i < 32; i++) row[64 + i] = row[i]; }
    }
    for (unsigned y = 0; y < 240; y++) for (unsigned x = 0; x < 256; x++) {
        unsigned s = spv(y, x), a = x1v(y, x), b = x2v(y, x);
        for (unsigned k = 0; k < 4; k++) { put(spr + (k * 240 + y) * 32, x, (s >> k) & 1); put(xp1 + (k * 240 + y) * 32, x, (a >> k) & 1); put(xp2 + (k * 240 + y) * 32, x, (b >> k) & 1); }
    }
    static const struct { int16_t y0, y1; uint8_t kind, cfg; uint16_t row0; uint8_t bytex, fx, pv; } T[] = {
        {   0,  31, AR_BG,    0,   0,  0, 0, 0 },     /* N = 0 */
        {  32,  63, AR_BG,    0, 100,  5, 3, 1 },     /* N = 43, odd fine X */
        {  64,  95, AR_BG,    1, 300, 63, 7, 2 },     /* N = 511 */
        {  96, 107, AR_BG,    0, 500, 16, 0, 2 },     /* rows 500..511, then restart at row 0 (wrap) */
        { 108, 127, AR_BG,    0,   0, 16, 0, 3 },
        { 128, 143, AR_EXACT, 0,   0,  0, 0, 3 },
        { 144, 159, AR_BLANK, 0,   0,  0, 0, 4 },
        { 160, 191, AR_BG,    1,   0, 31, 5, 4 },
        { 192, 215, AR_BG,    0, 200,  8, 0, 5 },
        { 216, 239, AR_BG,    0, 224,  8, 0, 6 },     /* first line > 255 on PAL (Copper wait wrap) */
    };
    frame.nreg = 10;
    for (unsigned i = 0; i < 10; i++) {
        ArRegion *r = &frame.reg[i];
        r->y0 = T[i].y0; r->y1 = T[i].y1; r->kind = T[i].kind; r->cfg = T[i].cfg; r->row0 = T[i].row0; r->bytex = T[i].bytex; r->fx = T[i].fx; r->gray = 0; r->emph = 0;
        for (unsigned e = 0; e < 32; e++) r->pal[e] = T[i].pv >= 6 ? (uint8_t)((e * 11 + 9 + 5 * T[i].pv) & 0x3F) : (uint8_t)((e * 7 + 3 + ((e % 8 == (unsigned)T[i].pv % 8) ? 5 * T[i].pv : 0) + (e == 17 ? i : 0)) & 0x3F);
    }
    static uint32_t used[10];
    for (unsigned i = 0; i < 10; i++) used[i] = (T[i].cfg ? 0x000000EFu : 0x0000FFFFu) | 0x00FE0000u;   /* palette entries shown: cfg 1: 0,1,2,3,5,6,7 (4 = backdrop), cfg 0: 0..15; sprites 17..23 */
    Ar2Mem m = { bmp, spr, xp1, xp2, BROW, used };
    uint32_t *p = list;
    *p++ = (0x1FCu << 16) | 0x0000;           /* FMODE 0: 16-bit fetch */
    *p++ = (0x100u << 16) | 0x0611;           /* 8 planes, dual playfield, colour */
    *p++ = (0x104u << 16) | 0x0040;           /* playfield 2 in front */
    *p++ = (0x092u << 16) | 0x0040;           /* DDFSTRT / DDFSTOP: 17 words per line */
    *p++ = (0x094u << 16) | 0x00C0;
    ar2_build(p, 2048 - 5, &frame, &m);
    plat_cop_ptr[0] = (uint32_t)list; plat_cop_ptr[1] = (uint32_t)list;
    plat_show(0);
    for (;;) __asm__ volatile("stop #0x2000");
}
