/* display_ar2.h - hardware side of arender stage 2b (see display_ar2.c) */
#ifndef DISPLAY_AR2_H
#define DISPLAY_AR2_H
#include <stdint.h>
#include "../runtime/arender.h"
typedef struct { uint8_t *bmp, *spr, *xpf1, *xpf2; unsigned brow; const uint32_t *used; } Ar2Mem;   /* bitmap base (cfg,plane,row) with row stride brow, sprite layer, exact-line playfields */
int ar2_build(uint32_t *list, unsigned max, const ArFrame *f, const Ar2Mem *m);
#endif
