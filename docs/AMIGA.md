# Little League Baseball: Championship Series on the Amiga 1200

Target: A1200 (68EC020 14.19/14.32 MHz, AGA, 2 MB chip RAM, **no fast RAM**), Kickstart 3.1 (only used by the bootblock to read the disk), PAL and NTSC.
Tested in FS-UAE 3.1.66 only (A1200 model, KS 3.1 40.68, chip 2048, slow/fast 0, headless under Xvfb). Nothing was run on real hardware.

**Status in one paragraph.** The whole chain works and is verified pixel for pixel against the reference emulator (boot from the ADF, translated 6502 code, PPU/APU/mapper runtime,
AGA display, game clock, keyboard/joysticks, Paula audio). **The speed is far below real time: 0.4 to 0.6 % of an NES** (about 0.3 frames per second, measured with the real A1200 timing of FS-UAE, section 9).
85 to 95 % of the time is the PPU model (line renderer, per-dot stepping, register synchronisation), the translated 6502 code itself costs 19 to 36 ms per NES frame (more than the 16.6 ms frame period),
the APU renderer 40 to 64 ms per frame and a full chunky-to-planar conversion 70 ms. See section 10 for what has to change. Everything else (pacing, display skipping, audio continuity) is in place and tested,
and will matter once the emulation gets close to real time.

## 1. Build

```
tools/build_adf.sh <rom.nes> <out.adf>
```

`<rom.nes>` is read, checked (iNES, mapper 1, 128 KB PRG, 128 KB CHR, no trainer) and copied into the disk image; the ROM and the ADF are never committed (`.gitignore`). The script runs the translator
(`translator/x65.py`), converts its dispatch tables to a sparse form (`tools/gen_tabsp.py`), generates the colour table (`tools/gen_palette.py`), compiles the runtime with `-DPPU_FB8 -DRT_AUDIO=1`
(freestanding `-m68020 -ffreestanding -nostdlib`, libgcc for the 64-bit divisions), links `platform-a1200/link.ld` and builds the ADF with `tools/mkdisk.py` (bootblock + payload).
`tools/build_xrt.sh` (qemu-m68k host build) is unchanged in behaviour: 1000 frames and the full 6000 frames are IDENTICAL to the Mesen reference trace after all Amiga changes
(`CMP_NOX=1 CMP_NO2002=1 TRACECMP=p8.trc`: 348,449 and 2,556,952 records).

Test builds (environment variables of `build_adf.sh`): `EXTRA="-DTELEM"` (counters drawn into the bottom picture lines), `EXTRA="-DPROFILE -DPLO=a -DPHI=b"` (PC-sampling profile page),
`EXTRA="-DSTOP_AT=N"` (show frame N and stop), `TESTINPUT=script.txt` (controller driven by a frame script), `SNAPSHOT=file.snap` (start from a runtime snapshot made by `tools/build_snap.sh`),
`AUDIO_TEST=file.pcm`, `DISPLAY_TEST=frames.bin`, `BUILD=name` (build directory). None of them is in the normal build.

## 2. Files

| path | purpose |
|---|---|
| `platform-a1200/boot.s` | bootblock (<= 1024 bytes), see section 4 |
| `platform-a1200/crt0.s`, `isr.s` | start-up (VBR 0, caches, BSS, vectors), interrupt stubs, crash colours |
| `platform-a1200/link.ld`, `rom.s` | memory layout, 256 KB ROM placeholder |
| `platform-a1200/platform.c/.h` | chips: reset, AGA display, Copper, game clock (CIA-B), keyboard, joysticks, E-clock counter |
| `platform-a1200/display.c/.h`, `c2p.s` | chunky frame -> 8 interleaved bitplanes, two buffers, change detection per 16 pixels |
| `platform-a1200/audio.c/.h`, `runtime/audio_words.h` | Paula output |
| `platform-a1200/main.c` | start-up, frame loop (pacing, display, audio), controller mapping |
| `platform-a1200/tabs.c`, `tools/gen_tabsp.py` | dispatch tables: sparse on disk, 8 x 64 KB in RAM |
| `platform-a1200/libc.c` | memset/memcpy/memmove/memcmp |
| `platform-a1200/telem.c/.h` | test instrumentation (only with -DTELEM / -DPROFILE) |
| `platform-a1200/test/*` | FS-UAE configs and run script, telemetry / profile decoders |
| `tests/amiga/*` | comparison tools and the test driver `run_tests.sh` |
| `tools/snap.c`, `build_snap.sh`, `tools/rt_host.c` | snapshots for test builds, reference frames (`FBDUMP`, `FBFRAMES`), reference PCM (`PCMOUT`) |

Changes to shared runtime files: `runtime/rt_x.c` (`#ifdef AMIGA`: no libc, debug code compiled out, illegal opcode -> `rt_fatal`), `runtime/ppu_ref.c` (the PPU_FB8 edit in `render_line_fast` used the already shifted
emphasis value: fixed, both variants identical, see section 8), `tools/rt_host.c`, `tools/build_xrt.sh` (extra flags / output directory).

## 3. Memory map (chip RAM, production build; all addresses from the link map)

| address | size | content |
|---|---|---|
| 0x000000 | 2 KB | exception vectors (copied by crt0), bootblock relocator at 0x200, boot stack 0x400 |
| 0x000800 | 252,408 | translated 6502 code (16,536 instructions in 3206 blocks; **larger than the 190 KB assumed**: the translator has grown) |
| 0x03E24A | 34,326 | text of `rt_x.s`, `rt_x.c`, `rt_core.c`, `ppu_ref.c`, `cpu6502.c`, `mapper_mmc1.c`, `apu.c`, c2p, display, main, audio, platform, libgcc |
| 0x046860 | 28,000 | read-only data: APU mixer tables, sparse dispatch table (3206 entries x 8 bytes at 0x047174), palette |
| 0x04D5C0 | 262,144 | **ROM: PRG 128 KB then CHR 128 KB**, read from the disk by the bootblock together with the code |
| 0x08D5C0 | 933,540 | BSS (cleared by crt0): `rt_apu` 80,090 (queues SQ 512 / TRI 4096 / NOI 8192), `rt`, `nes` 66,358 (PPU incl. the 60 KB framebuffer, 1 byte per pixel), APU LFSR tables 131,068 (0x0B12FA), display shadows 2 x 61,440 (0x0D1300), audio ring 6 x 512, **dispatch tables 8 x 65,536 (0x0F1464 - 0x171464)** |
| 0x171464 | 16 KB | supervisor stack (also used by the interrupt handlers) |
| 0x175470 | 2 x 61,440 | the two bitplane buffers (8 interleaved planes, 256 x 240) |
| 0x193470 | 445,328 | **free** up to 0x200000 (the build fails if the image grows past 0x1FF000) |

Everything fits: 1,651,824 of 2,097,152 bytes are used (79 %), 435 KB are left. **No compaction is needed.** The dispatch tables are the biggest single item (512 KB) and 99.98 % of their entries are the slow-path
address; on the disk they are stored sparsely (26 KB) and expanded at boot (`tabs_expand`, about 135,000 longword writes). If memory becomes tight later: 16-bit offsets relative to the code base
(both the table and `jmp ([a3,d7.l*4])` in `rt_x.s` would change) would halve them, a page-sparse two-level table would shrink them to about 40 KB at the cost of one more memory access per dispatch.

Disk: payload 576,960 bytes (the 880 KB disk holds 900,096): code and runtime 287 KB + read-only data 28 KB + ROM 256 KB; the ADF is a build product.

## 4. Boot

The ADF is an OFS-signature disk with a bootblock (286 bytes, checksummed by `tools/mkadf.py`; `tools/mkdisk.py` patches the payload length into it).
KS 3.1 loads the bootblock and enters it with A6 = ExecBase, A1 = trackdisk request. It
1. refuses 68040/68060 (cyan screen), 2. `AllocMem`s a chip RAM staging buffer of the payload size (magenta if that fails), 3. reads the payload (ADF bytes 1024..) with one `CMD_READ`,
red screen on error, 4. switches the motor off, 5. enters supervisor mode, masks everything, waits for the Blitter, stops all DMA, clears the caches, and
6. copies the payload to 0x800 with a relocator running at 0x200 (the OS is never used again) and jumps to `_start`.
`crt0.s`: `VBR = 0`, I-cache cleared and enabled (data cache off: custom chips must not be cached), stack, `plat_hw_reset` (all chips quiet), BSS cleared, vectors installed,
`plat_init` (AGA check - solid red if not AGA - PAL/NTSC detection by counting beam lines, display, Copper, CIA timers, keyboard, DMA and interrupts on), `main`.
`main`: palette and buffers, dispatch tables, audio, `rt_init(PRG, CHR)` (PRG/CHR are the ROM section in place), `xrt_run`.

## 5. Display

AGA, 256 x 240, lores, **8 bitplanes = 256 colour registers**, exactly as the old A1200 port: BPLCON0 0x0211, FMODE 1 (32-bit fetch), DDFSTRT/STOP 0x40/0xB0, DIWSTRT/STOP horizontal 0xA1 / 0x1A1,
border blanked (BRDRBLNK), planes interleaved (line y of plane p at `base + y*256 + p*32`, BPLxMOD 224). The bitplane pointers are in the Copper list and reloaded once per frame at line 14.

* **PAL**: display lines 52..291 (240 lines centred in the 256-line standard window).
* **NTSC**: display lines 21..260 on a 262-line frame. 21 is the first line the beam can display, so all **240 lines fit**; none of the 240 NES lines is cut by the Amiga (the FS-UAE screenshot of a stopped frame equals the reference
  frame in all 61,440 pixels, test `scenes ntsc`). The bottom NES line is Amiga line 260 of 262; on a real NTSC monitor the overscan of the tube decides what is visible (not testable here).
* **Colours**: pixel value v = NES colour (bits 0..5) | red emphasis << 6 | green emphasis << 7; the colour register v holds the RGB of that pixel. The 64 base colours are the default palette of the Mesen libretro core
  and the emphasis is Mesen's `GenerateFullColorPalette` (the other two guns x 0.84 per emphasis bit, columns $xE/$xF unaffected): `tools/gen_palette.py`. **Blue emphasis (bit 2 of the PPU's 3 bits) is not displayed**
  (8 planes only; the game never sets it: emphasis seen in 6000 frames is 0 and green only, 55 frames). Verified against Mesen's own frames: 3000 frames, 183.9 million pixels, 2 mismatches (frame 2451, pixels (78,36),(79,36):
  Mesen outputs an RGB that is not in its own 512-entry table, 0x0A9300 instead of 0x0C9300 for colour $1A; the cause is in Mesen's output, not in the table).
  The palette is static: it is loaded once by the vertical-blank interrupt and never touched again.
* **Conversion**: the PPU writes one byte per pixel into `nes.ppu.fb` (PPU_FB8). `display_frame()` (called from the frame hook) compares every aligned group of 16 pixels with a chunky *shadow* of what the target
  buffer shows and converts only the changed groups with the 82-instruction c2p kernel (a fast path for 16 pixels of one colour), then asks the vertical-blank interrupt to flip buffers (the Copper pointers are
  written in the interrupt, before line 14: no tearing, no half-written pointers). Two buffers, two shadows. `c2p_frame_all` (no comparison) is used after the telemetry overwrote picture lines.
  The first version converts what changed relative to the buffer's own shadow (frame n-2), not relative to the previous frame; static scenes cost only the compare pass (section 9).

## 6. Timing and pacing

* **Game clock**: CIA-B timer A, one tick per NES frame = 29780.5 CPU cycles of 1,789,772.7 Hz = 60.0988 Hz. Period in E-clock cycles: PAL 11803 + 21423809/39375000 (11803.5441), NTSC 11912 + 7815439/39375000
  (11912.1985), derived from 3931026/236250000 s per frame and the colour clocks 3546895 / 3579545 Hz; the fractional part by a Bresenham reload in the interrupt (exact on average).
  CIA-B timer B is a free-running 16-bit E-clock counter used for time measurements (wraps every 92 ms), TOD counts beam lines.
* **Frame loop** (`on_frame`, the runtime's `frame_hook`, called at the start of line 240 when the picture is complete). Frame f is due at tick `base + f + 1`.
  1. If early: wait with `STOP` until the tick. If late: no wait (catch-up); at most `BACKLOG_MAX` = 8 ticks of lag are kept, more is dropped (the game then runs slower than real time but never accumulates an
     unbounded backlog).
  2. Picture: skipped while behind real time (at most `DISPLAY_SKIP_MAX` = 3 frames in a row, so the screen updates at least every 4th frame); otherwise `display_frame()`. If the previous picture has not been shown yet
     (vertical blank not reached) the picture is skipped: on **PAL** (50 Hz) that happens for 1 of 6 game frames: **499 of 600 frames shown (83.2 %, 5 of 6)**, on **NTSC** 598 of 600 (the 59.94 Hz display is 0.2 % slower than the game).
  3. Sound: render the samples of this frame (below); if the ring is full (the game is ahead of Paula) wait for a free slot.
* The game itself always runs every frame; skipping only concerns the picture. Input is read by the runtime when the game polls the controller (line 241 of every frame) through `rt.input_hook`.

## 7. Audio

`apu_render8()` (box filter, signed 8 bit, silence = -128) is called once per frame for the CPU cycles `[a_from, nes.ppu.dots/3 - 2)` of that frame (contiguous, never beyond the last APU register access).
Paula: channels **0 (left) and 1 (right)** play identical data, volume 64, the full 8-bit range, no fades, period **124** colour clocks (28,604 Hz PAL, 28,867 Hz NTSC; 124 is the shortest period with full video DMA).
Deviation from the request: not "0+3 / 1+2" as two ping-pong channel pairs. The hardware double buffering of one channel pair (AUDxLC/LEN hold the next buffer, Paula latches them when the current one ends)
is gap-free without any restart; a second pair would only add the problem of starting channels at exactly the right moment (DMA-off/on glitches). Channels 2 and 3 are unused.

* **Rate lock**: samples per frame = (period of the game clock in colour clocks) / (2 x Paula period) words, i.e. 237.97 words (PAL) / 240.16 (NTSC); `runtime/audio_words.h` produces 237 or 238 (PAL) / 240 or 241 (NTSC) words per frame
  from an exact 64-bit Bresenham accumulator: over any length the number of samples produced equals the number Paula consumes, derived from the same constants as the game clock (not from a free-running guess).
  The same code produces the reference PCM on the host (`PCMOUT`).
* **Buffering**: a ring of 6 chunks of one frame each. Playback starts when 3 chunks are queued (about 2 frames of latency). The channel-0 audio interrupt (level 4) fires when a buffer has started; its handler
  releases the finished chunk and points both channels at the next chunk. **If no chunk is ready** (the game is slower than real time) it queues 16 words (1.1 ms) of silence at the APU's silent level (-128) and
  looks again at the next interrupt: Paula never loops stale data, the sound stays continuous in time (clean gaps, no buzz), audio and input never wait for the emulation. When the game is far slower than real time the
  result is a granular, staccato sound; the game's own tempo is not stretched (a variable period per chunk would be needed for that and was not done).
* The 8-bit unipolar output has a DC offset (silence plays at -128 x 64 volume); the Amiga's analogue stage is AC coupled, there is one step at start. No high-pass was added (the runtime's PCM is played unmodified).
* Measured cost of `apu_render8` on the A1200 model: 40 to 64 ms per frame (section 9) - more than 2 frame periods at real speed.

## 8. Controls

| Amiga | NES |
|---|---|
| joystick port 2 (JOY1DAT): directions | player 1 D-pad |
| port 2 fire (CIA-A PRA bit 7) | A |
| port 2 second button (POTGOR bit 14) | B |
| joystick port 1 (JOY0DAT), fire (PRA bit 6), second button (POTGOR bit 10) | player 2: D-pad, A, B |
| Return or keypad Enter | Start (both players) |
| Space | Select (both players) |

`POTGO` is initialised to 0xFF00 as in the old port (proven in FS-UAE; **real hardware untested**). FS-UAE: `joystick_port_1 = keyboard` gives the cursor keys, Right Ctrl = fire, `keyboard_key_x = action_joy_1_2nd_button`.

## 9. Tests and measured results

All numbers below were produced in this session with FS-UAE 3.1.66, A1200, KS 3.1 40.68, chip 2 MB, no fast RAM, `video_sync = off`. `tests/amiga/run_tests.sh <rom> <workdir> [pal|ntsc|both]` repeats the functional tests
(it needs `KICK=` a Kickstart 3.1 A1200 ROM, test only). Functional tests use `FAST=1` (FS-UAE `accuracy = 0`, `uae_cpu_speed = max`: the emulated CPU is as fast as the host, **not** A1200 timing; the program is deterministic and input
is frame driven, so the results do not depend on the speed). Speed and profile measurements use the default A1200 timing.

### 9a. Pixel exactness (screenshot = reference frame)

FS-UAE native full-frame screenshot (F12+S, 1 lores pixel = 2 x 2 screenshot pixels, picture window at (138, 52) PAL / (138, 0) NTSC), compared with `FBDUMP` frames of the reference runtime
(`tests/amiga/cmp_shot.py`; expected RGB = `nes_rgb[value & 0xFF]`). Build with `-DSTOP_AT=N` (frame N shown, game stopped), input from `tools/discovery/inputs/play_watchgame.txt` compiled in.

| frame | scene | PAL differing pixels | NTSC differing pixels |
|---|---|---|---|
| 100 | 50th anniversary logo | 0 of 61,440 | 0 |
| 300 | title screen | 0 | 0 |
| 400 | team selection map | 0 | 0 |
| 1200 | line-up list | 0 | 0 |
| 1600 | field, batting view (sprites, sprite-0 split) | 0 | 0 |
| 3000 | field, scrolling | 0 | 0 |
| 5000 | field | 0 | 0 |

(All 14 screenshots from one run of `tests/amiga/run_tests.sh`; the 2 x 2 pixel blocks of every screenshot are uniform, i.e. exactly 256 x 240 lores pixels.) PPU_FB8 against the 16-bit framebuffer: 6000 frames of the translated runtime, low byte identical in all frames
(55 of them with emphasis bits set).

### 9b. Real keyboard / joystick path (xdotool into FS-UAE)

Boot, wait for the title, then Return (Start), Down, Right Ctrl (A), Down, Down, Right Ctrl: the five screenshots are pixel-identical to reference frames 45 (Championship/Exhibition/Power Analysis menu),
151 (cursor on Exhibition Mode), 186 (1P/2P/Watch Game), 301 (cursor on Watch Game) and 345 (team selection map): 0 differing pixels each (`tests/amiga/match_ref.py` finds the closest reference frame).
Both joystick ports and keys read through the telemetry (`input` words of the HUD, every bit seen): joystick port 2 (keyboard emulation `joystick_port_1`): Up, Down, Left, Right, A, B, Select, Start = 0xFF for player 1;
joystick port 1 (`joystick_port_0 = keyboard`): 0xFF for player 2 including the second button (POTGOR bit 10); the keys Return/Space reach both players.

### 9c. Audio

`AUDIO_TEST` builds replay the 420 chunks (PAL: 199,898 samples, 237/238 words; NTSC: 201,738 samples, 240/241 words) that the runtime produced for the first 420 frames of the play script through `audio.c` at the game clock
(no emulation); FS-UAE's output is recorded with the OpenAL-soft wave backend at Paula's rate (`audio_frequency = 28604` / `28867`, so no resampling) and compared with the reference PCM
(`tests/amiga/audio_cmp.py`: aligned per 4096 samples, 3-tap smoothed). **420 of 420 chunks played, 0 underruns** during the replay on PAL and NTSC. PAL: median correlation 0.985 (5th percentile 0.948, min 0.928); NTSC: 0.938 (0.871, 0.838);
drift of the recording against the nominal rate -5.7 ms (PAL) and -15.2 ms (NTSC) over 6.8 s, i.e. FS-UAE's Paula output clock is 0.08 % / 0.22 % off its own CIA clock (the old port saw +-10 ms over 4 s too); gain 111 / 105 per
sample unit, silence -128 plays at -14433 / -13998. The NTSC correlation is lower because of the larger drift and FS-UAE's resampling of edges, not because of missing data (the chunk counters are exact). L and R differ by sub-sample interpolation (max |L-R| 5101 in 2554 of 203,088 samples), not by data (identical data on both channels).
What this shows: the DMA double buffering, the interrupt chaining, the rate lock and the amplitude chain work; the stream is the runtime's PCM. What it does not show: sound from the *running game* at real speed (the game
cannot reach real time, so there is no continuous game audio to record); in the speed runs below the audio interrupt delivers chunk after chunk with silence in between.

### 9d. Speed (frames executed per real second / 60.0988), real A1200 timing

Method: the build with `-DTELEM` draws counters (game clock ticks T, game frames F, ...) as black/grey pixel rows into picture lines 224-227; `platform-a1200/test/telem.py` reads them from FS-UAE screenshots (one every 30 s).
Speed = delta F / delta T (one tick is one NES frame period of the emulated machine, so the number does not depend on the host). Scenes start from runtime snapshots (`tools/build_snap.sh`, interpreter state after N frames of the play
script), because reaching them from reset would take hours.

| scene (snapshot at frame) | frames / ticks (PAL, 7 screenshots, 190 s) | speed | frames per second |
|---|---|---|---|
| menu (100: Championship/Exhibition menu) | 68 / 11,379 | **0.60 %** | 0.36 |
| line-up list (1200) | 56 / 11,788 | **0.48 %** | 0.29 |
| field, batting/fielding, busiest scene (2000) | 60 / 11,323 | **0.53 %** | 0.32 |

NTSC (field scene, snapshot 2000): 60 frames / 11,650 ticks = **0.52 %**, the same as PAL (the machine is CPU bound). Speed from reset (the first 24 frames, game start) was 0.3-0.5 % (per-dot PPU stepping while the game waits for vertical blank).
FAST mode (host-speed emulation) is not an A1200 measurement; it ran the scripted 5000 frames in about 2.5 minutes.

### 9e. Where the time goes (profile at real A1200 timing)

`-DPROFILE`: CIA-A timer A interrupts at 573 Hz (not tied to the game clock) record the interrupted PC in 32-byte bins, summed per link section (`link.ld` markers `_pr0..`) and the 44 hottest bins are drawn on the screen
(`platform-a1200/test/profread.py`). Window ticks 1500..9000 (125 s). ms per NES frame (real A1200 time; 100 % speed would need the sum to be below 16.6):

| component | menu (frames 108-151) | line-up (1208-1237) | field (2007-2045) |
|---|---|---|---|
| translated 6502 code | 18.9 | 35.6 | 26.8 |
| `rt_x.s` glue (dispatch, helper entry) | 2.8 | 13.9 | 16.8 |
| `rt_x.c` (I/O access helpers, cycle limit, wait-loop skipping) | 14.5 | 52.8 | 72.5 |
| `cpu6502.c` + mapper (slow path) | 1.9 | 2.4 | 9.1 |
| `rt_core.c` (time model, `rt_sync`, PPU register access, NMI logic) | 317 | 796 | 336 |
| `ppu_ref.c` (line renderer, per-dot model, `vram_read`) | 2469 | 3327 | 2753 |
| APU (`apu_render8` + register accesses) | 64.0 | 46.5 | 39.1 |
| display (c2p + compare) | 12.4 | 22.8 | 29.9 |
| main/audio/platform | 1.9 | 6.4 | 1.6 |
| **total per frame** | **2905** | **4310** | **3283** |
| **share of the PPU model** (ppu_ref + rt_core) | **96 %** | **96 %** | **94 %** |

Inside the PPU: `ppu_sync_fast` (which contains the inlined `render_line_fast`) 27-53 %, `vram_read` 10-17 %, `rt_sync` + `ppu_step` + `process_scanline` + `load_tile_info` + `pixel_color` (the per-dot model that runs between
register accesses and in vertical blank: 8,100 to 11,500 dots per frame, of 89,342) 17-45 %. The runtime renders about 236 lines per frame with the line renderer.
Direct measurements in the same runs: `apu_render8` 33-55 ms per frame; one picture conversion 70 ms when all 3,840 groups change (`DISPLAY_TEST`: max 49,916 E-clocks = 70 ms PAL, 76 ms NTSC),
**38 ms for the compare pass alone when nothing changed** (60 KB framebuffer + 60 KB shadow read from chip RAM at about 3 MB/s with the display DMA running).
I/O per frame (reference trace): 335 (menu) to 589 accesses per frame: 256 `$2004` writes (OAM clear) + 8 `$4016` + 32 `$4016/7` reads + mapper writes 25-246 + a few `$2005-$2007`.

### 9f. Display pacing logic (`DISPLAY_TEST_FLIP`, 600 game frames, no emulation)

PAL: 499 shown / 101 skipped (83.2 %: 5 of 6); NTSC: 598 / 2. With real conversions (`DISPLAY_TEST`) even two identical frames could not be shown every frame: the compare pass alone (38 ms) is longer than 2 frame periods,
PAL 171 and NTSC 180 of 600.

## 10. What has to change to approach real time (for the translator/runtime owner)

The numbers are unambiguous: the PPU model is 94-96 % of the time, but even without it the rest (119-194 ms per frame) would run at only 9-14 % of real time.
Per NES frame at the real A1200 timing (stock chip RAM, 256-byte I-cache only, 8-plane DMA): the translated code needs 19-36 ms, `rt_x.s`/`rt_x.c` 17-90 ms, the APU 40-64 ms, the display 12-30 ms
(more if the picture changes, 70 ms worst case), against a budget of 16.6 ms for everything.

1. **PPU** (needed: about 200 x): the PPU model costs about 10-12 ms per rendered line (tile fetches through `vram_read`, per-pixel BG/sprite loops, per-dot stepping between register accesses). Everything the NES PPU does here can be done in the NES's own
   2-bit planar tile format: render *into* planar bitplane memory (BG: copy 8-pixel tile rows, 5 planes = pattern bit 0, bit 1, attribute-palette bits 2-3, sprite flag; the 32 colour registers = the PPU palette RAM, so
   there is no 256-entry colour mapping and no chunky-to-planar step at all), sprites with the Blitter or AGA hardware sprites, scroll by BPLCON1/DDF + pointer arithmetic, mid-frame register changes (the
   sprite-0 split, the CHR bank switches around lines 60 and 95-115) by Copper lists or by re-rendering only the lines after the change. The per-dot model should only run while something observable (sprite-0 hit, vblank
   flag, `$2002` polls) can happen; the 2.4 M `$2002` reads of the game are mostly polling loops that can be skipped analytically as the two known ones already are.
2. **Chunky framebuffer and c2p are not viable** on a stock A1200 even at 100 % PPU speed: writing 60 KB, reading it back for the compare (38 ms) and converting (up to 70 ms) are each longer than a frame period.
   A planar renderer with dirty information from the renderer would avoid all of it.
3. **I/O helpers**: `rt_x.s` + `rt_x.c` cost 700-2,200 cycles per I/O access (17-90 ms for 335-589 accesses per frame), not counting the PPU synchronisation done inside (`rt_core`, 320-800 ms per frame): `rt_compute_limit_at`,
   64-bit divisions by 3 through libgcc `__udivdi3`/`__divdi3` in `sample_cycle_for_dots` and the limit arithmetic, `ppu_wr`/`ppu_rd` dispatch. The 256 `$2004` writes per frame (OAM clear before the DMA) and the 25-250 mapper writes per frame are the bulk: batching the `$2004` loop (the game does
   `LDA #$FF / STA $2004` x 256), a 32-bit cycle counter for the limit, and inline handling of the RAM-like registers would remove most of it.
4. **Translated code**: 19-36 ms per frame for 20-28 K NES cycles means 16 Amiga clocks per NES cycle; the tables (3206 entries) are reached through `jmp ([a3,d7.l*4])` (two chip RAM reads per dispatch) and the code
   is 252 KB for a 256-byte I-cache. Straight-line code with fewer dispatches (more `bra` between blocks, direct `jsr`/`rts` mapping onto the 68020 stack where the 6502 stack is not inspected) and fewer cycle-budget updates per instruction would help.
5. **APU**: `apu_render8` is 40-64 ms per frame (about 1,000 cycles per output sample on the 68020). 64-bit arithmetic in the event merge and the per-sample reciprocal multiplication should be replaced by 32-bit
   arithmetic (a frame has at most 30 K cycles), or the renderer should run at a quarter of the sample rate and duplicate samples.
6. Interface needs: `tab.s` is parsed by `tools/gen_tabsp.py` (labels `tab_win0..6`, `tab_fix`, `.long`/`.rept`/`.endr`); keep that format or tell me. The frame hook is used as specified.

## 11. Open problems / not verified

* Real hardware (boot, AGA fetch timing, POTGO configuration of the second button, keyboard, floppy) and A1200 CPU timing beyond what FS-UAE models.
* Speed: 0.4-0.6 %, see sections 9 and 10. The audio of the *running* game at real speed not recorded (only the replay of its PCM and the speed-run counters).
* Blue emphasis not displayed; the NES left-8-pixel clipping, grey mode etc. are the runtime's (identical to the reference in all frames tested).
* Audio: one DC step at the start of playback; no high-pass; channels 2/3 unused (see section 7).
* The two Mesen output pixels that differ from the generated palette (section 5) are unexplained.
* The functional tests run with a host-speed CPU (not A1200 timing); all of them were run on PAL and the scene and audio tests also on NTSC (`run_tests.sh both`: 14 of 14 scenes pixel-exact).
