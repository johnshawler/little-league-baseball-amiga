/*
 * platform.h - Little League Baseball, Amiga 1200 platform layer (A1200 / AGA / 68EC020 / 2 MB chip RAM, no OS after the bootblock).
 * Derived from the proven A1200 layer of the Zoo Keeper port (boot, AGA display set-up, CIA tick, keyboard, joysticks).
 *
 *   CPU      68020 supervisor mode, VBR = 0, instruction cache on, data cache off.  All RAM is chip RAM.
 *   Display  256x240 lores, 8 interleaved bitplanes (256 colours), border blanked; two buffers, flipped by the vertical-blank interrupt.
 *            PAL : display lines 52..291 (240 lines, centred in the 256 line standard window)
 *            NTSC: display lines 21..260 (240 lines; 21 is the first line the beam can display on a 262 line frame)
 *   Time     CIA-B timer A = game clock, period = one NES frame (29780.5 CPU cycles of 1.789773 MHz = 60.0988 Hz) in E-clock cycles,
 *            fractional part by a Bresenham reload (exact on average); CIA-B TOD counts beam lines (diagnostic).
 *   IRQs     level 2 CIA-A (keyboard, profiler), level 3 VERTB (buffer flip), level 4 Paula audio, level 6 CIA-B timer A (game clock).
 */
#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdint.h>

#define PLAT_W            256
#define PLAT_H            240
#if defined(ARENDER_HW)
#define PLAT_DEPTH        5                        /* arender: 5 separate planes of 32 bytes per row (plane-major), Copper list per picture */
#define PLAT_STRIDE       32
#define PLAT_PLANE_STRIDE 7680
#elif defined(PPU_FB5)
#define PLAT_DEPTH        5                        /* 5 planes = 32 colours = the NES palette RAM */
#define PLAT_STRIDE       160                      /* bytes per line (all 5 planes interleaved) */
#else
#define PLAT_DEPTH        8
#define PLAT_STRIDE       256                      /* bytes per line (all 8 planes interleaved) */
#endif
#ifndef PLAT_PLANE_STRIDE
#define PLAT_PLANE_STRIDE 32                       /* bytes between the planes of one line */
#endif
#ifdef ARENDER_HW
#define PLAT_MOD 0                                 /* plane-major buffers: the next row of a plane follows directly */
#else
#define PLAT_MOD (PLAT_STRIDE - 32)
#endif
#define PLAT_BUF_SIZE     (PLAT_STRIDE * PLAT_H)   /* 61440 (8 planes) / 38400 (5 planes) */

#ifndef PLAT_PAL_VSTART
#define PLAT_PAL_VSTART   52      /* first display line, PAL  (VPOS) */
#endif
#ifndef PLAT_NTSC_VSTART
#define PLAT_NTSC_VSTART  21      /* first display line, NTSC (VPOS) */
#endif

/* Input bits returned by plat_joy_bits(port) */
#define PIN_UP      0x0001
#define PIN_DOWN    0x0002
#define PIN_LEFT    0x0004
#define PIN_RIGHT   0x0008
#define PIN_FIRE    0x0010
#define PIN_BTN2    0x0020

/* Amiga raw keycodes (make code; break = code | 0x80 on the wire) */
#define KEY_SPACE   0x40
#define KEY_RETURN  0x44
#define KEY_ENTER   0x43     /* keypad enter */
#define KEY_ESC     0x45

typedef struct {
    uint16_t spr[8][4];
    uint16_t wait[2];
    uint16_t bpl[8][4];
    uint16_t end[2];
} PlatCopper;

/* ------------------------------------------------------------------- state --- (volatile: readable at any time) */
extern volatile uint32_t plat_tick_count;   /* game clock ticks since plat_init (one per NES frame, 60.0988 Hz) */
extern volatile uint32_t plat_vbl_count;    /* vertical blanks since plat_init */
extern uint8_t  plat_is_pal;                /* 1 = PAL (>= 286 lines per frame), 0 = NTSC */
extern uint16_t plat_lines;                 /* measured lines per frame (312 / 262) */
extern uint16_t plat_vstart;                /* first display line in use */
#define PLAT_TICK_DEN 39375000u
extern uint16_t plat_tick_base;             /* game clock period = plat_tick_base + plat_tick_frac / PLAT_TICK_DEN E-clock cycles */
extern uint32_t plat_tick_frac;
uint16_t plat_eclock(void);                 /* free-running down-counting E-clock (709379 Hz PAL / 715909 Hz NTSC), wraps every 92 ms */
extern volatile uint8_t plat_keymap[16];    /* 128-bit key-down bitmap (raw codes) */

/* ------------------------------------------------------------------- system -*/
void plat_hw_reset(void);   /* called by crt0: quiesce all chips; safe from any hot state */
void plat_init(void);       /* called by crt0 once; sets up everything; enables IRQs */

/* -------------------------------------------------------------------- video */
extern uint8_t plat_buf[2][PLAT_BUF_SIZE];  /* the two display buffers (chip RAM) */
void plat_show(unsigned buf);               /* display buffer 0/1 from the next vertical blank on */
int  plat_show_pending(void);               /* a plat_show() has not been applied by the vertical-blank interrupt yet */
void plat_set_palette32(const uint32_t rgb[32]);     /* colour registers 0..31 (0x00RRGGBB), loaded by the next vertical-blank interrupt together with the buffer flip
                                                        (loads nothing if unchanged) */
void plat_set_palette256(const uint32_t rgb[256]);   /* 0x00RRGGBB, loaded by the next vertical blank */
void plat_set_color(unsigned idx, uint16_t rgb);     /* RGB444, colour register idx (< 32), immediate (error screens) */
void plat_wait_vbl(void);
extern volatile uint32_t plat_cop_ptr[2];   /* ARENDER_HW: Copper list of display buffer 0/1 (0 = use the built-in list); COP1LC is written by the vertical-blank interrupt together with the buffer flip */
extern uint16_t plat_null_sprite[8];
void plat_display_full(void);               /* 8 planes + bitplane DMA on (measurement builds with fewer planes, before the profile page is drawn) */

/* --------------------------------------------------------------------- time */
void plat_wait_ticks(uint32_t t);           /* wait (STOP) until plat_tick_count >= t */

/* -------------------------------------------------------------------- input */
uint16_t plat_joy_bits(unsigned port);      /* PIN_* bits.  port 0 = Amiga joystick port 2 (JOY1DAT, player 1), port 1 = Amiga port 1 (JOY0DAT, player 2) */
#define  plat_key_down(code)  ((plat_keymap[(code) >> 3] >> ((code) & 7)) & 1)

/* -------------------------------------------------------------------- audio (audio.c) */
extern void plat_prof_pc(uint32_t pc);      /* weak: PC sample of the profiler interrupt */
#endif
