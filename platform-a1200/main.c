/* main.c - Little League Baseball on the A1200: start-up, frame loop (pacing, display, audio), input.
 * The translated NES program and the runtime run on the machine; once per NES frame the runtime calls on_frame() (rt.frame_hook, at the start of
 * line 240 when the picture is complete).  See docs/AMIGA.md. */
#include <stdint.h>
#include "platform.h"
#include "display.h"
#include "audio.h"
#include "telem.h"
#include "../runtime/rt.h"
#include "../runtime/rt_x.h"

extern const uint8_t rom_start[];             /* PRG 128 KB then CHR 128 KB (linker symbol: the .rom section, patched into the disk image) */
extern void tabs_expand(void);

#ifndef BACKLOG_MAX
#define BACKLOG_MAX 8                  /* game clock ticks the game may be behind real time before the lag is dropped (catch-up burst limit) */
#endif
#ifndef DISPLAY_SKIP_MAX
#define DISPLAY_SKIP_MAX 3             /* consecutive frames whose picture conversion is skipped while behind real time */
#endif

void rt_fatal(int code)                /* illegal opcode in the slow path: paint the screen red/orange and stop */
{
    for (;;) plat_set_color(0, code == 3 ? 0x0F40 : 0x0F00);
}

/* ------------------------------------------------------------------ input */
#ifdef TESTINPUT      /* test builds: the controller is driven by a frame script (same file format and mapping as tools/rt_host.c: "frame hexmask", libretro bits) instead of the hardware */
static const struct { uint32_t f, m; } test_script[] = {
#include "testinput.inc"
};
static uint8_t input(int port, long frame)
{
    uint32_t m = 0;
    if (port) return 0;
    for (unsigned i = 0; i < sizeof test_script / sizeof test_script[0]; i++) { if (test_script[i].f <= (uint32_t)frame) m = test_script[i].m; else break; }
    uint8_t b = 0;
    if (m & 0x100) b |= 1; if (m & 1) b |= 2; if (m & 4) b |= 4; if (m & 8) b |= 8; if (m & 0x10) b |= 16; if (m & 0x20) b |= 32; if (m & 0x40) b |= 64; if (m & 0x80) b |= 128;
    return b;
}
#else
static uint8_t input(int port, long frame)
{
    (void)frame;
    uint16_t j = plat_joy_bits((unsigned)port);
    uint8_t b = 0;
    if (j & PIN_FIRE)  b |= 0x01;      /* A */
    if (j & PIN_BTN2)  b |= 0x02;      /* B */
    if (j & PIN_UP)    b |= 0x10;
    if (j & PIN_DOWN)  b |= 0x20;
    if (j & PIN_LEFT)  b |= 0x40;
    if (j & PIN_RIGHT) b |= 0x80;
    if (plat_key_down(KEY_SPACE)) b |= 0x04;                                    /* Select */
    if (plat_key_down(KEY_RETURN) || plat_key_down(KEY_ENTER)) b |= 0x08;       /* Start */
    return b;
}
#endif

/* ------------------------------------------------------------------ statistics (read back through the telemetry page) */
static uint32_t game_base;             /* game clock tick at which frame 0 is due */
static uint32_t st_idle, st_audio, st_disp, st_late, st_dskip, st_dconv, st_maxlag;
static uint8_t skip_run;
#ifdef TELEM
static uint32_t sticky;
#endif
static uint64_t a_from;

static inline uint32_t e_since(uint16_t t0) { return (uint16_t)(t0 - plat_eclock()); }

static long f0;                        /* frame index at start (non-zero when starting from a snapshot) */

static void on_frame(long f)
{
    uint16_t t0;
    uint32_t deadline = game_base + (uint32_t)(f - f0) + 1;
    int32_t lag = (int32_t)(plat_tick_count - deadline);

    /* 1. pace to the game clock; when late, run on (catch-up) up to BACKLOG_MAX ticks, then drop the lag */
    if (lag < 0) { t0 = plat_eclock(); plat_wait_ticks(deadline); st_idle += e_since(t0); lag = 0; }
    else if (lag > BACKLOG_MAX) { game_base += (uint32_t)(lag - BACKLOG_MAX); lag = BACKLOG_MAX; }
    if (lag > 0) st_late++;
    if ((uint32_t)lag > st_maxlag) st_maxlag = (uint32_t)lag;

    /* 2. picture: skipped while behind real time (at most DISPLAY_SKIP_MAX frames in a row) */
    if (lag > 0 && skip_run < DISPLAY_SKIP_MAX) { skip_run++; st_dskip++; }
    else {
        skip_run = 0; t0 = plat_eclock();
        if (display_frame(nes.ppu.fb)) st_dconv++;
        st_disp += e_since(t0);
#ifdef TELEM
        {
            uint32_t w[8 * 8] = { TELEM_MAGIC, plat_tick_count, (uint32_t)f + 1, plat_vbl_count, audio_underruns, audio_chunks_played, st_dconv, st_dskip,
                                  st_idle, st_audio, st_disp, st_late, st_maxlag, display_pairs_last, plat_is_pal, rt_apu.dropped,
                                  (uint32_t)input(0, 0) | ((uint32_t)input(1, 0) << 8), 0 };
            sticky |= w[16];     /* every controller bit seen so far (input test) */
            w[17] = sticky;
            telem_draw(plat_buf[display_back() ^ 1], w, 18, 224);
        }
#endif
    }

#ifdef STOP_AT      /* test builds: show the picture of frame STOP_AT (hook index, as FBDUMP of tools/rt_host.c) and stop */
    if (f == STOP_AT) {
        while (!display_frame(nes.ppu.fb)) plat_wait_vbl();
        for (;;) __asm__ volatile("stop #0x2000");
    }
#endif
    /* 3. sound: the samples of this frame; wait for a free slot if the ring is full (the game is ahead of Paula) */
    {
        int8_t *buf = audio_alloc();
        if (!buf) { t0 = plat_eclock(); do plat_wait_ticks(plat_tick_count + 1); while (!(buf = audio_alloc())); st_idle += e_since(t0); }
        t0 = plat_eclock();
        unsigned w = audio_words_next();
        uint64_t to = nes.ppu.dots / 3 - 2;
        apu_render8(&rt_apu, a_from, to, buf, 2 * w);
        a_from = to;
        audio_commit(w);
        st_audio += e_since(t0);
    }
#ifdef PROFILE
    {
        static uint8_t done;
        if (plat_tick_count >= PHI + 60 && !done) { done = 1; display_freeze(); telem_prof_report(plat_buf[display_back() ^ 1]); plat_show(display_back() ^ 1); }
    }
#endif
}

#ifdef SNAPSHOT      /* test builds: start from a runtime state captured by tools/snap.c instead of the reset state */
extern const uint8_t snap_data[];
static uint8_t snap_load(void)
{
    const uint32_t *h = (const uint32_t *)snap_data;
    if (h[0] != 0x534E5031u || h[1] != sizeof nes || h[2] != sizeof rt || h[3] != sizeof rt_apu) for (;;) plat_set_color(0, 0x0F0F);   /* layout mismatch: magenta */
    const uint8_t *p = snap_data + 20;
    const uint8_t *prg = nes.map.prg, *chr = nes.map.chr;
    void (*fh)(long) = rt.frame_hook; uint8_t (*ih)(int, long) = rt.input_hook;
    memcpy(&nes, p, sizeof nes); p += sizeof nes;
    nes.map.prg = prg; nes.map.chr = chr;
    memcpy(&rt, p, sizeof rt); p += sizeof rt;
    rt.frame_hook = fh; rt.input_hook = ih; rt.trace_hook = 0; rt.ram = nes.ram;
    memcpy(&rt_apu, p, sizeof rt_apu); p += sizeof rt_apu;
    a_from = ((uint64_t)((const uint32_t *)p)[0] << 32) | ((const uint32_t *)p)[1];
    f0 = rt.frames_done;
    return 1;
}
#endif
#ifdef TELEM
static void stage(uint32_t n) { uint32_t w[2] = { TELEM_MAGIC, 0x80000000u | n }; telem_draw(plat_buf[0], w, 2, 224); }
#else
#define stage(n) ((void)0)
#endif
#ifdef DISPLAY_TEST    /* display pacing test: two stored 256x240 frames are shown alternately, one per game clock tick, without emulation (how many pictures are converted on PAL/NTSC, cost of a full conversion) */
extern const uint8_t dtest_frames[];
void main(void)
{
    display_init();
    uint32_t base = plat_tick_count + 30, conv = 0, skipped = 0, e_conv = 0, e_max = 0, pairs = 0;
    for (unsigned k = 0; k < 600; k++) {
        plat_wait_ticks(++base);
        uint16_t t0 = plat_eclock();
#ifdef DISPLAY_TEST_FLIP     /* no conversion: only the buffer flip logic (which game frames can be shown on a 50 Hz display) */
        int r = 0; if (!plat_show_pending()) { plat_show(k & 1); r = 1; }
#else
        int r = display_frame(dtest_frames + (k & 1) * 61440);
#endif
        uint32_t e = e_since(t0);
        if (r) { conv++; e_conv += e; pairs += display_pairs_last; if (e > e_max) e_max = e; } else skipped++;
    }
    uint32_t w[16] = { TELEM_MAGIC, plat_tick_count, 600, plat_vbl_count, conv, skipped, e_conv, e_max, pairs, plat_is_pal, plat_lines };
    telem_draw(plat_buf[0], w, 16, 224); telem_draw(plat_buf[1], w, 16, 224);
    for (;;) __asm__ volatile("stop #0x2000");
}
#elif defined(AUDIO_TEST)      /* audio chain test: replay the PCM chunks of a host run (tools/rt_host.c PCMOUT) through audio.c at the game clock, no emulation */
extern const uint8_t audio_test_pcm[], audio_test_pcm_end[];
void main(void)
{
    display_init();
    audio_init();
    uint32_t base = plat_tick_count + 30;      /* half a second of silence first */
    plat_wait_ticks(base);
    for (const uint8_t *p = audio_test_pcm; p < audio_test_pcm_end; ) {
        unsigned w = (p[0] << 8) | p[1];
        int8_t *buf;
        plat_wait_ticks(++base);
        while (!(buf = audio_alloc())) plat_wait_ticks(plat_tick_count + 1);
        for (unsigned i = 0; i < 2 * w; i++) buf[i] = (int8_t)p[2 + i];
        audio_commit(w);
        p += 2 + 2 * w;
    }
#ifdef TELEM
    {   uint32_t u_end = audio_underruns;      /* underruns during the replay (the 3 chunks still queued cannot have caused any) */
        plat_wait_ticks(base + 60);
        uint32_t w[16] = { TELEM_MAGIC, plat_tick_count, 0, plat_vbl_count, u_end, audio_chunks_played, 0 }; telem_draw(plat_buf[0], w, 16, 224); }
#endif
    for (;;) __asm__ volatile("stop #0x2000");
}
#else
void main(void)
{
    display_init();
    stage(1);
    tabs_expand();
    stage(2);
    audio_init();
    stage(3);
    rt_init(rom_start, rom_start + 131072);
    stage(4);
    rt.input_hook = input;
    rt.frame_hook = on_frame;
    a_from = 0;
#ifdef SNAPSHOT
    snap_load();
#endif
    game_base = plat_tick_count;
    xrt_run(0x7FFFFFFF);
    for (;;) { }
}
#endif
