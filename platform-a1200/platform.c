/*
 * platform.c - Amiga 1200 (AGA, 68EC020) hardware layer: reset, display, Copper, game clock, keyboard, joysticks.
 * Derived from the A1200 layer of the Zoo Keeper port; see platform.h.  Paula is driven by audio.c.
 */
#include "platform.h"

/* ---------------------------------------------------------------- registers */
#define R16(o)   (*(volatile uint16_t *)(0xDFF000u + (o)))
#define R32(o)   (*(volatile uint32_t *)(0xDFF000u + (o)))
#define CIAA(r)  (*(volatile uint8_t *)(0xBFE001u + (unsigned)(r) * 0x100u))
#define CIAB(r)  (*(volatile uint8_t *)(0xBFD000u + (unsigned)(r) * 0x100u))
#define C_PRA 0x0
#define C_PRB 0x1
#define C_DDRA 0x2
#define C_DDRB 0x3
#define C_TALO 0x4
#define C_TAHI 0x5
#define C_TBLO 0x6
#define C_TBHI 0x7
#define C_TOD0 0x8
#define C_TOD1 0x9
#define C_TOD2 0xA
#define C_SDR  0xC
#define C_ICR  0xD
#define C_CRA  0xE
#define C_CRB  0xF

#define DMACONR  0x002
#define VPOSR    0x004
#define VHPOSR   0x006
#define JOY0DAT  0x00A
#define JOY1DAT  0x00C
#define POTGOR   0x016
#define POTGO    0x034
#define DENISEID 0x07C
#define COP1LC   0x080
#define COPJMP1  0x088
#define DIWSTRT  0x08E
#define DIWSTOP  0x090
#define DDFSTRT  0x092
#define DDFSTOP  0x094
#define DMACON   0x096
#define CLXCON   0x098
#define INTENA   0x09A
#define INTREQ   0x09C
#define INTREQR  0x01E
#define ADKCON   0x09E
#define AUDx(ch,o) (0x0A0 + (ch) * 0x10 + (o))
#define BPLCON0  0x100
#define BPLCON1  0x102
#define BPLCON2  0x104
#define BPLCON3  0x106
#define BPL1MOD  0x108
#define BPL2MOD  0x10A
#define BPLCON4  0x10C
#define CLXCON2  0x10E
#define SPR0PT   0x120
#define SPRx(n,o) (0x140 + (n) * 8 + (o))
#define COLOR00  0x180
#define DIWHIGH  0x1E4
#define FMODE    0x1FC

#ifndef PLAT_POTGO_INIT
#define PLAT_POTGO_INIT 0xFF00
#endif

/* AGA display programming (values proven in FS-UAE by the previous A1200 port): 32-bit bitplane fetch (FMODE 1) halves the DMA slots the
 * 8 planes take away from the CPU; DDFSTRT/STOP 0x40/0xB0 place a 256 pixel picture at DIWSTRT 0xA1. */
#ifndef PLAT_PLANES
#ifdef PPU_FB5
#define PLAT_PLANES 5
#else
#define PLAT_PLANES 8
#endif
#define PLAT_PLANES_DEFAULT 1            /* bitplanes fetched (8 = 256 colours); fewer/0 only for the DMA contention measurements */
#endif
#ifndef PLAT_FMODE
#define PLAT_FMODE 1
#endif
#if PLAT_PLANES == 8
#define BPLCON0_8PL  0x0211       /* BPU3 (bit 4) = 8 planes with BPU2..0 = 0, COLOR (bit 9), lores, ECSENA (bit 0: enables BRDRBLNK in BPLCON3) */
#else
#define BPLCON0_8PL  (0x0201 | (PLAT_PLANES << 12))
#endif
#define BPLCON3_BASE 0x0C20       /* BANK 0, PF2OF default (011), LOCT 0, SPRES lores, BRDRBLNK (border blanked: always black) */
#define BPLCON3_LOCT 0x0200
#define BPLCON4_BASE 0x0011       /* default: no bitplane XOR, sprite colour banks 1 (unused, sprites are off) */
#if PLAT_FMODE == 0
#define FMODE_VAL 0x0000
#define DDF_STRT 0x0048
#define DDF_STOP 0x00C0
#elif PLAT_FMODE == 1
#define FMODE_VAL    0x0001
#define DDF_STRT     0x0040
#define DDF_STOP     0x00B0
#else
#define FMODE_VAL 0x0003
#define DDF_STRT 0x0038
#define DDF_STOP 0x00B8
#endif

/* DMACON / INTENA bits */
#define SETBIT   0x8000
#define DMA_ALL  0x7FFF
#define DMAEN    0x0200
#define BPLEN    0x0100
#define COPEN    0x0080
#define BLTEN    0x0040
#define INT_PORTS 0x0008
#define INT_VERTB 0x0020
#define INT_EXTER 0x2000
#define INT_INTEN 0x4000

/* ---------------------------------------------------------------- game clock
 * One tick = one NES frame = 29780.5 CPU cycles of 21477272.7/12 Hz = 357366 master clocks of 236.25/11 MHz = 3931026/236250000 s.
 * E-clock = colour clock / 5: PAL 3546895/5 = 709379 Hz, NTSC 3579545/5 = 715909 Hz.
 *   PAL : 11803 + 21423809/39375000 E cycles per tick (11803.5441)      NTSC: 11912 + 7815439/39375000 (11912.1985)
 * A CIA timer with latch L has period L+1 cycles.  Each underflow the ISR adds `frac` to an accumulator; when it passes DEN the next period is
 * base+1, else base: the long-term mean period is exact. */
#define TICK_DEN PLAT_TICK_DEN
static const struct { uint16_t base; uint32_t frac; } tick_parm[2] = {
    { 11912, 7815439 },    /* [0] NTSC */
    { 11803, 21423809 },   /* [1] PAL  */
};

/* ------------------------------------------------------------------- state */
volatile uint32_t plat_tick_count, plat_vbl_count;
uint8_t  plat_is_pal;
uint16_t plat_lines;
uint16_t plat_vstart;
uint16_t plat_tick_base;
uint32_t plat_tick_frac;
volatile uint8_t  plat_keymap[16];

static PlatCopper plat_cop __attribute__((aligned(8), section(".chipbuf")));
uint8_t plat_buf[2][PLAT_BUF_SIZE] __attribute__((section(".chipbuf"), aligned(16)));
uint16_t plat_null_sprite[8] __attribute__((aligned(8), section(".chipbuf")));
#define null_sprite plat_null_sprite
volatile uint32_t plat_cop_ptr[2];   /* all zero = end of sprite */

static uint16_t pal_hi[256], pal_lo[256];   /* high/low nibbles of every channel, as written with LOCT = 0 / 1 */
static volatile uint8_t pal_pending;
static volatile int8_t  show_req = -1;      /* buffer to display from the next vertical blank, -1: none */
static uint16_t tick_base;
static uint32_t tick_frac, tick_acc;


/* free-running 16-bit E-clock counter (counts down; wraps every 92 ms): differences of two readings are valid up to that long */
uint16_t plat_eclock(void)
{
    uint8_t h = CIAB(C_TBHI), l = CIAB(C_TBLO);
    if (CIAB(C_TBHI) != h) { h = CIAB(C_TBHI); l = CIAB(C_TBLO); }
    return (uint16_t)((h << 8) | l);
}

/* ---------------------------------------------------------------- utilities */
static inline uint16_t irq_lock_vbl(void)
{
    uint16_t old;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0300,%%sr" : "=d"(old) : : "memory", "cc");
    return old;
}
static inline void irq_restore(uint16_t sr) { __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc"); }

static uint16_t beam_line(void)         /* current beam line (0..311) without tearing between VPOSR and VHPOSR */
{
    for (;;) {
        uint16_t a = R16(VPOSR), b = R16(VHPOSR), c = R16(VPOSR);
        if (((a ^ c) & 1) == 0) return (uint16_t)(((a & 1) << 8) | (b >> 8));
    }
}

/* ---------------------------------------------------------------- hw reset  */
void plat_hw_reset(void)
{
    unsigned i;
    (void)R16(DMACONR);
    for (i = 0; i < 400000u && (R16(DMACONR) & 0x4000); i++) { }   /* let a Blitter operation left by the OS finish */
    R16(INTENA) = DMA_ALL; R16(INTREQ) = DMA_ALL; R16(DMACON) = DMA_ALL; R16(ADKCON) = DMA_ALL;
    R16(INTREQ) = DMA_ALL; R16(INTREQ) = DMA_ALL;
    for (i = 0; i < 4; i++) { R16(AUDx(i, 8)) = 0; R16(AUDx(i, 4)) = 1; R16(AUDx(i, 6)) = 124; }
    for (i = 0; i < 8; i++) { R16(SPRx(i, 0)) = 0; R16(SPRx(i, 4)) = 0; R16(SPRx(i, 6)) = 0; R16(SPRx(i, 2)) = 0; }
    R16(FMODE) = 0; R16(BPLCON0) = 0x0200; R16(BPLCON1) = 0; R16(BPLCON2) = 0; R16(BPLCON3) = BPLCON3_BASE; R16(BPLCON4) = BPLCON4_BASE;
    R16(CLXCON) = 0; R16(CLXCON2) = 0; R16(BPL1MOD) = 0; R16(BPL2MOD) = 0; R16(COLOR00) = 0x0000;
    R16(POTGO) = PLAT_POTGO_INIT;
    CIAA(C_ICR) = 0x7F; CIAA(C_CRA) = 0; CIAA(C_CRB) = 0; (void)CIAA(C_ICR);
    CIAA(C_DDRA) = 0x03;                        /* bit0 OVL, bit1 LED out; 6,7 = fire in */
    CIAA(C_PRA)  = (uint8_t)(CIAA(C_PRA) & ~0x01u);
    CIAB(C_ICR) = 0x7F; CIAB(C_CRA) = 0; CIAB(C_CRB) = 0; (void)CIAB(C_ICR);
    CIAB(C_DDRB) = 0xFF; CIAB(C_PRB) = 0xFF; CIAB(C_PRB) = 0xF7; CIAB(C_PRB) = 0xFF;   /* floppy: motor off, deselect */
    R16(INTREQ) = DMA_ALL;
}

/* Count the lines of a complete frame by watching the beam counter: highest line number between two wraps is 311 (PAL) / 261 (NTSC). */
static uint16_t measure_lines(void)
{
    uint16_t prev = beam_line(), cur, max = 0;
    unsigned wraps = 0, guard = 0;
    while (wraps < 2 && ++guard < 8000000u) {
        cur = beam_line();
        if (cur + 100u < prev) { wraps++; if (wraps == 1) max = 0; }
        else if (wraps == 1 && cur > max) max = cur;
        prev = cur;
    }
    return (uint16_t)(max + 1);
}

/* ------------------------------------------------------------ keyboard ISR  */
static void kbd_handshake(void)
{
    unsigned n = 0;
    uint8_t last = (uint8_t)(R16(VHPOSR) >> 8);
    CIAA(C_CRA) = (uint8_t)(CIAA(C_CRA) | 0x40);            /* SP low for >= 75 us: wait for 3 raster-line changes */
    while (n < 3) { uint8_t l = (uint8_t)(R16(VHPOSR) >> 8); if (l != last) { last = l; n++; } }
    CIAA(C_CRA) = (uint8_t)(CIAA(C_CRA) & ~0x40);
}

void plat_isr_ports_c(void)
{
    uint8_t icr = CIAA(C_ICR);                  /* read clears the CIA interrupt */
    if (icr & 0x08) {                           /* SP: key byte received */
        uint8_t d = (uint8_t)~CIAA(C_SDR);      /* wire format: ~(code<<1 | up) */
        kbd_handshake();
        uint8_t code = (d >> 1) & 0x7F, up = d & 1;
        if (code < 0x78) {
            uint8_t mask = (uint8_t)(1u << (code & 7));
            if (up) plat_keymap[code >> 3] &= (uint8_t)~mask; else plat_keymap[code >> 3] |= mask;
        }
    }
    R16(INTREQ) = INT_PORTS;
}

/* ---------------------------------------------------------------- game clock ISR  */
void plat_isr_ciab_c(void)
{
    uint8_t icr = CIAB(C_ICR);                  /* read clears; do this BEFORE the INTREQ clear */
    if (icr & 0x01) {                           /* timer A underflow */
        uint16_t p = tick_base;
        plat_tick_count++;
        tick_acc += tick_frac;
        if (tick_acc >= TICK_DEN) { tick_acc -= TICK_DEN; p++; }
        p--;                                    /* latch = period - 1 */
        CIAB(C_TALO) = (uint8_t)p;              /* running timer: only the latch changes */
        CIAB(C_TAHI) = (uint8_t)(p >> 8);
    }
    R16(INTREQ) = INT_EXTER;
}

/* ---------------------------------------------------------------- VBL ISR   */
static void apply_bpl_ptrs(unsigned buf)
{
    uint32_t base = (uint32_t)plat_buf[buf];
    for (unsigned p = 0; p < PLAT_DEPTH; p++) {
        uint32_t a = base + p * PLAT_PLANE_STRIDE;
        plat_cop.bpl[p][1] = (uint16_t)(a >> 16);
        plat_cop.bpl[p][3] = (uint16_t)a;
    }
}

__attribute__((weak)) void plat_prof_pc(uint32_t pc) { (void)pc; }

/* AGA colour registers are written in banks of 32 (BPLCON3 BANK) in two passes: LOCT = 0 sets the high nibbles of R, G and B, LOCT = 1 the low nibbles. */
static void load_palette(void)
{
    volatile uint16_t *col = (volatile uint16_t *)(0xDFF000u + COLOR00);
#ifdef PPU_FB5
    for (unsigned bank = 0; bank < 1; bank++) {
#else
    for (unsigned bank = 0; bank < 8; bank++) {
#endif
        const uint16_t *hi = &pal_hi[bank * 32], *lo = &pal_lo[bank * 32];
        R16(BPLCON3) = (uint16_t)(BPLCON3_BASE | (bank << 13));
        for (unsigned i = 0; i < 32; i++) col[i] = hi[i];
        R16(BPLCON3) = (uint16_t)(BPLCON3_BASE | (bank << 13) | BPLCON3_LOCT);
        for (unsigned i = 0; i < 32; i++) col[i] = lo[i];
    }
    R16(BPLCON3) = BPLCON3_BASE;
}

void plat_isr_vbl_c(void)
{
    if (!(R16(INTREQR) & INT_VERTB)) { R16(INTREQ) = 0x0050; return; }
    R16(INTREQ) = INT_VERTB;
    plat_vbl_count++;
    if (show_req >= 0) { if (plat_cop_ptr[(unsigned)show_req]) R32(COP1LC) = plat_cop_ptr[(unsigned)show_req]; else apply_bpl_ptrs((unsigned)show_req); show_req = -1; }   /* before the Copper reloads the pointers at line PLAT_COP_SETUP_LINE */
    if (pal_pending) { pal_pending = 0; load_palette(); }
}

/* ---------------------------------------------------------------- video API */
void plat_show(unsigned buf) { show_req = (int8_t)buf; }
int  plat_show_pending(void) { return show_req >= 0; }

void plat_wait_vbl(void)
{
    uint32_t c = plat_vbl_count;
    while (plat_vbl_count == c) { }
}

__attribute__((section(".text.idle"))) void plat_wait_ticks(uint32_t t)
{
    while ((int32_t)(plat_tick_count - t) < 0) __asm__ volatile("stop #0x2000" : : : "memory");
}

void plat_set_palette32(const uint32_t rgb[32])
{
    uint16_t sr = irq_lock_vbl();
    unsigned changed = 0;
    for (unsigned i = 0; i < 32; i++) {
        uint32_t c = rgb[i], r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        uint16_t hi = (uint16_t)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)), lo = (uint16_t)(((r & 15) << 8) | ((g & 15) << 4) | (b & 15));
        if (hi != pal_hi[i] || lo != pal_lo[i]) { pal_hi[i] = hi; pal_lo[i] = lo; changed = 1; }
    }
    if (changed) pal_pending = 1;
    irq_restore(sr);
}

void plat_set_palette256(const uint32_t rgb[256])
{
    uint16_t sr = irq_lock_vbl();
    for (unsigned i = 0; i < 256; i++) {
        uint32_t c = rgb[i], r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        pal_hi[i] = (uint16_t)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
        pal_lo[i] = (uint16_t)(((r & 15) << 8) | ((g & 15) << 4) | (b & 15));
    }
    pal_pending = 1;
    irq_restore(sr);
}

void plat_set_color(unsigned idx, uint16_t rgb)    /* immediate, bank 0 only (error screens, before/without the vertical-blank loader) */
{
    R16(COLOR00 + 2 * (idx & 31)) = rgb & 0x0FFF;
}

/* measurement builds (PLAT_PLANES < 8): switch the full 8-plane display on so that the telemetry page can be read from a screenshot */
void plat_display_full(void)
{
#ifdef PPU_FB5
    R16(BPLCON0) = 0x5201;
#else
    R16(BPLCON0) = 0x0211;
#endif
    R16(FMODE) = 0x0001; R16(DDFSTRT) = 0x0040; R16(DDFSTOP) = 0x00B0;
    R16(DMACON) = SETBIT | DMAEN | BPLEN;
}

/* -------------------------------------------------------------------- input */
uint16_t plat_joy_bits(unsigned port)
{
    uint16_t j = R16(port ? JOY0DAT : JOY1DAT), b = 0;
    if (j & 0x0200) b |= PIN_LEFT;
    if (j & 0x0002) b |= PIN_RIGHT;
    if ((j ^ (j >> 1)) & 0x0001) b |= PIN_DOWN;     /* bit1 ^ bit0 */
    if ((j ^ (j >> 1)) & 0x0100) b |= PIN_UP;       /* bit9 ^ bit8 */
    if (!(CIAA(C_PRA) & (port ? 0x40 : 0x80))) b |= PIN_FIRE;      /* active low: port 1 = PRA bit 6, port 2 = bit 7 */
    if (!(R16(POTGOR) & (port ? 0x0400 : 0x4000))) b |= PIN_BTN2;  /* DATLY (port 1) / DATRY (port 2), active low */
    return b;
}

/* --------------------------------------------------------------------- init */
void plat_init(void)
{
    unsigned i;
    if ((R16(DENISEID) & 0xFF) != 0xF8) { for (;;) R16(COLOR00) = 0x0F00; }       /* AGA required: solid red */

    plat_lines  = measure_lines();
    plat_is_pal = (plat_lines >= 286);
    plat_vstart = plat_is_pal ? PLAT_PAL_VSTART : PLAT_NTSC_VSTART;

    for (i = 0; i < 8; i++) {
        plat_cop.spr[i][0] = (uint16_t)(SPR0PT + i * 4);
        plat_cop.spr[i][1] = (uint16_t)((uint32_t)null_sprite >> 16);
        plat_cop.spr[i][2] = (uint16_t)(SPR0PT + i * 4 + 2);
        plat_cop.spr[i][3] = (uint16_t)(uint32_t)null_sprite;
    }
    plat_cop.wait[0] = (uint16_t)((14 << 8) | 1);   /* WAIT line 14, h = 0: the pointers are reloaded once per frame */
    plat_cop.wait[1] = 0xFFFE;
    for (i = 0; i < PLAT_DEPTH; i++) { plat_cop.bpl[i][0] = (uint16_t)(0x0E0 + i * 4); plat_cop.bpl[i][2] = (uint16_t)(0x0E2 + i * 4); }
    apply_bpl_ptrs(0);
    plat_cop.end[0] = 0xFFFF; plat_cop.end[1] = 0xFFFE;
    pal_pending = 1;                           /* palette (if set) is loaded in the first vertical blank */
    for (i = 0; i < 256; i++) { pal_hi[i] = 0; pal_lo[i] = 0; }

    {   /* display window: 256x240, lores, 8 planes, interleaved */
        unsigned vs = plat_vstart, ve = plat_vstart + PLAT_H;
        R16(DIWSTRT) = (uint16_t)(((vs & 0xFF) << 8) | 0xA1);   /* H = 0xA1 */
        R16(DIWSTOP) = (uint16_t)(((ve & 0xFF) << 8) | 0xA1);   /* H = 0x1A1 (bit 8 implied) */
        R16(DIWHIGH) = 0x2100;                  /* H8 of DIWSTOP = 1, V8 of DIWSTOP = 1 (stop line 292 PAL / 261 NTSC), start lines < 256 */
        R16(DDFSTRT) = DDF_STRT; R16(DDFSTOP) = DDF_STOP;
        R16(FMODE) = FMODE_VAL; R16(BPLCON0) = BPLCON0_8PL; R16(BPLCON1) = 0; R16(BPLCON2) = 0;
        R16(BPLCON3) = BPLCON3_BASE; R16(BPLCON4) = BPLCON4_BASE;
        R16(BPL1MOD) = PLAT_MOD; R16(BPL2MOD) = PLAT_MOD;
    }
    R32(COP1LC) = (uint32_t)&plat_cop;
    R16(COPJMP1) = 0;

    {   /* CIA-B timer A = game clock; CIA-B TOD = scanline counter */
        const unsigned t = plat_is_pal ? 1 : 0;
        tick_base = tick_parm[t].base; tick_frac = tick_parm[t].frac; tick_acc = 0;
        plat_tick_base = tick_base; plat_tick_frac = tick_frac;
        uint16_t latch = (uint16_t)(tick_base - 1);
        CIAB(C_CRB) = 0x00;
        CIAB(C_TOD2) = 0; CIAB(C_TOD1) = 0; CIAB(C_TOD0) = 0;
        CIAB(C_TALO) = (uint8_t)latch; CIAB(C_TAHI) = (uint8_t)(latch >> 8);
        CIAB(C_CRA) = 0x11;                     /* continuous, force-load, START */
        CIAB(C_TBLO) = 0xFF; CIAB(C_TBHI) = 0xFF; CIAB(C_CRB) = 0x01;   /* timer B: free-running E-clock counter for time measurements */
        (void)CIAB(C_ICR);
        CIAB(C_ICR) = 0x81;                     /* enable the timer A interrupt */
    }
    kbd_handshake();
    (void)CIAA(C_ICR);
#if defined(PROFILE) || defined(PCTRACE)        /* PC sampling at ~573 Hz (CIA-A timer A), not commensurate with the game clock */
    CIAA(C_TALO) = 1237 & 255; CIAA(C_TAHI) = 1237 >> 8; CIAA(C_CRA) = 0x11;
    CIAA(C_ICR) = 0x89;
#else
    CIAA(C_ICR) = 0x88;
#endif
    R16(INTREQ) = DMA_ALL;
#if PLAT_PLANES == 0
    R16(DMACON) = SETBIT | DMAEN | COPEN | BLTEN;       /* measurement: no bitplane DMA at all */
#else
    R16(DMACON) = SETBIT | DMAEN | BPLEN | COPEN | BLTEN;
#endif
    R16(INTENA) = SETBIT | INT_INTEN | INT_EXTER | INT_VERTB | INT_PORTS;
    __asm__ volatile("move.w #0x2000,%%sr" : : : "memory");
}
