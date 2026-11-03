/* rt_x.h - glue between the translated 68020 code and the runtime (shared by C and assembler: keep the offsets in rt_x.s in step) */
#ifndef RT_X_H
#define RT_X_H
#include <stdint.h>
#define XRT_OFF_OPENBUS 194      /* offsetof(Rt,openbus) on the 68k, used by rt_x.s (checked by a static assert in rt_x.c) */
typedef struct {
    uint8_t  nzc, vflag, misc, pad;     /* 0: pad (offset 3) = open-bus value for the next helper read; canonical flags: N,Z in P layout / V in bit 6 / I,D bits */
    int16_t  adj; uint16_t ptmp;        /* 4: access-cycle adjustment for the next helper call */
    uint32_t d[6];                      /* 8: d0=A d1=X d2=Y d3=S d4=C(0/FF) d5=cycle budget */
    void    *entry;                     /* 32 */
    void    *saved_sp;                  /* 36 */
    uint8_t *ram;                       /* 40: a0 */
    uint8_t *win;                       /* 44: a1 = PRG base of the switchable window (bank*16384 added) */
    void    *tabwin;                    /* 48: a3 = window dispatch table, biased by -0x8000*4 */
    void    *tabfix;                    /* 52: a4 = fixed dispatch table, biased by -0xC000*4 */
    uint8_t *nztab;                     /* 56: a5 */
    uint8_t *fixbase;                   /* 60: a6 = fixed PRG base - 0x4000 */
    long     frames_target;             /* 64 */
} XCtx;
extern XCtx xrt_ctx;
extern uint32_t *xl_tab_win[7], *xl_tab_fix;
void xrt_run(long frames);               /* run the translated program until `frames` frames are done */
#endif
