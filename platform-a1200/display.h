/* display.h - chunky NES frame (uint8 per pixel: NES colour 0..63 | red emphasis << 6 | green emphasis << 7) -> 8 interleaved bitplanes (AGA).
 * The 256 colour registers hold the displayed colour of every possible pixel value (static palette, tools/gen_palette.py), so a pixel value IS
 * the colour index and the plane bits are its bits: no palette handling at run time.  Two buffers; the vertical-blank interrupt flips them.
 * Per buffer a chunky shadow of what it shows lets display_frame() convert only the 16-pixel groups that changed. */
#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdint.h>
void     display_init(void);                  /* palette, buffers cleared, first buffer shown */
/* Convert `fb` (240 rows of 256 bytes, 4-byte aligned) into the back buffer and show it from the next vertical blank.  Returns 1 if the picture
 * was converted, 0 if it was skipped because the previous one has not been displayed yet (the display is slower than the game on PAL). */
int      display_frame(const uint8_t *fb, uint8_t *rowdirty);   /* rowdirty: per-row flags set by the renderer (nes.ppu.rowdirty), cleared here; 0 = compare everything */
void     display_freeze(void);                /* no further pictures (profile page on screen) */
void     display_invalidate(void);            /* force a full conversion next time (after the telemetry overwrote picture lines) */
extern uint32_t display_pairs_last;           /* 16-pixel groups converted by the last display_frame() */
extern uint8_t display_shadow[2][240 * 256];
unsigned display_back(void);
/* unit-test hooks */
void     display_c2p_pair(const uint8_t *src, uint8_t *dst);
#endif
