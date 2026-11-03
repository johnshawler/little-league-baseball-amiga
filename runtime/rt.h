/* rt.h - target independent runtime of the translated NES program: time model, lazy PPU synchronisation, NMI edge logic, bus for the interpreter
 * (safety net / slow path) and the I/O helpers that translated code calls.  No Amiga hardware names in here. */
#ifndef RT_H
#define RT_H
#include <stdint.h>
#include "cpu6502.h"
#include "nes.h"
#include "apu.h"

#define RT_NEVER 0xFFFFFFFFFFFFFFFFull

typedef struct {
    Cpu6502  cpu;
    uint64_t cyc;                 /* cycle number of the most recent access (reference numbering) */
    uint64_t limit;               /* E: absolute cycle by which the fast path must stop (translated code keeps d5 = limit - now) */
    /* NMI edge logic (see rt_core.c) */
    struct { uint64_t sample; uint64_t key; uint8_t lvl; } q[8]; int nq;
    uint8_t  lvl_sampled; uint64_t need_cycle;
    int      need_halt, dma_on, dma_page;
    uint8_t  openbus;
    long     frames_done;
    void   (*frame_hook)(long frame);        /* called when a frame is complete (line 240) */
    uint8_t (*input_hook)(int port, long frame);   /* returns controller state (A,B,Sel,Start,U,D,L,R = bits 0..7) at the poll time */
    void   (*trace_hook)(char type, uint16_t a, uint8_t v, int32_t abs_);   /* optional: X/R/W records for host verification */
    int      op_fetch;
    uint8_t *ram;                 /* == nes.ram */
    int      ldirty;              /* the inputs of rt_compute_limit_at changed (NMI queue, DMA): the fast path recomputes the cycle limit after the next write */
} Rt;
extern Rt rt;
extern Apu rt_apu;           /* 2A03 APU, runtime/apu.c; advanced lazily by register accesses and apu_render*() */

/* x / 3 for 64-bit x without a libgcc division (2^32 = 3*0x55555555 + 1): used by the cycle <-> dot conversions on the hot paths */
static inline uint64_t rt_div3(uint64_t x){
    uint32_t hi=(uint32_t)(x>>32), lo=(uint32_t)x, qh=hi/3, r=hi-qh*3;
    uint32_t t=lo+r, ql=r*0x55555555u + (t<lo ? 0x55555555u+(t+1)/3 : t/3);
    return ((uint64_t)qh<<32)|ql;
}
void rt_init(const uint8_t *prg, const uint8_t *chr);
/* Interpreter-only execution (host verification of the lazy time model, and the slow path of the translated build). */
void rt_run_interp(long frames);
/* one instruction with exact bus timing and NMI check; returns -1 on an undocumented opcode */
int  rt_step(void);
/* absolute cycle limit for the fast path (translated code): recomputed after register writes that can change event times */
uint64_t rt_compute_limit(void);
uint64_t rt_compute_limit_at(uint64_t now);

/* fast-path helpers: access at absolute cycle c (reference numbering) */
uint8_t rt_io_rd(uint16_t addr, uint64_t c);
void    rt_io_wr(uint16_t addr, uint8_t v, uint64_t c);
void    ppu_sync(Nes *n, uint64_t dots);
void    ppu_sync_fast(Nes *n, uint64_t dots);
#endif
