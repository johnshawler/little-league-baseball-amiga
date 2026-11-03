/* audio.c - Paula output (see audio.h).
 *
 * Hardware: channels 0 (left) and 1 (right) play the same 8-bit signed data, period 124 colour clocks (28604 Hz PAL, 28867 Hz NTSC; the shortest period with full
 * video DMA that the Paula DMA schedule sustains), volume 64.  Playback is gap-free through the DMA reload: the registers AUDxLC/LEN hold the NEXT buffer;
 * Paula latches them when the current one ends and raises the channel-0 audio interrupt, whose handler (level 4) points both channels at the following buffer.
 * The following buffer is the next chunk from the ring if the emulator has produced one, else a short silence (AUD_SILENCE = the level of a silent APU) so that
 * a late frame never makes Paula loop old data.  Chunk n holds the samples of NES frame n; their number per frame follows from the ratio of the game
 * clock (CIA E-clock ticks per NES frame) to the Paula period, so that on average exactly as many samples are produced as are played.
 */
#include "platform.h"
#include "audio.h"
#include "../runtime/audio_words.h"

#define R16(o)   (*(volatile uint16_t *)(0xDFF000u + (o)))
#define R32(o)   (*(volatile uint32_t *)(0xDFF000u + (o)))
#define DMACON    0x096
#define INTENA    0x09A
#define INTREQ    0x09C
#define ADKCON    0x09E
#define AUDLC(c)  (0x0A0 + ((c) << 4))
#define AUDLEN(c) (0x0A4 + ((c) << 4))
#define AUDPER(c) (0x0A6 + ((c) << 4))
#define AUDVOL(c) (0x0A8 + ((c) << 4))
#define SETBIT 0x8000
#define DMAEN  0x0200
#define INT_AUD0 0x0080
#define INT_AUD1 0x0100

#define SIL_WORDS 16
static int8_t chunk[AUD_SLOTS][AUD_MAX_WORDS * 2] __attribute__((aligned(4)));
static uint16_t chunk_words[AUD_SLOTS];
static int8_t silence[SIL_WORDS * 2] __attribute__((aligned(4)));

static volatile uint32_t q_head;         /* chunks produced (main) */
static volatile uint32_t q_next;         /* chunks handed to the hardware (interrupt) */
static volatile uint32_t q_tail;         /* chunks whose playback is over (interrupt) */
static volatile uint8_t started;
static uint8_t cur_ring, next_ring, have_next;
volatile uint32_t audio_underruns, audio_chunks_played;
uint16_t audio_period = 124;

static AudioWords aw;             /* words per frame (runtime/audio_words.h) */

static inline void set_buf(const void *p, unsigned words)
{
    R32(AUDLC(0)) = (uint32_t)p; R16(AUDLEN(0)) = (uint16_t)words;
    R32(AUDLC(1)) = (uint32_t)p; R16(AUDLEN(1)) = (uint16_t)words;
}

void plat_isr_audio_c(void)
{
    R16(INTREQ) = INT_AUD0 | INT_AUD1;
    if (have_next) {                              /* the buffer that was queued has just started: the one before it is over */
        if (cur_ring) { q_tail++; audio_chunks_played++; }
        cur_ring = next_ring;
    }
    if (q_head != q_next) {
        unsigned s = q_next % AUD_SLOTS; q_next++;
        set_buf(chunk[s], chunk_words[s]); next_ring = 1;
    } else {
        set_buf(silence, SIL_WORDS); next_ring = 0; audio_underruns++;
    }
    have_next = 1;
}

void audio_init(void)
{
    for (unsigned i = 0; i < SIL_WORDS * 2; i++) silence[i] = (int8_t)AUD_SILENCE;
    for (unsigned s = 0; s < AUD_SLOTS; s++) for (unsigned i = 0; i < AUD_MAX_WORDS * 2; i++) chunk[s][i] = (int8_t)AUD_SILENCE;
    audio_period = 124;
    aw_init(&aw, plat_tick_base, plat_tick_frac, PLAT_TICK_DEN, audio_period);
    q_head = q_next = q_tail = 0; started = 0; have_next = 0; cur_ring = next_ring = 0;
    R16(DMACON) = 0x0003; R16(INTENA) = INT_AUD0 | INT_AUD1; R16(INTREQ) = INT_AUD0 | INT_AUD1;
    R16(ADKCON) = 0x00FF;                         /* no audio modulation */
    for (unsigned c = 0; c < 2; c++) { R16(AUDVOL(c)) = 64; R16(AUDPER(c)) = audio_period; }
}

unsigned audio_words_next(void) { return aw_next(&aw); }

unsigned audio_free_slots(void) { return AUD_SLOTS - (q_head - q_tail); }

int8_t *audio_alloc(void)
{
    if (q_head - q_tail >= AUD_SLOTS) return 0;
    return chunk[q_head % AUD_SLOTS];
}

void audio_commit(unsigned words)
{
    unsigned s = q_head % AUD_SLOTS;
    chunk_words[s] = (uint16_t)words;
    __asm__ volatile("" ::: "memory");
    q_head++;
    if (!started && q_head - q_next >= AUD_PRIME) {
        started = 1;
        unsigned s0 = q_next % AUD_SLOTS; q_next++;
        cur_ring = 1; have_next = 0;
        set_buf(chunk[s0], chunk_words[s0]);
        R16(INTENA) = SETBIT | INT_AUD0;           /* the first interrupt (buffer loaded) queues the next one */
        R16(DMACON) = SETBIT | DMAEN | 0x0003;
    }
}
