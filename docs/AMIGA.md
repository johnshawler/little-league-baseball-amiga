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

## 12. Round 2: speed work (measured, real A1200 timing in FS-UAE)

Changes (all verified identical to the reference): 6000-frame host trace IDENTICAL (2,556,952 records) with all frames drawn and with only every 3rd / 5th frame drawn (`DRAWPAT`); all 6000 framebuffers
equal the previous renderer's (`tests/amiga` / FIFO comparison, 0 differing frames); reference PCM bit-identical; 14 of 14 scene screenshots (PAL+NTSC) pixel-exact; real-speed screenshots of the field scene
(drawn-frames-only path) equal reference frames 2052, 2080, 2108 (0 differing pixels outside the telemetry lines).

1. `nes.ppu.draw` (set by `on_frame` for the *next* frame: 0 while behind real time, at most `DISPLAY_SKIP_MAX` = 3 frames in a row): lines are not rendered, only their state effects are computed (sprite overflow, sprite-0 hit by the
   existing analytic `line_hit_cx`, from a per-line sprite coverage table rebuilt only after OAM writes). Vertical-blank lines 242..260 are skipped analytically (`ppu_skip_vbl`).
   Drawn lines: tile fetch through direct pointers, 4 pixels per long, sprites laid over the line once, and a 32-bit *signature of the line's inputs*: a line whose signature equals that of the line already in the framebuffer is not
   rendered again and is not marked changed (`nes.ppu.rowdirty`).
2. `rt_div3` replaces the 64-bit `/3` (libgcc) in the cycle/dot conversions, `/341` is 32-bit; OAM DMA from RAM runs as a block (cycle count identical: 148,929,788 cycles in 5001 frames before and after) when every write stores into OAM.
3. `apu_render`: next-event times of the four queues cached (bit-identical PCM). Not the 10x asked for (see below).
4. Display: `c2p_frame` converts only rows the renderer marked (per-buffer row flags) and, inside them, the 16-pixel groups that differ from the shadow; no full-frame compare any more (38 ms -> 0.6 to 20 ms).

| ms per NES frame | menu before | after | line-up before | after | field before | after |
|---|---|---|---|---|---|---|
| translated 6502 code (incl. translator changes of the coordinator) | 18.9 | 4.1 | 35.6 | 11.5 | 26.8 | 25.1 |
| rt_x.s + rt_x.c + cpu6502/mapper | 19.2 | 17.1 | 69.1 | 46.1 | 98.4 | 81.7 |
| rt_core (sync, PPU registers, NMI) | 317 | 98 | 796 | 309 | 336 | 144 |
| ppu_ref | 2469 | 341 | 3327 | 818 | 2753 | 535 |
| APU | 64.0 | 40.2 | 46.5 | 25.9 | 39.1 | 23.8 |
| display (c2p) | 12.4 | 0.6 | 22.8 | 6.9 | 29.9 | 20.0 |
| total | 2905 | 515 | 4310 | 1220 | 3283 | 831 |
| **speed** | 0.60 % | **3.3 %** | 0.48 % | **1.4 %** | 0.53 % | **2.0 %** |

Remaining PPU cost is mostly the per-dot model that runs when a register access (`$2002` polls: 197 per frame in the line-up screen, `$2004`, `$2005-7`) lands inside a visible line, and `render_line_fast` on drawn frames.

## 13. Round 3, step 1: what the display DMA and the chip RAM cost (FS-UAE A1200)

Setup of every run: FS-UAE 3.1.66 `amiga_model = A1200` (68EC020, 2 MB chip, no other RAM), KS 3.1, `video_sync = off`, **default timing options** (no `accuracy`, `uae_cpu_*` or `cpu_speed` line in `platform-a1200/test/pal.fs-uae`:
FS-UAE's A1200 model default, i.e. the CPU runs at its original speed with the emulator's approximate chip-RAM/DMA contention, not cycle-exact CPU emulation).
Scene: field (snapshot at frame 2000), PAL, profile window ticks 1500..5000, 8-plane (PPU_FB8) build of round 2 plus the new `PLAT_PLANES` / `PLAT_FMODE` options of `platform.c`.
`PLAT_PLANES=0` switches the bitplane DMA off (picture black; the 8-plane display is enabled again only to draw the profile page).

| display | frames in 3500 ticks | speed | translated code ms/frame | rt_x.s ms | rt_x.c ms | total ms/frame |
|---|---|---|---|---|---|---|
| (a) no bitplane DMA | 89 | **2.54 %** | 19.3 | 12.5 | 46.3 | 654 |
| (b) 5 planes, FMODE 1 (32-bit fetch) | 82 | 2.34 % | 20.5 | 13.5 | 50.8 | 710 |
| (c) 8 planes, FMODE 1 (as shipped in rounds 1-2) | 70 | 2.00 % | 25.5 | 16.9 | 61.3 | 832 |
| (d1) 8 planes, FMODE 0 (16-bit fetch) | 43 | 1.23 % | 39.0 | 24.6 | - | 1354 |
| (d3) 8 planes, FMODE 3 (64-bit fetch) | not measured: my DDF values (0x38/0xB8) do not give a valid picture, the telemetry could not be read | | | | | |

Reading: the 8-plane display costs the CPU **21 %** of its speed against no display DMA (translated code 25.5 vs 19.3 ms, +32 %), 5 planes cost 8 % (b vs a); switching 8 to 5 planes gains **17 %** (2.00 -> 2.34 %).
With 16-bit fetch the same display costs 1.65x more (1.23 %). So DMA contention is a factor of 1.0 to 1.3 on the speed, nowhere near the factor of 40 that is missing: the CPU work itself dominates.

Cycle-exact timing: `accuracy = 2` and `uae_cpu_cycle_exact = 1 / uae_cpu_memory_cycle_exact = 1` were tried (same scene, 700 s of wall time): the emulation is so much slower than real time on this host that not even the first frame
of the scene was completed, so no number could be obtained. The default setting above is what all numbers in this document use; if a real stock A1200 has slower chip-RAM access than FS-UAE models, the real speed is lower.

## 14. Round 3, step 2: 5 bitplanes (PPU_FB5)

* **What it is.** `-DPPU_FB5` (default of `tools/build_adf.sh`; `FBFLAG=-DPPU_FB8` gives the old 8-plane build): the framebuffer holds the *palette RAM index* 0..31 of every pixel (background: attribute palette x 4 + pixel,
  0 for pixel value 0; sprites 16 + palette x 4 + pixel; forced blank: the `$3Fxx` index the address points at). The display has **5 interleaved planes** (`PLAT_STRIDE` 160, buffers 38,400 bytes), c2p writes 5 planes
  (`c2p.s` assembled with `P5`), and the 32 colour registers are loaded from a snapshot of the PPU palette RAM when a picture is shown: `nes_rgb[(pal[i] & (gray ? 0x30 : 0x3F)) | red emphasis << 6 | green emphasis << 7]`,
  by the vertical-blank interrupt that flips the buffer (`plat_set_palette32`, nothing is written if the colours did not change). The 16-bit and 8-bit framebuffer builds are unchanged.
* **Palette changes during a frame** (palette RAM written with another value, or gray/emphasis bits changed, while the PPU is on a visible line; `nes.ppu.palvar`), measured with the reference runtime over 6000 frames of the play script and of coverage scripts:
  play (p8) 6 frames (42, 1324, 2451, 3018, 4082, 5195), cov2 6, cov6 6, cov1 8, cov3 2, cov4 4, cov5 1, cov7 3, cov8 1 - **0.02 to 0.13 % of the frames** (screen transitions; 23 to 37 lines of such a frame are drawn with another palette).
  The 5-plane build **does not show such a frame** (the previous picture stays), counted in `disp_skip`. Everything else is exact.
* **Proof.** Host: the PPU_FB5 build with the palette applied at the frame hook is compared with the 16-bit reference build for all 6000 frames: the differing frames are exactly the `palvar` frames (p8: 6 of 6000, cov2 6, cov6 6). Amiga: `run_tests.sh` scenes (frames 100, 300, 400, 1200, 1600, 3000, 5000, PAL and NTSC,
  5 planes): **14 of 14 pixel-exact** against the reference frames after palette application.
* The edit of the PPU files (owned by the PPU agent) is minimal: `FBVAL` for FB5 in `nes.h`, the `CV` / `PIXV` macros and the `PALVAR` hook in `ppu_ref.c`.

## 15. Round 3, step 3: speed table (real A1200 timing, default FS-UAE timing)

Builds are made from a clean checkout (the working tree is shared with other agents). "FB8" = 8 planes, "FB5" = 5 planes, both with the PPU of commit 230437e (closed-form model between register accesses) and the same snapshots.

| scene | round 2 (FB8, old PPU) | now FB8 | **now FB5** | gain FB5/FB8 |
|---|---|---|---|---|
| menu (100) | 3.3 % | 3.2 % | **3.9 %** (212 frames / 5500 ticks, 466 ms/frame) | +20 % |
| line-up (1200) | 1.4 % | 2.95 % | **3.6 %** (198 frames, 500 ms/frame) | +22 % |
| field (2000) | 2.0 % | 2.0 % | **2.8 %** (153 frames, 590 ms/frame) | +37 % |

ms per NES frame, FB5, current tree (menu / line-up / field): translated code 3.3 / 7.2 / 19.7, `rt_x.s` 2.7 / 5.7 / 13.5, `rt_x.c` 10.1 / 19.5 / 46.3, `rt_core` 24.6 / 31.7 / 41.2, `ppu_ref` 352 / 363 / 425,
`cpu6502`+mapper 2.5 / 4.2 / 13.3, APU 34.6 / 27.2 / 22.3, c2p + display 0.5 / 2.3 / 15.8, main/audio/platform 1.2 / 1.4 / 1.2.
Largest remaining costs: `ppu_ref` (72-81 %: still the line renderer and the sprite/tile work on drawn frames plus the state-only lines), then `rt_x.c` + `rt_x.s` + translated code in the field scene (80 ms), APU (22-35 ms), c2p (16 ms in the field scene).

## 16. Round 4: 8 MB fast RAM (the target machine now has it)

**Assumption (default build, `FASTRAM=1`):** an A1200 with an 8 MB 32-bit fast RAM board covering `$200000-$9FFFFF` (FS-UAE: `fast_memory = 8192` gives exactly this, Z2 autoconfig card at `$200000`, 8192 KB; the trapdoor boards of
real machines (Blizzard 1230, Apollo, ...) map their first 8 MB at `$200000` as well when autoconfigured as Zorro II-style RAM; boards that map elsewhere (e.g. at `$08000000`) are not supported by this link address).
The bootblock and the HDF loader scan `ExecBase->MemList` while the OS is alive for a `MEMF_FAST` MemHeader with `mh_Lower <= $200100` (the header sits at the start of the region) and `mh_Upper >= $A00000`;
if there is none the ADF shows an Intuition `DisplayAlert` ("Little League Baseball needs 8 MB FAST RAM at $200000") and the HDF loader prints the message in the CLI and returns RETURN_FAIL (tested with `fast_memory = 0`: alert shown).
`FASTRAM=0 tools/build_adf.sh` (and `build_hdf.sh`) builds the old chip-only layout (`link.ld`); `FASTRAM=0` also switches the FS-UAE test configs back to `fast_memory = 0` (`run.sh`, `run_hdf.sh`).

Memory model: `platform-a1200/link_fast.ld` links the payload (code 252 KB, runtime, read-only data, ROM, BSS: dispatch tables, PPU/APU state, framebuffer, display shadows, 16 KB stack) at `$200000` (end `$3774C8`: 3.4 MB of the 8 MB are used);
in chip RAM remain only the vectors (0..$3FF, copied by crt0), and the section `.chipbuf` linked at `$1000` (bitplane buffers 2 x 38,400, Copper list, null sprite, Paula chunk ring and silence buffer; `$1000-$148B8`, cleared by crt0).
The bootblock/loader still stage the payload (585 KB) in chip RAM and the relocator (running from `$200`) copies it to `$200000` (descending memmove) before jumping there. Interrupt handlers, stack and all data are in fast RAM; I-cache on, no D-cache (68EC020).
Sparse dispatch tables and the shrunk APU queues are kept (harmless; the tables expand at boot into 8 x 64 KB of fast RAM).

Verification (FS-UAE, A1200, KS 3.1, `fast_memory = 8192`): `run_tests.sh` scenes **14 of 14 pixel-exact** (PAL + NTSC); the HDF (`build_hdf.sh`, fast RAM loader) boots to the title (frame 30) and menu (frame 100) with 0 differing pixels on PAL and NTSC, and
the real keyboard (Return, Down) reaches reference frames 45 and 151 with 0 differing pixels (PAL + NTSC); host regression unchanged (the runtime is not touched).

Speed (same snapshots, 5 planes, PAL, profile window ticks 1500..7000, FS-UAE default timing; chip only = commit 633b964 FASTRAM=0):

| | menu (100) chip -> fast | line-up (1200) | field (2000) |
|---|---|---|---|
| frames in 5500 ticks | 212 -> **402** | 198 -> **372** | 153 -> **280** |
| **speed** | 3.86 % -> **7.3 %** | 3.6 % -> **6.8 %** | 2.78 % -> **5.1 %** |
| translated code ms/frame | 3.3 -> 1.9 | 7.2 -> 3.8 | 19.7 -> 10.7 |
| rt_x.s + rt_x.c | 12.8 -> 6.2 | 25.2 -> 11.1 | 59.8 -> 26.5 |
| rt_core | 24.6 -> 13.6 | 31.7 -> 15.9 | 41.2 -> 20.2 |
| ppu_ref | 352 -> 183 | 363 -> 192 | 425 -> 230 |
| APU | 34.6 -> 20.8 | 27.2 -> 18.2 | 22.3 -> 23.7 |
| c2p + display | 0.5 -> 0.3 | 2.3 -> 1.5 | 15.8 -> 9.0 |
| total ms/frame | 466 -> 252 | 500 -> 268 | 590 -> 330 |

Fast RAM gives a factor 1.85 everywhere (code and data accesses no longer wait for chip-RAM slots); the PPU model (70-80 % of the time) is the remaining problem.

## 17. Round 5: state at a2a2288 (closed-form PPU, APU cue mode, fast RAM, 5 planes)

**Verification** (FS-UAE A1200 + 8 MB fast RAM, KS 3.1; clean checkouts; base a2a2288 = d441c40 + the cue-mode chip-RAM fix): host trace comparison IDENTICAL over 2,556,952 records (6000 frames); `run_tests.sh` scenes **14 of 14 pixel-exact** (PAL + NTSC; run at d441c40);
the HDF (`tools/build_hdf.sh`) boots to the title (frame 30) and menu (frame 100) with 0 differing pixels and the keyboard (Return, Down) reaches reference frames 45 and 151 with 0 differing pixels, PAL and NTSC (a2a2288 image).
Delivered image: `build/llb_fastram.hdf` (not in git), SHA-256 `3e9199ab1befa93bc5b63b6de89edfefc1b6a3b175cf425ed1ac269aaf32bbbd`.

**Audio in cue mode (default)** replaces the PCM path of section 7: no samples are produced; once per frame `apu_cue_frame()` derives from the APU registers/counters at the end of the frame, for each NES channel, a Paula period, a volume and a looping
waveform (`docs/APU.md`, "Cue mode"), and `audio_cue_apply()` writes only the registers that changed: pulse 1 -> Paula 0 (left), pulse 2 -> 1 (right), noise -> 2 (right), triangle -> 3 (left). The waveforms are built by the APU module in fast RAM and
copied into a chip RAM pool on first use (Paula cannot read fast RAM; before commit a2a2288 the fast-RAM build was silent). The oscillators free-run (no phase lock to the NES), so what can be checked is the spectrum, not the waveform:
`tools/apu_cuesim.c` + `tools/apu_cuecmp.py` simulate Paula against `apu_render8` (spectral magnitude correlation median 0.90, loudness median -0.03 dB, `docs/APU.md`). `tests/amiga/audio_cmp.py` (waveform correlation of a recording with the exact PCM) therefore only applies to the
`-DAUDIO_EXACT` build; for the cue build the FS-UAE check is: recording of the stopped frame 1300 (`-DSTOP_AT=1300`): not silent, rms 2318 / peak 9070 (16-bit units), dominant frequency 219 Hz (a held tone of the stopped frame), and the DMA state follows the APU registers.
`audio.c`'s underrun/ring logic is only used in the exact build.

**Speed and profile at d441c40** (same method: snapshots of the play script, PAL, 5 planes, fast RAM, ticks 1500..7000, FS-UAE default timing):

| | menu (100) | line-up (1200) | field (2000) |
|---|---|---|---|
| frames in 5500 ticks | 3684 | 1081 | 1243 |
| **speed** (previous round) | **67 %** (7.3 %) | **19.7 %** (6.8 %) | **22.6 %** (5.1 %) |
| translated 6502 code ms/frame | 1.8 | 8.7 | 8.0 |
| rt_x.s | 0.9 | 3.5 | 3.3 |
| rt_x.c | 1.0 | 3.9 | 3.7 |
| rt_core | 5.3 | 9.7 | 9.5 |
| ppu_ref (fp_line, ppu_fp_run, ...) | 13.0 | 46.3 | 38.7 |
| cpu6502 + mapper | 0.7 | 2.7 | 2.4 |
| APU (cue) | 1.0 | 1.2 | 1.2 |
| c2p + display | 0.4 | 7.8 | 6.0 |
| main/audio/platform | 0.9 | 0.8 | 0.9 |
| total ms per NES frame | 25 | 85 | 74 |

Hottest functions (share of all samples): `fp_line` 21 % (line-up) / 17 % (field), `c2p_frame` 5.5 % / 4.7 %, `xrt_rd_c` 2 %, `mem_read` 2 %, `rt_wr_mmc1` 1.2 %, `fetch_tile` 1 %.

**Long run** (field start of the Watch Game script: snapshot at frame 1600, normal telemetry build, 380 s of emulated time): game frames 2288 -> 7497 in ticks 2959 -> 25792, i.e. **22.8 %** on average (5209 frames = 87 s of game time in 380 s); the display converts every 3rd to 4th frame (1474 converted, 4423 skipped while behind).

## 18. ARENDER_HW: planar renderer on the Amiga (stage 2a; build option, default OFF)

`ARENDER_HW=1 tools/build_adf.sh <rom> <adf>` (also `build_hdf.sh`) replaces the chunky framebuffer + c2p path by the planar picture of `runtime/arender.c` (docs/RENDER.md):
* main.c sets `ppu_ar_enable = 1` before `xrt_run`; on every drawn frame the PPU model logs the scene and `ar_end_frame()` fills `ar_out` before the frame hook (exact lines are converted inside arender, the platform never touches `nes.ppu.fb`).
* `platform-a1200/display_ar.c` (replaces `display.c`/`c2p.s`): copies the 5 planes (5 x 7680 bytes, plane-major, BPLxMOD 0) from arender's buffer in fast RAM into the back display buffer in chip RAM and builds **one Copper list per picture** (2 lists x 16 KB in chip RAM):
  null sprite pointers, the 32 colour registers of the first region (BPLCON3 LOCT two-pass), the 5 bitplane pointers, and for every further region whose palette/gray/emphasis differs the changed colour registers, written in the horizontal blank of the line before the region
  (`WAIT line y0-1, x=$D2`; lines above 255 use the `WAIT 255,$DF` wrap). `plat_show(b)` makes the vertical-blank interrupt write `COP1LC` (`plat_cop_ptr[b]`), so pointers and colours of a picture switch together. Frames whose picture is skipped (behind real time, flip pending) are not composed.
* Chip RAM budget: 2 x 38,400 (planes) + 2 x 16 KB (Copper) = 109 KB; the persistent background bitmaps (arender's 8 configurations x 128 KB = 1 MB) stay in **fast RAM** (the display reads the composed planes, not the bitmaps). Measured over the 6000-frame play script and cov1-8 with an 8-entry LRU: **3 to 9 distinct configurations per run
  (p8 9, cov6 8, cov2 7, cov1 6, cov4/cov7 5, cov3/cov5 4, cov8 3), and the number of cache misses equals the number of distinct configurations (nothing is rebuilt after eviction)**. Pointing the display straight into the bitmaps (no copy, BPLCON1 fine X, sprites on top) would need the active configurations in chip RAM: 3 to 4 x 128 KB = 384-512 KB would fit; not done.
* Colour changes that need more MOVEs than fit into the blank (all 32 colours, e.g. the emphasis flash of the start screen) are late by 1-3 lines: option `-DAR_SKIP_BIG_PALETTE=n` shows no picture instead.

Verification (FS-UAE A1200 + 8 MB fast RAM): `run_tests.sh` scenes **14 of 14 pixel-exact** (PAL + NTSC, `ARENDER_HW=1`); frames with mid-frame palette changes (`STOP_AT` 1324, 2451, 3018, 4082 PAL, 1324, 2451, 3018 NTSC): 0 differing pixels (Copper colour switching works);
frame 42 (emphasis flash, 32 colours): 583 pixels differ (rows 23-25, late colours), PAL and NTSC; the HDF built with `ARENDER_HW=1` boots to the frame-30 and frame-100 scenes with 0 differing pixels (PAL + NTSC).
**Not exact: free running with the real keyboard** (HDF, Return then Down, 12 s waits): the screenshots are the right screens but 4,600-9,100 pixels differ from the closest reference frames (top region colours wrong in the shots), while the chunky build matches exactly in the same test. The scene tests always draw every frame (`STOP_AT`),
the free-running build draws only every 3rd-4th frame while behind; the cause is not found (suspect: arender state across undrawn frames, or palette state of the first region); `PPU_CHECK=1 ARENDER=1` of the host harness already fails at this commit (sp0hit, frame 8), so the PPU agent has to look at it first.

Speed (same method, PAL, fast RAM, 5 planes; chunky build in brackets): menu **19.7 %** (67 %), line-up **14.6 %** (19.7 %), field **13.3 %** (22.6 %), 380 s Watch Game run **13.5 %** (22.8 %). So **the planar path is currently slower**:
`ar_flush` (the region renderer in arender.c) is 42-50 % of all samples = 53-66 ms per NES frame averaged over all frames (about 4x that per composed picture), `ensure_trow` 3-4 %, `ar_end_frame` 4-5 %, memcpy/memset 4-7 %, my plane copy 2-3 %; the PPU model itself dropped to 12-13.5 ms/frame (from 46/39) and c2p is gone.
Per frame ms (menu / line-up / field): translated 1.8 / 6.3 / 9.3, rt_x.s 0.8 / 2.6 / 3.4, rt_x.c 1.0 / 2.8 / 4.1, rt_core 5.2 / 8.0 / 9.9, PPU model 11.9 / 13.5 / 12.5, cpu6502 0.7 / 2.0 / 2.5, APU 1.0 / 0.8 / 1.0, my copy 2.9, **arender 50 / 66 / 71**.
The renderer needs the same treatment the chunky path got (byte loops with fine-X shifts of 32-byte rows on 5 planes per line are too slow on a 68020); stage 2b (display pointers into the bitmaps, BPLCON1 fine X, sprites as the only per-line work) removes the shifting entirely.

## 19. Stage 2b hardware: dual playfield, Copper list from regions (platform-a1200/display_ar2.c)
Status: builder and display mode verified on the synthetic scene (PAL and NTSC, 0 differing pixels); not yet connected to the live arender (waits for the PPU agent's stage 2b output).
* **Format (fixed)**: AGA dual playfield, 4 + 4 planes. BPLCON0 0x0611, BPLCON2 0x0040 (playfield 2 = sprite layer in front), BPLCON3 0x1020 (PF2OF = 16), FMODE 0, DDFSTRT 0x40 / DDFSTOP 0xC0 (17 words per line, the extra word is hidden by the scroll or lies left of DIWSTRT).
  Playfield 1 (odd planes) = background window into the persistent bitmaps, scroll BPLCON1 low nibble; playfield 2 = sprite layer (screen aligned).  Colour registers: 0..15 background value (4, 8, 12 = backdrop), 16..31 sprite entries.
  DMA cost: 8 planes with the program in fast RAM costs nothing measurable (22.79 % vs 22.85 % with 5 planes), so this is the cheaper exact format (no chip-RAM copy of the picture any more).
* **Pointer arithmetic**: N = bytex*8 + fx, r = N & 15; r = 0: word (N>>4)-1 mod 32 (N = 0: word 31, the guard bytes continue with words 0..), scroll 0; else word N>>4, scroll 16-r.  Modulo bitmap stride - 34 (PF1), -2 (PF2: pointer = row - 2).
  Requires **AR_BROW = 96** (32 guard bytes), and 2 spare bytes in front of the sprite layer / exact playfields (reported in docs/RENDER.md).
* **Copper list per picture** (`ar2_build(list, max, frame, mem)`): region 0 at the start of the list (sprite pointers null, all 32 colours, pointers, modulo, scroll); every further region after `WAIT (y0-1, 0xCA)` (after the last fetch of the previous line); lines above 255 use the 0xFFDFFFFE wrap.
  Only changes are written: a region that continues (same bitmap, row sequence and window) costs no pointer MOVEs because the DMA pointers already stand at the next line (the builder tracks where the DMA will be); colours: hi/lo nibble pass only for changed registers.
  Optional `Ar2Mem.used[i]` masks: unused colours are not written, colours unused by the previous region are written one line earlier.
* **Budget**: about 22-26 MOVEs fit between the last fetch of a line and the first pixel of the next (4 CCK each, the hblank without fetch is ~91 CCK; colours are first needed ~13 CCK after the fetch restarts). Measured: a region switch with 8 pointer lo words + 8 changed colours (17 MOVEs with the BPLCON3 passes) + scroll is late by 5..10 pixels on its first line without the used masks, exact with them.
  A full palette change (emphasis flash) always needs the `used` masks of the PPU agent (or it shows the first lines wrong).
* **Test**: `ARHW_TEST=1 tools/build_adf.sh` builds platform-a1200/arhw_test.c (synthetic bitmaps with pixel formulas, 10 regions: N = 0, N = 511, odd fine X, row wrap restart, exact, blank, line > 255, palette changes, used masks); `tests/amiga/arhw_expect.py` computes the picture; run_tests.sh section "stage 2b display hardware" (PAL + NTSC, 0 pixels tolerance).

### 19.1 Live stage 2b (ARENDER_HW=2, now the default of build_adf.sh / build_hdf.sh; `ARENDER_HW=0` = chunky + c2p, `1` = stage 2a)
* **Integration**: `display_init()` gives arender its chip-RAM block (`ar_hw_setmem`, 970 KB in `.chipbuf`) and sets `ar_hw_enable = 1`; `display_frame()` builds the Copper list for `ar_out` (`ar2_build` with `Ar2Mem{ar_hw.bmp, spr, xpf1, xpf2, AR_BROW, used[]}`), `plat_show()`, then `ar_hw_shown()` (exactly once per composed picture: a second call for the same picture, as the old `STOP_AT` loop did, sees the other sprite/exact buffer set).
  The list sets FMODE/BPLCON0/BPLCON2/DDF itself, so the 5-plane buffers of platform.c are idle. Colours come from `ar_hw_palette()`. Telemetry (TELEM/PROFILE builds) is a 4-plane page that replaces lines 224.. (or the whole picture for the profile page) via a last Copper region.
* **Big palette switches** (frame 42 emphasis flash, 64 MOVEs): the colours are written into the hidden colour bank (registers 128..159, BPLCON3 BANK 4) in the hblanks of the lines before the region (<= 16 MOVEs per line) and the region switch is a single `BPLCON4 = $8011 / $0011` write (XOR $80 on the colour index; works in dual playfield, verified in ARHW_TEST: region 9 changes all 23 shown colours). The toggle waits for hpos $D4 because the last pixels of the previous line are displayed until $CA.
  If there are too few lines before the region the colours are written late as before (first line wrong). With the bank scheme frame 42 is exact (it was 512 pixels off).
* **Verification** (FS-UAE A1200 + 8 MB fast RAM, FAST=1, commit 3f23920 + below): `run_tests.sh` scenes 100/300/400/1200/1600/3000/5000 PAL and NTSC **14 of 14, 0 differing pixels**; mid-frame palette frames 42, 1324, 2451, 3018, 4082 PAL (42, 1324 NTSC): 0 pixels; free-running keyboard path (long waits, nearest reference frame, 0 pixels tolerance): ADF PAL+NTSC k1..k5 (frames 45/151/186/301/345) **0 pixels**, HDF PAL+NTSC (45, 151) **0 pixels**; ARHW_TEST synthetic scene PAL+NTSC 0 pixels.
* **Speed** (PAL, fast RAM, FS-UAE default timing, snapshots of the play script, ticks 1500..7000; chunky path of section 17 in brackets): menu **29.9 %** (67 %), line-up **25.7 %** (19.7 %), field **24.4 %** (22.6 %); 380 s Watch Game run (snapshot 1600): frames 2213 -> 8525 in ticks 2373 -> 25388 = **27.4 %** (22.8 %); display converts nothing any more (the Copper list is the display), 1732 pictures shown.
  Per frame ms (menu / line-up / field): translated 5.1 / 8.4 / 8.6, rt_x.s 2.4 / 3.3 / 3.4, rt_x.c 3.0 / 3.7 / 3.9, rt_core 7.1 / 7.6 / 8.1, PPU model 11.0 / 9.2 / 9.5, cpu6502 1.1 / 1.6 / 1.6, APU 0.7 / 0.9 / 1.1, main/audio/platform 1.3 / 1.4 / 1.5, **arender (counted under libgcc/other) 23.8 / 28.6 / 30.4**.
  Hottest: `ensure_trow` 14 / 21 / 22 % of all samples (rebuilding bitmap tile rows), `ar_flush` 4-6 %, `ar_begin_frame`/`ar_end_frame` 2.5-3.5 % each. The menu is slower than the chunky path (the chunky PPU skips undrawn frames cheaply and the title screens change the bitmaps constantly); gameplay is 8-13 % faster, and with arender's `ensure_trow` fixed it should gain much more (arender is now the largest single component).

### 19.2 State at 90af238 (= 1025759 + profile categories): tests, speed, whole-picture table
Tests (FS-UAE A1200 + 8 MB fast RAM, `FAST=1`, clean worktree, `run_tests.sh` full set PAL + NTSC): scenes **14 of 14** pixel-exact; mid-frame palette frames 42, 1324, 2451, 3018, 4082 PAL + NTSC **10 of 10**; free-running keyboard ADF (PAL + NTSC, frames 45/151/186/301/345) and HDF (45/151) **0 pixels**; ARHW_TEST PAL + NTSC **0 pixels**; joystick/keyboard ports, audio chain (underruns 0), flip logic (PAL 499 of 600 frames shown, NTSC 598) as before. No exactness problem seen around frame 2650 (scene 3000 and the free-running tests pass).
Speed (PAL, fast RAM, default timing, snapshots, ticks 1500..7000, one category scheme: arender.o has its own row, display code is part of "display + main/audio/platform"; the profile build's scripted `input()` scans the script on every read and `telem_draw` is test-only, together ~1.3 ms of that row):

| ms per NES frame | menu (100) | line-up (1200) | field (2000) |
|---|---|---|---|
| frames in 5500 ticks / speed | 2090 / **38.0 %** | 1846 / **33.6 %** | 1909 / **34.7 %** |
| translated 6502 code | 6.2 | 9.2 | 8.2 |
| rt_x.s (dispatch glue) | 2.7 | 3.4 | 3.3 |
| rt_x.c (I/O helpers, slow path) | 3.3 | 4.0 | 3.9 |
| rt_core (time model, PPU register access) | 7.4 | 8.0 | 8.0 |
| ppu_ref (PPU model) | 10.6 | 9.2 | 9.5 |
| cpu6502 + mapper | 1.2 | 1.7 | 1.5 |
| APU (cue) | 0.9 | 1.1 | 1.1 |
| arender | 8.7 | 10.2 | 9.7 |
| display (ar2_build, plat) + main/audio/platform | 2.8 | 2.7 | 2.7 |
| total | 43.8 | 49.6 | 48.0 |

380 s Watch Game run (snapshot 1600): frames 2481 -> 11413 in ticks 2376 -> 25376 = **38.8 %** (chunky path 22.8 %); 2454 pictures shown. (Previous stage 2b state 69e4a07: 25.7 / 24.4 / 27.4 %.)
`ar2_build` is below the resolution of the profile (`pal_items` 0.4 % of samples; the Copper list costs well under 1 ms per picture); the frame hook, input poll and platform code together are about 2 ms. To reach 100 % the frame must fit 16.6 ms: the next targets are the PPU model + `rt_core` + `rt_x` (about 24 ms together: every PPU register access and time-model sync), arender (~10 ms), and the translated code (~8 ms, dominated by cycle accounting).

### 19.3 State at c6700b2: complete verification, speed, delivered images
Tests (FS-UAE A1200 + 8 MB fast RAM, `FAST=1`, clean worktree of c6700b2, `tests/amiga/run_tests.sh <rom> <dir> both`): scenes **14 of 14** pixel-exact (PAL + NTSC); free-running keyboard ADF (frames 45/151/186/301/345, PAL + NTSC) and HDF boot (45/151) **0 pixels**; ARHW_TEST PAL + NTSC **0 pixels**; controllers (sticky_input 3327 / 65292), audio chain (underruns 0, correlation median 0.97 PAL / 0.95 NTSC), flip logic (PAL 499 of 600 frames shown, NTSC 598).
Mid-frame palette frames 42, 1324, 2451, 3018, 4082 (STOP_AT builds, reference from the translated host build under `qemu-m68k -singlestep`): PAL + NTSC **10 of 10, 0 pixels**.
The delivered `llb_final2.hdf` (built from the same commit with `tools/build_hdf.sh`) was booted separately: keyboard Return / Down reach reference frames 45 / 151 with 0 pixels, PAL + NTSC.
Host: native lazy interpreter `PPU_CHECK=1` with `ARENDER=1` and `2` at `DRAWPAT` 1, 3, 4, 5, 6, 11 IDENTICAL over 58,283,430 records; translated qemu `-singlestep` IDENTICAL over 2,556,952 records (`A=1`, `DRAWPAT=3`, `4`, `6`).

Speed (PAL, fast RAM, default timing, snapshots of the play script, profile window ticks 1500..7000 = 5500 ticks, same category scheme as 19.2; the profile build's scripted `input()` and `telem_draw` are test-only, together ~1.3 ms of the last row):

| ms per NES frame | menu (100) | line-up (1200) | field (2000) |
|---|---|---|---|
| frames in 5500 ticks / speed | 4369 / **79.4 %** | 2094 / **38.1 %** | 2133 / **38.8 %** |
| translated 6502 code | 1.8 | 9.3 | 8.9 |
| rt_x.s (dispatch glue) | 0.9 | 3.1 | 3.0 |
| rt_x.c (I/O helpers, slow path) | 1.0 | 2.1 | 2.0 |
| rt_core (time model, PPU register access) | 3.8 | 5.0 | 5.1 |
| ppu_ref (PPU model) | 8.2 | 8.5 | 8.8 |
| cpu6502 + mapper | 0.6 | 1.7 | 1.6 |
| APU (cue) | 1.1 | 1.1 | 1.1 |
| arender | 2.3 | 10.4 | 9.9 |
| display (ar2_build, plat) + main/audio/platform | 1.3 | 2.5 | 2.4 |
| total | 21.0 | 43.7 | 42.8 |

(19.2 at 90af238: 38.0 / 33.6 / 34.7 %, i.e. line-up +13 %, field +12 %; the menu window starts at game frame 1254 here, so its percentage is not comparable with the old row.)
380 s Watch Game run (snapshot 1600, normal telemetry build): frames 2805 -> 12885 in ticks 2973 -> 25971 = **43.8 %** (19.2: 38.8 %); 2520 pictures shown (disp_conv), late 11285, underruns 0.

Delivered images (built from c6700b2, ROM not included): `llb_final2.adf` SHA-256 `1d97d6b150d52b6c651a332366d0a74434006959f5c3d8155fae059357084ed9`, `llb_final2.hdf` (= build/llb_fastram.hdf) SHA-256 `32c2c6caad3daf29f35b1092038711ca8a21132daab6b4c4a51231f0b011f673`.

What 1:1 (16.6 ms per frame for everything) would still need: the line-up / field frame costs 43 ms, i.e. 2.6x too much, and every block is large: translated code + dispatch + helpers 14 ms (cycle accounting per block and one helper call per I/O or indirect access), time model + PPU model 14 ms (about 1500 cycles per synchronisation piece, 50-100 pieces per frame), arender 10 ms per game frame (15-20 ms per drawn frame), the rest 6 ms.  Tuning single hot spots (done in this and the previous rounds) gives 10-15 % per round; 1:1 needs a different structure: (a) a PPU model that does not run between accesses at all (accesses to the PPU recorded with their cycle and the frame replayed once from the recorded register writes, closed-form), (b) translator output with cycle budgets per superblock and no per-block accounting for blocks without I/O, (c) arender drawing only every 4th-6th frame and composing sprites with the blitter or a cached per-sprite 16 pixel pre-shift, (d) fewer I/O helper calls by translating the common PPU register writes inline.  Together these are estimated at 20-25 ms, so even then 60 Hz is not certain on a 14 MHz 68EC020; 50-60 % is the realistic target for the next rounds.
