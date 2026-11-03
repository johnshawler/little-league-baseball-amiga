/* telem.h - test instrumentation: numbers drawn as black/grey pixels into the picture so that they can be read back from FS-UAE screenshots.
 * Compiled in only with -DTELEM (speed HUD) / -DPROFILE (PC sampling profile); the normal build has none of it. */
#ifndef TELEM_H
#define TELEM_H
#include <stdint.h>
#define TELEM_MAGIC 0x4C4C4231u            /* 'LLB1' first long of a page */
/* draw nwords longs, 8 per row, bit 31 first, 1 = colour index 255 (black), 0 = index 0 (grey), two display lines per row starting at display line `line` */
void telem_draw(uint8_t *buf, const uint32_t *w, unsigned nwords, unsigned line);
void telem_prof_report(uint8_t *buf);      /* profile page over the whole screen */
extern uint32_t prof_total;
#endif
