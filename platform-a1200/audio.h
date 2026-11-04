/* audio.h - Paula playback of the APU's PCM: one chunk (one NES frame of samples) per frame, played back to back from a ring of chunks through
 * hardware double buffering (AUDxLC/LEN reload) on a left and a right channel carrying identical data.  See docs/AMIGA.md. */
#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>
#include "../runtime/apu.h"
#define AUD_SLOTS 6          /* ring of chunks */
#define AUD_MAX_WORDS 256    /* words (2 samples each) per chunk: 237/238 on PAL, 240/241 on NTSC */
#define AUD_PRIME 3          /* chunks queued before playback starts */
#define AUD_SILENCE 0x80     /* 8-bit signed value of a silent APU (apu_render8: (0*83>>10)-128) */
void     audio_init(void);                       /* after plat_init */
/* the next free chunk (AUD_MAX_WORDS*2 samples) or 0 if the ring is full */
int8_t  *audio_alloc(void);
void     audio_commit(unsigned words);           /* the chunk obtained from audio_alloc() holds `words` words (samples = 2 * words) */
unsigned audio_words_next(void);                 /* words (1 word = 2 samples) to produce for the next frame: 237/238 (PAL) or 240/241 (NTSC) from a Bresenham accumulator keeping sample production in step with the game clock */
unsigned audio_free_slots(void);
extern volatile uint32_t audio_underruns;        /* chunk boundaries where no chunk was ready (a short silence was queued) */
extern volatile uint32_t audio_chunks_played;
extern uint16_t audio_period;
/* Cue mode (default; -DAUDIO_EXACT selects the PCM path above): the four Paula channels loop small waveforms whose period / volume / waveform are set once per NES frame
 * from the APU state (runtime/apu.c apu_cue_frame).  NES pulse 1 -> Paula 0 (left), pulse 2 -> 1 (right), noise -> 2 (right), triangle -> 3 (left). */
void audio_cue_init(void);                       /* after plat_init; starts the four channels silent */
void audio_cue_apply(const ApuCue *c);           /* program period / volume / waveform of the four channels (only registers that changed are written) */
#endif
