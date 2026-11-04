# APU (2A03 sound) - design, verification, cost

Files: `runtime/apu.h`, `runtime/apu.c` (target independent, integer only, no libc; compiles with `m68k-linux-gnu-gcc -m68020 -ffreestanding -O2`),
hooked into `runtime/rt_core.c` (`$4015` read, `$4000-$4013/$4015/$4017` writes, `rt_apu`), tests in `tests/apu_test.c`,
verification tools `tools/apu_verify.c`, `tools/apu_blipcmp.cpp`, `tools/apu_audiocmp.py`, `tools/apu_gentab.c`, Mesen instrumentation `tools/discovery/mesen-apu.patch`.
The oracle is the Mesen source (libretro build, commit 0102910c); every quirk in `apu.c` is a port of the corresponding Mesen code.

## API (runtime/apu.h)

| call | meaning |
|---|---|
| `apu_reset(a)` | power-on state (`$4017` acts as if 0 was written 9-12 cycles before the first opcode, as Mesen), builds the 128 KB noise LFSR jump tables once |
| `apu_write(a, addr, v, cyc)` | `$4000-$4013`, `$4015`, `$4017`; `cyc` = bus cycle number of the write (rt.cyc / nesref `nes.cyc`) |
| `apu_read_status(a, cyc)` | `$4015` read (clears the frame IRQ flag). Bit 5 (open bus) is not included: `rt_core.c` ORs in `rt.openbus & 0x20` |
| `apu_run_to(a, cyc)` | advance the CPU-observable state (frame counter, IRQ flag, length counters) to a bus cycle |
| `apu_irq(a)`, `apu_irq_cycle(a)` | frame IRQ line as of the last run position; bus cycle at which it will next be raised (for a scheduler) |
| `apu_set_audio(a, on)` | `rt_core.c` leaves audio **off** unless compiled with `-DRT_AUDIO=1` (then no channel sequencer is simulated and no event is queued) |
| `apu_flush`, `apu_pop_event` | bring the audio events complete up to a cycle / pop (cycle, channel, level) events |
| `apu_render16/8(a, from, to, out, n)` | n mono samples covering the bus cycles `[from,to)`, contiguous calls; any integer rate (n/(to-from)); box filter |
| `apu_mix(sq1,sq2,tri,noi)` | Mesen mixer value (0..3158) |
| `apu_render16_ref/8_ref` | the same output by the straightforward one-pass algorithm (any window < 2^22 cycles); the fast path is tested against it |
| `apu_cue_init`, `apu_cue_frame(a,cyc,pal,&cue)`, `apu_cue_wave(id)` | cue mode (see below): per frame the four Paula channel parameters (period, volume, waveform) instead of PCM |

DMC: registers `$4010-$4013` are accepted and ignored, `$4015` bit 4 and the DMC IRQ (bit 7) always read 0. The game never enables it (all 20,004 `$4015`
writes in the 6000-frame run are `00 07 0b 0d 0e 0f`; `$4010-$4012` are never written, `$4013` 69 times) so this is exact for this game; DMC DMA stalls are not modelled.

## Structure

*Time.* Internally `a->cyc` = bus cycle + 1 (Mesen's APU cycle counter). The APU is *never* stepped per cycle: `apu_run_to` jumps from frame-counter step to step
(7457, 14913, 22371, 29828/29829/29830 cycles) and falls back to single cycles only while a `$4017` write or a length-counter write is pending (Mesen's
"reload after the frame counter, before the channel clocks of the next cycle" ordering, `len_reload_timing` fix) and for 2 cycles after a frame-counter tick (`_blockFrameCounterTick`).
Typical cost: 4 steps per video frame plus one per register write.

*Channels.* Each channel keeps `(timer, sync)`; `*_sync(target)` catches it up in closed form: the clocks in `(sync, target]` are at `sync+timer+1+j*(period+1)`, their number
and the remaining timer follow from one division; the sequencer position is advanced arithmetically (pulse: `dutypos -= k`, triangle: `seq += k` only while length and linear counter are non-zero,
noise: LFSR jump via position tables for long mode, `k mod L` single steps (L = 93 or 31) for short mode). A channel is synchronised **before every parameter change**
(its own register write to the write cycle; all channels to cycle T-1 before a frame-counter tick or a length reload at T, because the channel clock of cycle T already sees the new state),
so level changes are generated with the parameters that were valid at the time. When the output level can change only at the first clock (muted pulse, volume 0, noise with volume 0) no per-clock work is done.
Level *changes* are appended to per-channel event queues (cycle, level) - the same information Mesen hands to its mixer (`AddDelta`). Capacities `APU_Q_SQ/TRI/NOI` = 4096/16384/8192 events;
overflow drops events and counts them in `dropped`. Measured maximum per video frame in the 6000-frame run: pulse 93/179, triangle 1995, noise 3630 (so rendering once per frame needs only about a quarter of that; worst case noise period 4 is 7445 events per frame).

*Events.* A channel level change is one 32 bit word in a linear per-channel queue: `(t << 8) | field`, `t` = bus cycle (low 24 bits), `field` = the level in the position it has in the mixer
table index (pulse 1 and triangle `level << 4`, pulse 2 and noise `level`). Generation is 32 bit arithmetic (the 64 bit cycle counter is only kept for the frame counter), the sync loops append straight to the queue
(`sq_sync`/`tri_sync`/`noi_sync`), and with `apu_set_audio(a,0)` the channel sync is skipped entirely. Consumed events are discarded by the render call, events beyond the window move to the front.
`apu_pop_event` rebuilds the full 32 bit cycle from `a->cyc` (queued events are never older than 2^23 cycles).

*Rendering.* `apu_render*` flushes (`apu_run_to(to-1)` + sync of all channels), then (window < 65536 cycles, the normal case) works in two passes over *events*, not samples:
`merge()` turns the four queues into one time ordered list of 32 bit entries `(t << 16) | mixer value` (`sq2_mix[pulse1<<4|pulse2] + tnd_mix[tri<<4|noise]`, tables from `tools/apu_gentab.c`, Mesen's exact formulas, DMC = 0)
and `core()` walks the sample boundaries and integrates the entries. Sample boundaries `floor(i*span/n)` come from a Bresenham step in 32 bit fixed point (one add, the carry is the extra cycle; exact, see `tests`), a
sample without an entry is the single table value (no multiplication, no division), a mixed sample costs one 32x32->64 bit multiplication by the reciprocal `ceil(2^32/len)` (identical to the old formula, so the output is bit identical).
Windows of 65536 cycles or more, `n >= 65536` or fewer than 2 cycles per sample use the reference path (`render_slow`: the straightforward algorithm, 64 bit boundaries) which is also what `apu_render*_ref` call.
Output of `apu_render16` is the mixer value 0..3158 (DC offset included, like the mixer before Mesen's high-pass); `apu_render8` gives `(v*83>>10)-128`.
Footprint: 128 KB LFSR tables (static BSS), `Apu` struct 128 KB with the default queue sizes (4 bytes per event; 52 KB with the Amiga build's 512/4096/8192), 2 KB of mixer tables.

## Cue mode (Amiga: default, `-DAUDIO_EXACT` selects the exact PCM mixer)

Instead of mixing PCM the Amiga build can program Paula's four DMA channels once per NES frame from the APU state at the frame end (`apu_cue_frame`, `platform-a1200/audio.c audio_cue_apply`, hook in `main.c on_frame`).
The APU then runs with `apu_set_audio(a,0)` (no channel sequencer, no event queue: only frame counter, envelopes, length/linear counters and sweeps, which is all the cue needs).

| NES channel | Paula channel | waveform (looping, in RAM) | period (colour clocks) | volume 0..64 |
|---|---|---|---|---|
| pulse 1 | 0 (left) | duty 12.5/25/50/75 %: 8 samples, or 4 / 2 samples (average of 2 / 4 steps) when the pitch needs it | `(2*real_period+2) * steps * 1.98` | `sq_mix[env level]` scaled |
| pulse 2 | 1 (right) | same | same | same |
| noise | 2 (right) | long mode: first 4096 samples of the 32767 LFSR cycle (stride 1,2,4,8,16 in LFSR steps for the fast periods); short mode: 93 state cycle twice | `(noise period) * stride * 1.98` | `tnd_mix[level]` scaled |
| triangle | 3 (left) | 32 steps through the (non-linear) tnd table of the triangle alone, or 16 / 8 / 4 samples | `(timer+1) * steps * 1.98` | constant (amplitude is in the wave), 0 when length or linear counter is 0 |

Paula's period is at least 124 colour clocks (full video DMA), so a wave sample must last at least 62.6 CPU cycles: waveforms are halved (averaging steps) as often as needed, no NES pitch is out of reach
(pulse real_period >= 8 needs 2 samples per cycle, triangle timer >= 7, every noise period); `ApuCue.hi` would flag an unreachable pitch (never in the 6000 frame run). Volume scale: a wave of +-127 at volume 64
spans 254 counts of the 8 bit output, a channel's `mixer value * 83/1024` is its peak-to-peak, so `vol = mix*5312/260096`, using the single channel point of the real non-linear mixer curve (`ApuCueCh.vol` of
pulse 15 = 15, triangle = 25, noise 15 = 18).

*Approximations (what the exact mixer does that the cue mode does not):* the oscillators free run (no phase lock to the NES: the waveform correlation with the exact signal is ~0, the spectrum matches);
the non-linear mixer is a per channel gain (a 4-channel chord is louder/softer than the exact mix by the mixer compression: loudness error p5..p95 -2.4..+1.4 dB over 100 ms windows); envelope, length counter,
sweep and linear counter are sampled at the frame end (one frame = 16.6 ms latency, volume changes inside a frame are not heard, the envelope decay steps 4 times per frame are heard as one step per frame);
the pitch is the register value at frame end (a vibrato/slide inside a frame is flattened); period rounding to whole colour clocks (mean 0.6-1.2 cents, max 5.3 cents); the Paula D/A is signed 8 bit with a hard
left/right split (pulse 1 + triangle left, pulse 2 + noise right) because Paula has only four channels: the exact mixer's mono-identical output needs two channels per source; noise is a 4096 sample loop
(long mode repeats every 143 ms) and the fast noise periods use every 2nd..16th LFSR step (same statistics, different sequence); DMC absent (as in the exact mixer). Cue application is a few register writes, so it cannot
glitch: a waveform change is latched by Paula at the end of the current loop, the period at the next sample.

*Measured* (`tools/apu_cuesim.c` + `tools/apu_cuecmp.py`: 6000 frames of the game, the cue mode simulated sample by sample at Paula's 124 colour clock rate with the same one frame delay as on the machine, against `apu_render8`):
spectral magnitude correlation per 100 ms window 0-8 kHz mean 0.855 / median 0.90 (PAL and NTSC alike); loudness cue/exact median -0.03 dB (p5 -2.4, p95 +1.4); per frame RMS level error mean |.| 1.8 dB;
dominant spectral peak within 25 cents in 75.8 % of windows (within 100 cents 80.9 %; 8.5 % are an octave or more off, where the exact signal's strongest component is a harmonic of the cue's fundamental or the reverse);
pitch (period rounding): pulse 1 mean 0.72 / max 5.28 cents over 4733 frames, pulse 2 0.62 / 4.87 (4449), triangle 0.71 / 2.21 (1758), noise 1.24 / 2.29 (1406); channels silenced for being too high: 0 frames.
log-spectral distance < 8 kHz: mean 8.7 dB (random phase of the noise-like parts and the missing mixer non-linearity; the exact path itself differs from Mesen's band limited PCM by 1.8 dB).
This is a *listening-proxy* only: nobody listened to it.

## Verification (all numbers from runs in this session)

Reference: Mesen 6000-frame play run (`p8.trc`, `p8.pcm`) and 1500-frame attract run.

1. **CPU-observable behaviour, `$4015` reads and no change of CPU timing.** `gcc -O2 -Wno-unused -o /tmp/rt_host tools/rt_host.c runtime/rt_core.c runtime/cpu6502.c runtime/ppu_ref.c runtime/mapper_mmc1.c runtime/apu.c`,
   `CMP4015=1 TRACECMP=p8.trc rt_host rom 6000 p8.txt -`: **IDENTICAL over 58,288,181 records** (X, W, R of `$2002/$4016/$4017`, plus all **4,751 reads of `$4015` compared by value and cycle**; 58,283,430 without `$4015`), both with audio off and with `-DRT_AUDIO=1`
   (so the channel sequencers cannot influence anything the CPU sees). Attract: 1500 frames vs a *fresh* Mesen trace streamed through a FIFO (nothing written to disk): **IDENTICAL over 13,172,331 records** incl. `$4015`.
2. **Channel outputs cycle by cycle** (`tools/apu_verify.c` vs the `mesen-apu.patch` log of every `AddOutput` level change: cycle, channel, new level). Events at the same cycle are collapsed to the last level (zero width).
   6000-frame play run: pulse1 139,004, pulse2 127,767, triangle 578,317, noise 182,726 events = **1,027,814 level changes, IDENTICAL (cycle and level) over the whole run**, none dropped.
   Attract run: 19,745 + 12,885 + 41,500 + 70,399 = 144,529 events **IDENTICAL**. (The comparison excludes the last 40,000 cycles because Mesen's own lazily run channels have not caught up there; the totals are equal anyway.)
3. **Final audio, bit exact through Mesen's own path.** `tools/apu_blipcmp.cpp` feeds *our* events through the SoundMixer arithmetic (per-stamp `GetOutputVolume`, delta x MasterVolume 10.0 as set by the libretro core) and Mesen's `blip_buf.cpp`,
   with Mesen's block ends (9999-cycle blocks and every video frame): **4,792,040 samples, 0 differ, max abs diff 0** against `p8.pcm` (its two channels are identical: 0 samples with L != R). Mesen's own events through the same tool also give 0 differences, so the harness itself is exact.
   (Details that mattered: MasterVolume 10.0, `blip_clear` after `blip_set_rates`, `EndFrame` also at every video frame end.)
4. **Our box-filter renderer vs Mesen's band-limited PCM** (`tools/apu_audiocmp.py`, 48 kHz, mine x10 and passed through blip's high-pass): best lag 8 samples (= blip's kernel delay), 861 active 100 ms windows:
   normalised cross correlation mean 0.9983, median 0.9987, min 0.9912; relative RMS error mean 5.9 %, median 5.4 %, max 16.8 %; log-spectral distance below 8 kHz mean 1.8 dB / median 0.95 dB, over the full 0-24 kHz band mean 8.0 dB
   (the box filter aliases ultrasonic square-wave harmonics, blip does not). This is the expected difference between a box filter and a band-limited step synthesis, not an emulation error (3. shows the emulation is exact).
5. **Self check** `tests/apu_test.c` (`gcc -O2 -Wall -Wno-misleading-indentation -o /tmp/apu_test tests/apu_test.c runtime/apu.c && /tmp/apu_test`, 0.7 s): 588 checks, 0 failed: LFSR cycle structure; 150 random register-traffic seeds where the *lazy* APU (touched only at accesses) must
   give identical `$4015` values and identical raw event streams to an APU that is advanced and synchronised every single cycle; power-on frame IRQ timing (flag first visible to a read at bus cycle 29830, set again at 29831 and 29832, `apu_irq_cycle`), inhibit,
   length counter load/disable/halt/expiry, 5-step mode; `apu_render16` equals brute-force per-cycle integration for 20 random cases.

6. **Fast renderer vs the previous renderer, bit exact.** `apu_render8` PCM of the 6000 frame run (`tools/rt_host.c PCMOUT`, PAL and NTSC chunk sizes, PAL 2,867,696 and NTSC 2,893,982 bytes) is `cmp`-identical to the PCM of the previous
   implementation (commit 513587b `runtime/apu.c`), on the host and in the m68k build under qemu-m68k (`tools/build_xrt.sh` with the Amiga flags, `PCMOUT`), and the 6000 frame host trace comparison against the Mesen trace
   (`CMP4015=1 TRACECMP=p8.trc`) stays IDENTICAL over 58,288,181 records. `tests/apu_test.c` (588 checks, also run under qemu-m68k) adds: fast path == reference path over 80 random multi-window sequences (8/16 bit, 2-3 cycle samples,
   odd counts), render8 == (render16*83>>10)-128, cue pitch/volume checks.

## Cost on the A1200 (FS-UAE, chip RAM only, 14 MHz 68EC020, PAL, busy-music scene: snapshot at frame 2380, frames 2440-2553, E-clock timing of each entry point, `-DAPU_PROF`)

| ms per NES frame | previous implementation | fast renderer (C, 32 bit) | cue mode |
|---|---|---|---|
| `apu_render8` (flush = event generation + merge + integrate) | 28.1 | 9.9 | - |
| `apu_cue_frame` + Paula register writes | - | - | 0.76 (0.55 of it `apu_run_to`) |
| `apu_write` (register writes, 6.8 per frame) | 4.35 | 3.3 | 1.03 |
| `apu_read_status` | 1.65 | 1.35 | 0.44 |
| **APU total** | **34.1** | **14.5** | **2.2** |

(The previous implementation was measured on frames 2421-2553, the others on 2440-2553 by interpolation of screenshots of the telemetry page; the register-access costs in cue mode are lower because no channel sequencer
is synchronised. The PC sampling profiler of the same builds attributes 4.0 ms/frame (0.9 %) to all APU code in cue mode against 75.8 ms (6.8 %) before, with the same factor ~2 between its absolute numbers and the E-clock
measurements; use the E-clock values.) Everything runs from chip RAM here: a 14 MHz 68020 fetches every instruction word that is not in the 256 byte I-cache through the display-starved chip bus, so cost follows the number of
executed instructions, not their kind; with code and tables in 32 bit fast RAM (the planned 8 MB configuration) these numbers will drop a lot (not measured yet). The 2 ms target for `apu_render8` is not reached in chip RAM
(9.9 ms: the busy triangle frames at 2000 events/frame dominate; sparse frames cost about 3 ms); the unrelated lazy part (register writes and the frame counter jump) is 4.6 ms of the 14.5.

## Event queue fill (6000 frame run, flush once per video frame)

Maximum events waiting at a render call (one frame of generation): pulse 1 93, pulse 2 179, triangle 1995, noise 3630 (sum 3853; mean 171, frames above 1000 events: 221, busiest 40 frame stretch: 2009 per frame, all triangle).
Amiga build queues 512 / 4096 / 8192: margins 2.9x / 2.1x / 2.3x at 1:1 speed (the render is called every frame, so nothing accumulates). Theoretical worst cases per frame: noise period 4 = 7445 events (fits 8192), a triangle at timer 2 would
produce 9,900+ (does not fit 4096; the game never writes a timer value below 20). `Apu.dropped` counts any loss (0 in every run).

## Not verified / limits

* Features the game never uses are ported from Mesen but have **no reference trace**: writes to `$4017` (the game never writes it; only the lazy-vs-per-cycle test and hand-computed expectations), noise short mode (`$400E` bit 7; values written are 00-0b),
  the pulse-1 negate sweep (pulse 1 only ever gets `$4001` = 00, 12, 92; pulse 2 negate is used and matches), DMC (stub), PAL timing.
* The reference's frame counter IRQ is only observed through `$4015` (the I flag stays set in this game); `apu_irq()` has not been checked against an actual IRQ delivery.
* An event queue holds a bounded number of events: render (or `apu_flush` + `apu_pop_event`) at least once per video frame. Dropped events are counted in `Apu.dropped` (0 in all runs).
* Rendering is mono; Mesen's two output channels are identical for this game (no panning).
* `apu_render` assumes `to` is not beyond the CPU's current cycle (the APU would be run ahead of the last register access).

## Host cost (Xeon 2.1 GHz, gcc -O2)

`tools/apu_verify.c` with `PCMOUT=...`: the whole 6000-frame run (4,792,040 samples at 48 kHz, 1.03 M events) spends **0.073 s in `apu_render16` including all channel catch-up/event generation = 15.3 ns/sample (about 32 host cycles per output sample)**, 12 us per 60 Hz frame
(= 0.2 events per sample, so roughly 150 host cycles per event including closed-form catch-up, queue merge and mixer lookup). The lazy CPU-observable part (frame steps, writes, reads) is a few hundred instructions per video frame.
I have not measured the 68020 (only compiled it: no libgcc calls except the two 64-bit divisions per render call); the cost structure scales with *events* (a few thousand per frame), not with CPU cycles (29,780 per frame).
At 22 kHz output rate the cost is lower per frame (366 samples/frame, same events).
