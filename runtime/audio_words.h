/* audio_words.h - how many samples to produce per NES frame so that production stays in step with Paula's consumption (target independent, used by the Amiga
 * audio module and by the host tools that produce the reference PCM).
 * The game clock ticks once per NES frame; one tick = tick_base + tick_frac/tick_den CPU-independent "E-clock" cycles = 5 * that many colour clocks.  Paula
 * consumes one 16-bit word (2 samples) every 2 * period colour clocks.  Words per frame W = 5 * (tick_base + frac/den) / (2 * period) is not an integer:
 * aw_next() returns floor((k+1) W) - floor(k W) for k = 0, 1, 2, ... (237 or 238 on PAL, 240 or 241 on NTSC at period 124), exact over any length. */
#ifndef AUDIO_WORDS_H
#define AUDIO_WORDS_H
#include <stdint.h>
typedef struct { uint32_t whole; uint64_t rem, den, acc; } AudioWords;
static inline void aw_init(AudioWords *a, uint32_t tick_base, uint32_t tick_frac, uint32_t tick_den, unsigned period)
{
    uint64_t num = 5ull * ((uint64_t)tick_base * tick_den + tick_frac);
    a->den = 2ull * period * tick_den;
    a->whole = (uint32_t)(num / a->den); a->rem = num % a->den; a->acc = 0;
}
static inline unsigned aw_next(AudioWords *a)
{
    unsigned w = a->whole;
    a->acc += a->rem;
    if (a->acc >= a->den) { a->acc -= a->den; w++; }
    return w;
}
#endif
