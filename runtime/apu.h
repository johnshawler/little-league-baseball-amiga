/* apu.h - Ricoh 2A03 APU model (NTSC), lazy / event driven, integer only.  Target independent (no stdio, no OS, no FPU).
 *
 * Two layers, both exact against the reference emulator (Mesen, see docs/APU.md):
 *   1. CPU-observable state: frame counter (4/5 step, IRQ flag), length counters, $4015 status.  Advanced lazily:
 *      apu_run_to() jumps from frame-counter event to frame-counter event, it never steps cycle by cycle (except the 1-3
 *      cycles after a $4017 write or a length-counter write, where the hardware has one-cycle ordering effects).
 *   2. Audio: the five channel outputs.  Every channel is caught up in closed form (timer skip-ahead) whenever one of its
 *      parameters is about to change, and the resulting level CHANGES are queued as (cycle, level) events.
 *      apu_render() integrates the non-linear mixer exactly over each output-sample interval from those events.
 *
 * Time: every cycle argument is the CPU cycle count as seen by the bus access (nes.cyc inside the cycle of the access,
 * i.e. the number that StartCpuCycle just incremented; the 8 reset cycles are cycles 0..7, the first opcode fetch is cycle 8).
 * Calls must use non-decreasing cycle values. */
#ifndef APU_H
#define APU_H
#include <stdint.h>

#define APU_CPU_HZ 1789773u

/* Event queue capacities (any size >= 1).  When a queue is full further events of that channel are dropped and counted in
 * Apu.dropped (render often enough, e.g. once per video frame, and this never happens: see docs/APU.md for the measured maxima).
 * Each queue is a linear array (no wrap-around): consumed events are discarded when a render call has used them, events that lie beyond the render
 * window are moved to the front. */
#ifndef APU_Q_SQ
#define APU_Q_SQ  4096
#endif
#ifndef APU_Q_TRI
#define APU_Q_TRI 16384
#endif
#ifndef APU_Q_NOI
#define APU_Q_NOI 8192
#endif

/* Queued event, one 32 bit word: (t << 8) | field, t = bus cycle (low 24 bits) from which the channel has the new level, field = the level in the position it has in the
 * mixer table index: pulse 1 and triangle: level << 4, pulse 2 and noise: level.  (see apu.c; apu_pop_event() gives the plain cycle and level) */

typedef struct { uint8_t len, halt, newhalt, enabled, reload, prev; } ApuLen;              /* length counter incl. the delayed reload of Mesen */
typedef struct { uint8_t constvol, vol, start, counter; int8_t divider; } ApuEnv;          /* envelope */
typedef struct {
    ApuLen l; ApuEnv e;
    uint8_t duty, dutypos, last;
    uint8_t sw_enable, sw_period, sw_negate, sw_shift, sw_reload, sw_divider, ch1;
    uint16_t real_period; uint32_t period, timer, sw_target;   /* period = timer reload value in CPU cycles - 1 */
    uint64_t sync;
} ApuPulse;
typedef struct {
    ApuLen l; uint8_t lin, lin_reload, lin_flag, lin_ctrl, seq, last;
    uint32_t period, timer; uint64_t sync;
} ApuTri;
typedef struct {
    ApuLen l; ApuEnv e; uint8_t mode, last; uint16_t sr;
    uint32_t period, timer; uint64_t sync;
} ApuNoise;

typedef struct {
    ApuPulse sq[2]; ApuTri tri; ApuNoise noi;
    /* frame counter */
    int32_t  fc_prev; uint8_t fc_step, fc_mode, fc_inhibit, fc_block; int16_t fc_newval; int8_t fc_wdelay;
    uint8_t  irq;                 /* frame IRQ flag ($4015 bit 6) */
    uint8_t  pending;             /* a length-counter write is waiting for its reload cycle */
    uint8_t  audio;               /* 1: simulate the channel sequencers and queue audio events (default) */
    uint64_t cyc;                 /* internal clock = bus cycle + 1 (Mesen's APU cycle counter); everything is simulated up to here */
    /* audio events */
    uint32_t qh[4], qt[4];        /* head/tail index of every queue */
    uint32_t qsq0[APU_Q_SQ+1], qsq1[APU_Q_SQ+1], qtri[APU_Q_TRI+1], qnoi[APU_Q_NOI+1];   /* +1: end marker used by the renderer */
    uint32_t dropped;
    /* renderer */
    uint8_t  rsq, rtn;            /* mixer table indices (pulse1<<4|pulse2, tri<<4|noise) of the last event consumed */
    uint64_t rpos;                /* events before this bus cycle have been consumed */
} Apu;

void     apu_reset(Apu *a);                                   /* power-on state; the first call also builds the (integer only) LFSR tables */
void     apu_set_audio(Apu *a, int on);                       /* off: skip channel sequencers/event generation (status, IRQ and length counters are unaffected) */
void     apu_write(Apu *a, uint16_t addr, uint8_t v, uint64_t cyc);   /* $4000-$4013, $4015, $4017 (not $4014, $4016) */
uint8_t  apu_read_status(Apu *a, uint64_t cyc);               /* $4015 read; clears the frame IRQ flag.  Bit 5 (open bus) is NOT included: OR in (openbus & 0x20) */
void     apu_run_to(Apu *a, uint64_t cyc);                    /* advance the observable state to the given bus cycle */
void     apu_flush(Apu *a, uint64_t cyc);                     /* bring the audio events complete up to (excluding) bus cycle cyc */
int      apu_irq(const Apu *a);                               /* frame-counter IRQ line as of the last run position */
uint64_t apu_irq_cycle(const Apu *a);                         /* bus cycle at which the line will next be raised (UINT64_MAX: not before the next register access) */
/* Pop the earliest event with t < until (bus cycle); channel 0..3 = pulse1, pulse2, triangle, noise.  Returns 0 if none. */
int      apu_pop_event(Apu *a, uint64_t until, uint32_t *t, int *ch, int *lvl);
/* Mixer value (Mesen units, 0..3158) of four channel levels. */
unsigned apu_mix(int sq1, int sq2, int tri, int noi);
/* Render n output samples covering the bus cycles [from, to) (box filter, exact time weighted average of the non-linear mixer).
 * Consecutive calls must be contiguous (from == previous to).  The sample boundaries are from + i*(to-from)/n, rounded down to whole cycles.
 * out16: Mesen mixer units 0..3158 (unsigned DC-offset value, as produced by the mixer before Mesen's high-pass).
 * out8:  signed 8 bit, (mix*83>>10) - 128, i.e. full scale 0..255 mapped to -128..127. */
void     apu_render16(Apu *a, uint64_t from, uint64_t to, int16_t *out, unsigned n);
void     apu_render8(Apu *a, uint64_t from, uint64_t to, int8_t *out, unsigned n);
/* The same results by the straightforward algorithm (one pass over the four queues, 64 bit sample boundaries; any window < 2^22 cycles).  apu_render*() uses it itself for
 * windows of 65536 cycles or more; the tests compare the fast path against it. */
void     apu_render16_ref(Apu *a, uint64_t from, uint64_t to, int16_t *out, unsigned n);
void     apu_render8_ref(Apu *a, uint64_t from, uint64_t to, int8_t *out, unsigned n);

/* ---- cue mode (alternative to apu_render*): instead of mixing PCM, the state of the four channels at the end of a frame is turned into a few Paula parameters per
 * channel (docs/APU.md "Cue mode").  The APU can run with apu_set_audio(a,0) in this mode: no event is queued, only the register-level state is needed.
 * ch[0..3] = pulse 1, pulse 2, triangle, noise.  vol 0..64 (0 = silent), per = Paula period in colour clocks (>= 124), wave = index for apu_cue_wave(). */
typedef struct { uint16_t per; uint8_t vol, wave; } ApuCueCh;
typedef struct { ApuCueCh ch[4]; uint8_t hi; } ApuCue;     /* hi: bit n set = channel n is too high for Paula and was silenced */
typedef struct { const int8_t *data; uint16_t words; } ApuCueWave;      /* looping 8 bit waveform (words = length / 2) */
#define APU_CUE_NWAVE 26
void apu_cue_init(void);                                   /* builds the waveforms once (RAM: chip RAM on the Amiga); also builds the LFSR tables */
const ApuCueWave *apu_cue_wave(unsigned id);
void apu_cue_frame(Apu *a, uint64_t cyc, unsigned pal, ApuCue *out);   /* run the observable state to cyc and derive the cues (pal: Paula clock 3546895 / 3579545 Hz) */
#endif
