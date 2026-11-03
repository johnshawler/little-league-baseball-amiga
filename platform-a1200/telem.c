/* telem.c - test instrumentation (see telem.h). */
#include "platform.h"
#include "telem.h"
#ifdef PCTRACE
#include "../runtime/rt.h"
#endif

void telem_draw(uint8_t *buf, const uint32_t *w, unsigned nwords, unsigned line)
{
    for (unsigned i = 0; i < nwords; i++) {
        unsigned row = i >> 3, col = (i & 7) * 4;              /* byte column of the long */
        uint32_t v = w[i];
        for (unsigned k = 0; k < 4; k++) {
            uint8_t b = (uint8_t)(v >> (24 - 8 * k));
            for (unsigned l = 0; l < 2; l++) {
                uint8_t *d = buf + (line + row * 2 + l) * PLAT_STRIDE + col + k;
                for (unsigned p = 0; p < 8; p++) d[p * PLAT_PLANE_STRIDE] = b;
            }
        }
    }
}

#ifdef PCTRACE      /* debugging aid: the interrupted PC of every profiler interrupt is drawn into the picture (lines 232..) */
void plat_prof_pc(uint32_t pc)
{
    static uint32_t n; uint32_t w[8] = { TELEM_MAGIC ^ 2, pc, ++n, (uint32_t)rt.frames_done, (uint32_t)nes.ppu.dots, (uint32_t)(nes.ppu.line + 1), (uint32_t)rt.cyc, 0 };
    telem_draw(plat_buf[0], w, 7, 232); telem_draw(plat_buf[1], w, 7, 232);
}
#endif
#ifdef PROFILE
#ifndef PSH
#define PSH 5                       /* bin = 32 bytes of code */
#endif
extern char _pr0[], _pr1[], _pr2[], _pr3[], _pr4[], _pr5[], _pr6[], _pr7[], _pr8[], _pr10[], _idle0[], _idle1[];
#define PLIM 0x80000
static uint16_t prof[PLIM >> PSH];
uint32_t prof_total, prof_other;
extern volatile uint32_t plat_tick_count;
#ifndef PLO
#define PLO 600
#define PHI 1800
#endif
void plat_prof_pc(uint32_t pc)
{
    if (plat_tick_count < PLO || plat_tick_count >= PHI) return;
    prof_total++;
    if (pc >= 0x800 && pc < PLIM) { uint16_t *c = &prof[pc >> PSH]; if (*c < 65535) ++*c; } else prof_other++;
}
void telem_prof_report(uint8_t *buf)
{
    static uint32_t w[8 * 14];
    char *edges[12] = { _pr0,_pr1,_pr2,_pr3,_pr4,_pr5,_pr6,_pr7,_pr8,_idle0,_idle1,_pr10 };   /* idle range lies between _pr8 and _pr9 */
    w[0] = TELEM_MAGIC ^ 1; w[1] = prof_total; w[2] = prof_other;
    /* categories: translated code, rt_x.s, rt_x.c, rt_core, ppu_ref, cpu6502+mapper, apu, display, platform/main/audio, idle(STOP), libgcc/other */
    for (unsigned c = 0; c < 11; c++) {
        uint32_t a0 = (uint32_t)edges[c], a1 = (uint32_t)edges[c + 1], sum = 0;
        for (uint32_t a = a0 >> PSH; a < (a1 + (1u << PSH) - 1) >> PSH && a < (PLIM >> PSH); a++) {
            /* bins that straddle two categories are attributed to the one holding their start: link order keeps categories 32-byte aligned enough for this */
            if ((a << PSH) >= a0 && (a << PSH) < a1) sum += prof[a];
        }
        w[3 + c] = sum;
    }
    /* top bins */
    for (unsigned r = 0; r < 44; r++) {
        unsigned best = 0, bi = 0;
        for (unsigned i = 0; i < (PLIM >> PSH); i++) if (prof[i] > best) { best = prof[i]; bi = i; }
        if (!best) break;
        prof[bi] = 0;
        w[16 + 2 * r] = bi << PSH; w[17 + 2 * r] = best;
    }
    telem_draw(buf, w, 16 + 88, 0);
}
#else
void telem_prof_report(uint8_t *buf) { (void)buf; }
uint32_t prof_total;
#endif
