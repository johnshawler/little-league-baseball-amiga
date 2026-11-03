# Little League Baseball: Championship Series (NES) → Amiga 1200: Facts page

> Discovery-phase document (written before the port was built). Later measurements and the as-built design are in `docs/DESIGN.md` and `docs/VERIFICATION.md`.

Status: DISCOVERY only. Nothing has been ported yet. Every fact is tagged
**[driver]** (MAME source, quoted), **[ROM]** (derived from the ROM and/or a reference-emulator trace) or **[assumed]**.
Raw statistics behind the [ROM] facts are in `docs/stats_*.txt`; the scripts that produce them are in `tools/discovery/`.

## 0. What the files are

| file | size | check | result |
|---|---|---|---|
| `Little League Baseball - Championship Series (USA).nes` (in the zip) | 262,160 | CRC32 7905ae67, SHA1 45564d86…29a1 | iNES header `4E 45 53 1A 08 10 10 08 …` |
| PRG (8 × 16 KB) | 131,072 | CRC32 **b3440357**, SHA1 ffacaeb3…0e1e934ddbfa | matches `hash/nes.xml` `littlelg` "nes-kq-0 prg" **[driver]** |
| CHR (16 × 8 KB) | 131,072 | CRC32 **2e60f8ee**, SHA1 963cd582…150c16 | matches "nes-kq-0 chr" **[driver]** |
| `Kickstart-3.1-A1200-40.68.rom` | 524,288 | not identified by me (not needed to port; test only) | never shipped |

* **Game/board [driver]**: MAME `hash/nes.xml` (master, commit 81d5ced8) software `littlelg`, "Little League Baseball Championship Series (USA)", SNK 1990, serial NES-KQ-USA,
  `slot=sxrom`, `pcb=NES-SLROM`, `mmc1_type=MMC1B2`, PRG 131072, CHR 131072 (CHR **ROM**, no CHR-RAM, no WRAM, no battery).
* iNES header **[ROM]**: PRG 8×16 KB, CHR 16×8 KB, flags6=0x10 (mapper low nibble 1 = MMC1, no battery, no trainer), flags7=0x08 (NES 2.0 identifier), mapper **1**.
* Machine: an NTSC NES (Ricoh 2A03 + 2C02 PPU) with an SLROM/MMC1B cartridge. This is **not an arcade board**; there is no MAME arcade driver, the "driver" is `nes.cpp` + the cartridge device `mmc1.cpp`.
  The cart is a USA NTSC game, so the PAL NES timing (`nespal`, 50.0070 Hz) is not a target: the reference timing is NTSC.

## 1. Source hardware (from the MAME driver; files fetched from github.com/mamedev/mame master, commit 81d5ced8)

| item | fact | source |
|---|---|---|
| CPU | Ricoh RP2A03G (6502 core, **no decimal mode**, on-chip APU), clock = 21.477272 MHz / 12 = **1.789773 MHz** | `nes.cpp:410` `RP2A03G(config, m_maincpu, NTSC_APU_CLOCK)`; `rp2a03.h:74,76` `RP2A03_NTSC_XTAL XTAL(21'477'272)`, `NTSC_APU_CLOCK (RP2A03_NTSC_XTAL/12)` |
| Only one CPU. No MCU, no second CPU, no coprocessor on the cart | | `nes.cpp:407-445` (single `maincpu`), `mmc1.cpp` (no CPU) |
| Main map | `$0000-$07FF` RAM (2 KB) mirrored to `$1FFF`; `$2000-$3FFF` PPU registers (mirrored every 8); `$4014` OAM DMA; `$4016` IN0 (controller 1 strobe/read); `$4017` read IN1 (controller 2); `$4100-$5FFF` cart low handler; `$6000-$7FFF` cart mid (WRAM, absent on this cart); `$8000-$FFFF` cart high (PRG read, mapper write) | `nes.cpp:386-391` and comments `:392-394` |
| APU | `$4000-$4013` and `$4015` (write), `$4015` read status, `$4017` write = frame counter | `rp2a03.cpp:22-25,29` |
| Screen | 256×240 visible (`set_visarea(0*8, 32*8-1, 0*8, 30*8-1)`), 262 scanlines, refresh **60.0988 Hz** | `nes.cpp:414,421,422` |
| PPU | 2C02 (NTSC): 262 scanlines/frame, vblank from scanline 241 to 260 | `ppu2c0x.h:56,61,64`; `ppu2c0x.h:266` |
| Interrupts | NMI from PPU vblank; IRQ from APU (frame counter, DMC) and mapper `scanline_irq`/`hblank_irq` hooks (unused by MMC1) | `rp2a03.cpp:71,85`; `nes.cpp:175-176` |
| Mapper | MMC1 (`nes_sxrom_device`, `NES-SLROM`): 5-write serial shift register, 4 registers selected by address bits 13-14 of the write; all writes to `$8000-$FFFF` | `mmc1.cpp:244-253` (`m_latch >>= 1; m_latch |= (data & 1) << 4; m_count = (m_count+1)%5; … reg = BIT(offset,13,2)`); bit 7 set resets the shift counter and ORs `0x0c` into reg0 (`mmc1.cpp:234-241`) |
| Mapper reset state | reg0 = 0x0f, others 0 | `mmc1.cpp:82-85` |
| PRG banking | reg0 bits 2-3: mode 0/1 = 32 KB, 2 = fix `$8000`, switch `$C000`, **3 = switch `$8000`, fix last bank at `$C000`** | `mmc1.cpp:153-168` |
| CHR banking | reg0 bit 4: 8 KB mode or **two independent 4 KB banks** (reg1 → `$0000`, reg2 → `$1000`) | `mmc1.cpp:174-182` |
| Mirroring | reg0 bits 0-1: 0 = one-screen low, 1 = one-screen high, **2 = vertical**, 3 = horizontal | `mmc1.cpp:185-190` |
| Consecutive-cycle writes | MMC1 ignores a second write on the cycle right after the first (matters only for read-modify-write instructions on `$8000-$FFFF`) | `mmc1.cpp:218-231` |
| PRG-RAM/WRAM | none on this cart (SLROM); `$6000-$7FFF` unused | `nes.xml` entry has no `wram` dataarea; trace shows 0 accesses (§2) |

Caveat on the driver as a reference: `ppu2c0x.cpp:215` says "monochrome and color emphasis aren't supported" and `:279` that the PPU "draws sprites into a frame buffer before displaying them". MAME's `ppu2c0x` is a scanline renderer,
not a dot-accurate one. It is a good source for the memory map and mapper but a weaker oracle for mid-frame timing than a dot-accurate emulator (see §4).

## 2. What the ROM does (from a reference-emulator trace; all numbers are from real runs, none are guesses)

Method: a libretro host (`tools/discovery/host.c`) drives the Mesen core (patched, `mesen-trace.patch`) headless, scripted input; every executed opcode (with the physical PRG offset),
every `$2000+` read and write is logged with the CPU cycle and frame number. Runs: **attract 1500 frames (25 s)** with no input; **play 6000 frames (100 s)**: Start, Exhibition Mode, *Watch Game* (the computer plays both sides), then A presses
(`inputs/play_watchgame.txt`). The play run reaches the live field (pitching, batting, fielding views) and the menus, lineup, team-select and umpire screens before it. 52.5 M instructions executed in the play run.
**It does not reach: Championship Series play, Power Analysis, password entry (reached in an earlier exploratory run), 2-player input, game over/series end.** Coverage of the ROM is therefore partial and is tracked in §6.

### Code vs data, banks
* Reset `$CBD5`, NMI `$CD85`, IRQ/BRK `$CDF4` (a single `RTI`) **[ROM, vector read from `$FFFA-$FFFF`]**. Bank 7 is fixed at `$C000-$FFFF`; banks 0-6 are switched into `$8000-$BFFF`.
* Executed instruction start addresses: **12,868** (play) / 13,014 with attract; code bytes covered by the static closure of those entries: ~36 KB of 128 KB (`docs/stats_static_closure.txt`; the closure assumes every bank for calls from the fixed bank, so it is an upper bound for the code seen; its "illegal opcode"/"RAM jump" lines are artefacts of decoding data as code in a wrong bank, not game behaviour).
  Per bank executed instruction addresses: bank 0: 1196, 1: 1181, 2: 923, 3: **0 in the play run** (231 in an earlier run), 4: 628, 5: 458, 6: 4677, 7: 3805. The rest of the 128 KB is data (text, tables, music data).
* Undocumented opcodes executed: **0**. `SED` executed: 0 (decimal flag is irrelevant on the 2A03 anyway). `BRK` executed: 0. `CLI` executed: **0**, `SEI` once (at reset): the I flag stays set, so **IRQ is never taken**; the IRQ vector handler is only the shared `RTI` at the end of the NMI handler.
* **Self-modifying code / code in RAM: none observed.** Instructions executed from addresses < `$8000`: **0** of 52.5 M. The cart has no WRAM and PRG is ROM, so the only way to change code is bank switching.
* **Bank switching is centralised**: stores that hit `$8000-$FFFF` come from **4 store sites, all in fixed bank 7** (the MMC1 serial writer; `an4.py`). Final MMC1 state used by the game: reg0 is written **once**, value **0x1E** (vertical mirroring, PRG mode 3 = switch `$8000` / fix `$C000`, CHR 4 KB mode) and never changes;
  reg3 (PRG bank) values 0-6; reg1/reg2 (CHR banks) values from 0 to 29 (`docs/stats_io_values.txt`). Frequency: 660,491 mapper-address writes in 6000 frames ≈ 110 writes/frame, 5 per register load (≈ 22 loads/frame, mostly PRG).
* Note for the mapper model: reg1 values with bit 4 set (18, 24, 27, 29) occur while PRG banks come from reg3. On SLROM (128 KB PRG) that bit is a CHR address bit, it must **not** be used as a PRG offset (MAME's `prg_offset = m_reg[1] & 0x10`, `mmc1.cpp:147`, only matters for ≥256 KB PRG). PRG bank = reg3 & 7.
* **Computed jumps**: 7 `JMP (ind)` sites executed, **no RTS-dispatch** (every `RTS` returns to a `JSR+3`; the 18 exceptions all return into the NMI entry, i.e. they are interrupted RTS instructions, `docs/stats_indirect_jumps.txt`):
  * bank 0 `$833B`: state dispatcher inside bank 0 (13 distinct targets, all in bank 0);
  * bank 7 `$C246`: **the far-call/main dispatcher**; hundreds of distinct targets in banks 0, 1, 2, 3, 4, 5, 6 and 7 (most in bank 6, the game logic); the target pointer comes from ROM tables and RAM;
  * bank 7 `$C212`, `$C66A`, `$C80E`, `$C82E`, `$C860`: smaller dispatchers (targets in banks 1, 2, 3, 4, 5, 6, 7).
  The target set is **not statically closed by direct control flow**; it needs the pointer tables decoded (planned, §6) plus a runtime safety net.
* **Peak cycles per frame** (CPU cycles between NMI entries, minus the cycles spent in the two idle/poll loops: wait-for-NMI `$CC09/$CC0C` and sprite-0 wait `$CDF9-$CE03`; exact cycle stamps from the reference core):
  mean **15,637** of 29,780.5 (52 %); typical menu frames 3-10 k; typical live-field frames 20-28 k; **972 of 5,992 frames (16 %) saturate the whole frame (≥ 29 k, the game's own logic overruns and the NES itself drops/lag frames there)**; max 29,785. So the worst case is 100 % of an NES CPU. (`docs/stats_cycles_phase.txt`)
  The NES CPU runs at a fixed 1.789773 MHz, so source cycles per second = 1,789,773 regardless of load.
* **Reads of `$2002` are timing-critical**: 2.4 M reads per 5,400 frames. Two kinds: the NMI handler's **sprite-0-hit wait** (`$CDF9-$CE03`: `BIT $2002 / BVS / BIT $2002 / BMI / BVC`) and vblank polls at start-up. After the hit the handler writes `$2005`, `$2000` and the MMC1 CHR registers (`$A000/$C000`).

### Video
* PPU registers used: `$2000` values `00 04 10-13 90-93 B0` (NMI on, BG pattern table `$1000`, sprite table `$0000`, **both 8×8 (`90..`) and 8×16 (`B0`) sprites**, nametable select, VRAM increment 1/32), `$2001` values `00 1E 40` (`1E` = BG+sprites on incl. left 8 px; `40` = rendering off with colour emphasis, 25 times: screen transitions),
  `$2003`+`$4014` OAM DMA **once per frame** (the 256 `$2004` writes per frame seen in the trace are the DMA's own writes), `$2005/$2006/$2007` for scroll and VRAM/palette updates; **no reads of `$2004` or `$2007`**.
* **Mid-frame effects exist**: a sprite-0-hit split (the CPU is busy in the NMI handler until the hit, then writes the scroll registers), **plus mid-frame CHR 4 KB bank switches** (MMC1 reg1/reg2 loads, ~1.5 per frame, written in two places in the frame: about scanline 60 and about scanlines 95-115, measured as 30 phase bins of the frame since NMI entry, so ±9 scanlines) and `$2000` writes at about scanline 113 (coarse; the exact scanline per effect still has to be measured per scene). Scroll/`$2006` writes outside vblank occur too (forced blank during screen loads: `$2001` rendering off). (`docs/stats_cycles_phase.txt`, phase table)
  Consequence: the video model must be **scanline-segmented** (render with the state that was in force at each scanline), and the sprite-0 hit must be produced at the right pixel/scanline.
* CHR is **ROM only** (128 KB = 32 × 4 KB banks, 2 bpp tiles, never written by the CPU), so every tile can be converted to Amiga bitplane format once at build time. Name tables: 2 KB of PPU RAM, vertical mirroring (fixed). Palette RAM 32 bytes, OAM 256 bytes (64 sprites, 8 per scanline limit).
* NES colour: 64-entry master palette, ≤ 25 simultaneous colours (4 BG + 4 sprite palettes, 1 shared backdrop). Output 256×240. Frames dumped: title, attract, menus, lineup, field views all render within one 32-entry palette set at a time.

### Input
* Standard controllers only: `$4016` strobe writes (`00`,`01`) and 8 serial reads each of `$4016`/`$4017` per poll (23,808 polls in 6000 frames ≈ 4 per frame, both ports read). No Zapper, no expansion port access, no `$4017` writes.

### Sound
* APU channels used: pulse 1 (`$4000-$4003`), pulse 2 (`$4004-$4007`), triangle (`$4008-$400B`), noise (`$400C-$400F`); `$4015` written with `0F` (19,397×), `07`, `0B`, `0D`, `0E` (channel enables, never bit 4).
  **DMC is never enabled** (no `$4015` write has bit 4 set; `$4010-$4013` get 69 writes, all of value `$00` to `$4010`, once every 24 frames: no samples are fetched from PRG). `$4017` is **never written** (frame counter stays at its power-up 4-step mode; with the I flag set it produces no IRQ). Reads of `$4015`: 4,751 (the music driver polls length counters).
* Sound is therefore generated by the APU hardware from register writes made by a driver in the game code (software sequencer in the NMI handler). It is **not** software-computed PCM, so the engine to model is the 2A03 APU itself (pulse duty/sweep/envelope/length, triangle linear counter, noise LFSR, the non-linear mixer; see `nes_apu.cpp:135-145` for the mixer tables MAME uses).

## 3. Timing summary (what the Amiga side has to match)

| quantity | NES NTSC | note |
|---|---|---|
| CPU clock | 1,789,772.7 Hz | 29,780.5 cycles per frame |
| frame rate | 60.0988 Hz | NMI once per frame |
| scanline | 113.667 CPU cycles (341 PPU dots / 3) | 262 lines, vblank 241-260 [driver] |
| A1200 CPU | 68EC020 @ 14.19 MHz (PAL) / 14.32 MHz (NTSC) | no fast RAM; code and data in chip RAM **[assumed from your target]** |
| ratio | ≈ 7.9 Amiga clocks per NES CPU cycle | prior port (6809, same toolkit): ~5.5 68000 clocks per source cycle (`port/DESIGN.md:113`) |

## 4. Reference emulator / what I could and could not check
* **Network**: GitHub raw/clone, PyPI and the Ubuntu archive are reachable; `github.com/.../archive/*.zip` returned 403 (so I used `git clone`). MAME **source** was fetched (driver, mmc1, ppu2c0x, apu, rp2a03).
* **Reference emulator used: Mesen (libretro/Mesen @ 0102910c), built from source** (1 min) and instrumented (`tools/discovery/mesen-trace.patch`): it is an independent, dot-accurate emulator. **MAME has not been built** (a full or `nes`-only build is a long job; I have not started it).
  Therefore every behavioural fact in §2 is "**checked against Mesen only**"; the **driver facts in §1 are from MAME source and were not run**. If you want MAME as a second oracle I will build it in the background; it does not block the next steps.
* Traces (~900 MB per run) are **not committed** (size); the committed material is the scripts, the input scripts, the patch and the summarised statistics. The raw traces can be regenerated byte-identically (Mesen is deterministic) with `tools/discovery/README.md`.

## 5. Assumptions (defaults used; change any of them and I will re-check what depends on it)
1. Target: **Amiga 1200**, 68EC020, AGA, 2 MB chip RAM, no fast RAM, Kickstart 3.1; tested in **FS-UAE** only (nothing on real hardware).
2. Both **PAL and NTSC** Amigas. Game clock at the NES rate from a timer; on PAL display frames are skipped (about 1 in 6) so game and sound speed stay 100 % (confirmed by the user, §8).
3. Controls: joystick in **port 2** = D-pad + fire = **A**, second button = **B**; **Enter = Start**; **Select = Space** (not specified; change if you like). Player 2 (the game has a 2P mode) on joystick port 1 with the same mapping; Start/Select for P2 shared with the keyboard keys.
4. "Full fidelity": exact NES palette entries (no colour reduction), all 240 lines if the Amiga mode fits (PAL: yes; NTSC: to be measured, fall back to the largest visible window, no vertical scaling); sound: full-length, lossless, highest Paula rate sustainable.
5. Reference timing = NTSC NES (the cart is an NTSC release).
6. Build toolchain on this machine: `gcc`, `make`, python3, clang available; `m68k-linux-gnu-gcc/binutils`, `qemu-user`, `fs-uae` are **not installed yet** but are available from the Ubuntu archive (`apt-cache policy` shows candidates); no Kickstart other than the one you supplied.

## 6. Approach and feasibility (decision for your information; nothing below blocks)
**Recommended: (A) static translation of the 6502 code + hardware emulation around it**, with an on-demand interpreter kept only as a verified-unused safety net (not as the main engine).
Why it works: (1) one CPU, fixed code, no code in RAM, no self-modification observed; (2) bank switching goes through 4 sites, so translated code can be keyed by (bank, address) and a bank switch is a table swap; (3) ~13 k executed instructions (≈ 36 KB of code seen, 128 KB ROM bounds it) × the 6502→68k expansion of the previous port is a few hundred KB, which fits 2 MB chip RAM; (4) the reference core and instrumentation exist and run.
Why it is not "free":
* **Indirect jumps through pointer tables** (§2) mean control-flow discovery must decode tables and use the traces as ground truth; any target not found would hit the safety net. Measure: the safety net must stay at 0 hits in all regression runs.
* **Speed risk (the one that could change the plan)**: the game uses 52 % of an NES CPU on average and **100 % on 16 % of frames**. On the previous port a source cycle cost ~5.5 68000 clocks plus PPU/APU work; 29.8 k source cycles × ~5 ≈ 150 k of the ≈ 236 k Amiga clocks per 60 Hz frame leave ~85 k for PPU rendering, sprites, APU synthesis and display. Typical frames fit; **saturated frames may run slower than real time on an A1200 without fast RAM** (the NES also lags there). I cannot confirm this before building. Mitigations in order: flag liveness and 020 peepholes (done in the toolkit), fusing the wait loops (`$CC09`, `$CDF9`: a poll can be replaced by "advance the model to the event", which also removes the most frequent instructions: 14.6 M of 52.5 M), planar tile blitting from precomputed CHR (cheaper than chunky + c2p).
* **PPU**: scanline-segmented rendering with sprite-0-hit timing; reference is Mesen (dot-accurate) at the dump level.
* **APU**: software 2A03 model mixed to Paula; no DMC needed.
* **Alternatives**: (B) interpreter/on-demand translation is the fallback if translation of the pointer-table targets proves incomplete; (C) hand re-implementation is **not** recommended (cannot be verified byte for byte).
I do not find a reason to stop: the plan is feasible, with speed on saturated frames the main unknown.

## 7. Unverified / not yet done (explicit list)
* Gameplay paths not exercised: Championship Series play, Power Analysis, password entry/decoding, 2P mode and game over/series end (coverage will be extended with more scripted runs before translation is declared complete).
* Whether the safety net is needed (control-flow closure over the pointer tables not done).
* Everything about speed on the A1200 (needs a build).
* MAME as second oracle (not built).
* PAL/NTSC Amiga display geometry for 240 lines (to be measured in FS-UAE).
* Real hardware: nothing.

## 8. Decisions (answered by the user)
1. Select = Space: accepted.
2. PAL machines: run the game clock at the NES rate (60.0988 Hz) and skip display frames at 50 Hz (no 60 Hz NTSC-timing mode).
3. Reference emulator: Mesen is enough; MAME will not be built. Behavioural claims stay "checked only against Mesen".
