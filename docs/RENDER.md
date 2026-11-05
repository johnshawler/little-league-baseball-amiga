# Amiga-style rendering (arender)

Goal: stop emulating the PPU pixel pipeline into a chunky frame buffer followed by chunky-to-planar conversion.  The PPU model (`ppu_ref.c`) stays the exact
state/timing model; on displayed frames it records a **scene log**, and `runtime/arender.c` turns log + VRAM + OAM + CHR into a planar picture.
Everything stays proven against the per-dot reference: `ar_expand()` of the planar result must equal the reference frame buffer (PPU_FB5 palette index per
pixel) for every frame of the 6000-frame play script and cov1-8 (checked in the `PPU_CHECK` harness).

## Scene log (see `runtime/arender.h`)
* **Region**: a run of whole lines `[y0,y1]` with identical rendering parameters (fine X, BG/sprite pattern table, 8x16, CHR bank pair, mirroring, mask
  enables / left clip, palette RAM) and the normal scroll progression (`vl` of the first line; later lines: increment Y, copy `tx` horizontal bits).
  A frame normally has 3-6 regions (e.g. field / status panel split at the sprite-0 line).
* **Exact line**: a line the model cannot describe as a plain region (a register access, i.e. a mid-line parameter change, landed inside it or in the line
  before: tile and sprite fetches then used different parameters in the two halves).  Those rows are produced by the exact chunky renderer and converted;
  typically 0-12 lines per frame.  Nothing is approximated: this is what keeps stage 1 exact for arbitrary register timing.
* Palette RAM writes inside a region split it (the palette snapshot is part of the region), so mid-frame palette effects are exact ("palvar" frames).
* OAM is snapshotted at frame start; a write during the visible part marks the whole frame exact.

## Background: persistent planar bitmaps
Per configuration (CHR bank pair, BG pattern table, mirroring) one 4-plane bitmap of the 4 logical nametables (512 x 512 px; 4 planes: pattern bit 0/1,
palette bit 0/1 (attribute)), built from CHR tiles converted once to planar form (per 4 KB bank 256 tiles x 2 planes x 8 rows).  Updated incrementally from
the nametable row write counters (`Ppu.ntv[]`): only rows whose counter changed are rebuilt (an attribute row rebuilds the 4 tile rows it covers).  A few
configurations are cached (8; 128 KB each in fast RAM).  Scrolling costs nothing: line y of a region reads bitmap row `ynt*256 + coarseY*8 + fineY`
starting at pixel `xnt*256 + coarseX*8 + fx`, 256 pixels, wrapping at 512.  On the Amiga the display pointers point straight into the bitmap (byte offset =
coarse X, fine X = BPLCON1, Copper list per region).  Bitmaps the display reads must live in chip RAM (2 MB total, 8 configs = 1 MB: budget in stage 2).
Colour 0 of each palette is the backdrop: palette indices 4, 8, 12 are loaded with palette RAM entry 0 in the colour registers, so a BG pixel with pattern
value 0 needs no special case.

## Sprites
Per output line: evaluation exactly as the PPU (first 8 sprites in OAM order whose Y range covers the evaluation line; the 9th and later are not drawn),
patterns from CHR with flips / 8x16, composed on the line's planes with row-wise mask operations: opaque mask from the two pattern bytes, first opaque
sprite claims the pixel, behind-BG sprites only where the BG pixel value is 0, left-8 clip bit, right edge clipping.  Sprite pixels write plane 4 = 1 and
the sprite palette index (16 + 4*pal + value) into planes 0..3.

## Output
5 planes (index = plane4<<4 | plane3<<3 | ... | plane0), 240 rows of 32 bytes per plane, plus per region the 32-entry palette state (palette RAM, gray,
emphasis).  Stage 2: Copper list per region.

## Stages
1. (host, exact) scene log emission in the PPU model, `arender.c`, comparison in `PPU_CHECK`.
2. (Amiga) display pointers into the persistent bitmaps, fine X by BPLCON1, sprites drawn with save/restore or dual playfield, skipping work on frames that
   are not displayed.

## Integration (platform layer)
* Set `ppu_ar_enable = 1` (e.g. in main() before `xrt_run`); `ar_init()` is called by `rt_init`.  Build: `runtime/arender.c` is part of the runtime
  (added to `tools/build_xrt.sh`, `build_snap.sh`, `build_adf.sh`; PPU_FB5 builds only).
* On every frame with `nes.ppu.draw != 0` the model logs the scene and, when the frame is complete (before `rt.frame_hook`), `ar_end_frame()` fills `ar_out`:
  `ar_out.plane[0..4]` (240 rows of 32 bytes each, palette index bits 0..4 of every pixel) and `ar_out.reg[0..nreg-1]` (first/last line and the 32 palette RAM
  entries, gray/emphasis of every region: load the colour registers at `y0`).  `nes.ppu.fb` then only holds the exact rows; do NOT convert it any more.
* The planes are exactly what `display_frame` used to produce from `fb` with c2p, so the first integration step is a plain copy of the 5 planes (38 KB)
  into the display buffer plus the per-region colour changes; later steps point the display at persistent bitmaps (stage 2).

## Stage 2b: hardware format (agreed interface, to be implemented in arender.c / display_ar.c)
No per-line shifting of background rows.  `ar_end_frame` produces regions of three kinds (`ArRegion.kind`):
* `AR_BG`: lines y0..y1 show bitmap rows `row0 .. row0+(y1-y0)` of background bitmap `cfg` (`AR_BMP(cfg,plane,row)`, 4 planes, rows of `AR_BROW`=96 bytes: 64
  bytes = 512 pixels plus 32 guard bytes = copy of bytes 0..31, so the 17-word window starting at byte `bytex` (0..63) never wraps).  Plane pointer = `AR_BMP(cfg,plane,row0) + bytex`,
  rounded down to the fetch granularity of the platform's FMODE, the remainder goes into the horizontal scroll: scroll in pixels = (bytex mod g)*8 + fx
  (g = pointer granularity in bytes).  Display modulo = AR_BROW - width fetched.  Regions are split where the row sequence jumps (Y wrap) and where the first
  line's x differs from the following lines.
* `AR_EXACT`: lines already composed (exact lines, clipped / background-disabled lines): rows y0..y1 of `ar_hw.xpf1` (background playfield, planes 0..3, scroll 0) and
  `ar_hw.xpf2` (sprite playfield).
* `AR_BLANK`: backdrop only (blank_idx).
Playfield 1 = odd bitplanes = background value 0..15 (colour registers 0..15, registers 4, 8, 12 = copy of register 0); playfield 2 = even bitplanes = sprite value
v = 4*palette + pattern (0 transparent; colour registers 16..31).  Sprites live in `ar_hw.spr` (4 planes x 240 rows x 32 bytes, screen aligned) and are composed per
line with the exact NES rules (8 per line, priority, claim, flips, clip); a sprite behind the background is masked against the background pixel (read from the
bitmap at the scrolled position) while composing.  The layer is cleared incrementally (only rows written in the previous frame).  Per-region colour state as before
(`pal[32]`, gray, emph); every line is covered by exactly one region.
`ar_expand_hw()` expands exactly this display model (bitmap window + sprite layer, playfield 2 over playfield 1) to palette indices; the host check compares it with the reference.

### Platform notes for the stage 2b format (display_ar.c; measured in FS-UAE)
* **DMA cost with fast RAM** (field scene, chunky path, same snapshot, ticks 2339..12023): 5 planes FMODE 1 22.85 %, **8 planes FMODE 1 22.79 %**, 8 planes FMODE 0 22.31 %: with the program in fast RAM the display DMA costs nothing measurable, so the 4+4 dual playfield above is the cheaper exact format
  (only CPU accesses to chip RAM - sprite layer, Copper lists, bitmap updates - are slowed).
* Mode: BPLCON0 0x0611 (8 planes, DBLPF), BPLCON2 PF2PRI (0x0040: sprites in front), BPLCON3 PF2OF = 100 (playfield 2 colours start at register 16): sprite value u (1..15) -> register 16+u = palette RAM entry 16+u; background value v -> register v with 4, 8, 12 = entry 0.
* Fetch/scroll: FMODE 0 (16-bit fetch, g = 2) needs one extra word at the left (DDFSTRT 0x40 / DDFSTOP 0xC0, 17 words) and a 4-bit scroll: with N = bytex*8 + fx: r = N & 15; r = 0: pointer word (N>>4)-1, scroll 0; else pointer word N>>4, scroll 16-r (BPLCON1 low nibble = playfield 1, sprite layer unscrolled with 2 pad bytes).
  **Settled (verified in FS-UAE, tests/amiga/arhw_expect.py): AR_BROW must be 96, not 72.** The window reads 17 words = 34 bytes from word w0 (0..31): bytes 2*w0 .. 2*w0+33, up to byte 95.
  So every bitmap row has 64 bytes + **32 guard bytes = copy of bytes 0..31** (stride 96; 4 configurations x 4 planes x 512 rows x 96 = 786 KB chip RAM).  No pad in front of the row is needed: for N = 0
  the platform uses word 31 with scroll 0 (the guard bytes continue with words 0..).  The sprite layer / xpf1 / xpf2 (32-byte rows) need **2 bytes of slack in front of the first row of each block**
  (pointer = row - 2, the hidden pad word): allocate 2 spare bytes before ar_hw.spr / xpf1 / xpf2 (they are only fetched, never displayed).
* Palette switches that need more MOVEs than fit into the blank (about 22-26 MOVEs after the last fetch of the previous line): `display_ar2.c` writes only changed registers (hi/lo pass), no pointer MOVEs for a region that continues (a palette-only split costs just the colours), and
  with the optional `used` masks (`Ar2Mem.used[i]`, 32-bit palette-index mask of what region i shows; NULL = all) skips colours the region does not show and writes colours that the previous region does not show one line earlier.  **Request: `ArRegion.used`** (bit e = palette RAM entry e occurs in the region's output pixels, entry 0 for backdrop, 16+u for sprite value u).  Without it a full 32-colour change (emphasis flash) shows its first 1-3 lines late.

### Stage 2b: what arender produces (implemented, runtime/arender.c, `ar_hw_enable=1`; host check `ARENDER=2`)
* Memory (`ar_hw_setmem(chip)`, `AR_HW_BYTES`): `ar_hw.bmp` (4 configs x 4 planes x 512 rows x `AR_BROW`), then two sets of three blocks (sprite layer, xpf1, xpf2), each block
  `AR_LAYB` = 4 spare bytes + 4 planes x 240 rows x 32 bytes; the pointers `ar_hw.spr/xpf1/xpf2` point behind the spare bytes.  The Ar2Mem the Copper builder needs is
  {ar_hw.bmp, ar_hw.spr, ar_hw.xpf1, ar_hw.xpf2, AR_BROW, used[]}.
* Double buffering: the platform calls `ar_hw_shown()` after it switched the display to the picture of the last `ar_end_frame`; the next frame is composed into the other set.
  A frame that is not shown keeps the set.  The persistent bitmaps are updated in place (tile rows are rebuilt when the name table / attribute rows changed), so a bitmap row
  that changes while the previous picture is still displayed shows the new content one frame early (only for rows that change in the frame, i.e. at the edge of a scroll).
* Sprite layer: cleared incrementally (rows written two shown frames ago); the background left clip / disabled background is a cover (value 4 = backdrop) in the layer.
* `ArRegion.used` (32-bit mask of palette RAM entries that can occur; entry 0 = backdrop, 16+u sprite value u) for AR_BG regions: background part from per tile row masks built when
  the bitmap row is built, sprite part from the sprites drawn into the region's lines.  AR_EXACT / AR_BLANK: all bits.  The host check verifies that no pixel shows an entry outside `used`.
* A name table byte written while a frame is displayed (`ar_flush_nt`) bakes the background regions shown so far into xpf1/xpf2 (they point to bitmaps that are about to change).
* `ar_hw_palette(region, out[32])` = palette entry each colour register must show.
* Lines are plain regions even when a $2002 poll (sprite-0 wait) splits the line into pieces: the model decides at the end of the visible dots whether a PPU register / mapper /
  OAM write landed in the line (`Ppu.ar_wr`); exact rows dropped from about 40 to 1.8 per frame in the 6000 frame test.

### Stage 2b: arender cost reduction (round 2; FS-UAE PAL, fast RAM, profile page, ms per game frame summed over the arender functions + ar2_build)
Before = 0724ded with tile-granular rebuild only partially in place (profile `t1`), after = this round (`t9`).  Scenes: snapshots at frame 100 (menu), 1200 (line-up), 2000 (field).

| | menu | line-up | field |
|---|---|---|---|
| arender before (ms/frame) | 4.1 | 13.7 | 12.1 |
| arender after (ms/frame) | 1.8 | 5.4 | 6.6 |

What changed (all exact: `ARENDER=2 PPU_CHECK=1` host, translated qemu runs, DRAWPAT 1,3,4,5,6,11):
* `ensure_trow`: per tile row a copy of the 32 tile bytes + 8 attribute bytes it was built from; only the 4-tile groups that differ are looked at, only tiles whose (tile, palette) differ are redrawn, and only the bitmap bytes that differ from the old tile are written (chip RAM writes cost about 10 cycles each).  Rows that are new or have >= 3 changed groups are rewritten with long words from fast RAM buffers.  The palette-entry mask of a row is exact for new / bulk rows and a superset (OR of the new tiles) for small updates.
* Sprite layer: composed per line in a fast RAM line buffer with 16 bit window operations; the long words that differ from a fast RAM copy of each layer set are the only ones written to chip RAM (no clearing pass).  A line whose inputs (sprites in range with their OAM bytes, pattern banks, clip, and for sprites behind the background the background window and the bitmap generation) equal those of the last composition into the same set is not touched at all.
* Exact rows: chunky -> playfield encoding with 1024 entry tables per pixel pair (about 6x faster than the bit loops).
* `ar_log_region`: the vl of the line after the last region is kept incrementally (was quadratic); `ar_out.reg` is written directly (no per-frame region copy); long word `ar_set/ar_cpy/ar_cmp`.
* `hw_region`: the split into row-contiguous segments is closed form (no per line `incy1` loop; `-DAR_VERIFY` checks it against the loop).
* Remaining arender cost: sprite composition of lines with moving sprites (`hw_line`), chip RAM writes of scrolled-in tiles (`ensure_trow`), and the encoding of long runs of exact rows.

### Time model / PPU model round (FS-UAE PAL, fast RAM, profile page, same window of 700 ticks after the snapshots at frames 100 / 1200 / 2000)
Fresh profile (object level, ms per game frame) at 59ba1a1: menu ppu_ref 11.4, rt_core 3.7, arender 1.7; line-up ppu_ref 8.5, arender 5.6, rt_core 5.0, rt_x 3.0; field arender 7.0, rt_core 6.1, ppu_ref 4.5, rt_x (c+s) 6.6, mapper 1.3.
Where the time was (found with line level profiles of a `-g` build, `addr2line` on the 32 byte sample bins):
* menu: one loop of `ppu_fp_run` took 14 % of all samples (3.6 ms per frame): the batch path searched the next line with 8 or more sprites by scanning `lt_cnt[]` over the whole batch (up to 240 lines) every time a batch started and the overflow flag was still clear (always, in the menu).  Now `lt_n8[]` (next line with 8 or more sprites, built with the line table) is followed instead.
* OAM DMA block copy: the byte/long mix loop spilled registers in the huge `bus_rd` frame (0.6 ms per frame); now a small noinline long word routine with the attribute mask.
* field / line-up: 100-130 reads per frame through `(zp),y` into ROM / RAM and the RAM stores through `(zp),y` took the C helper chain (`xrt_rd_c` -> `rt_io_rd` -> `mem_read`, about 250 cycles each).  `rt_rd` / `rt_wr` in `rt_x.s` now handle RAM and ROM (window / fixed bank) in assembler (about 20 cycles); PPU/APU/mapper accesses are unchanged.
* diagnostics for the next round: `-DPPU_COUNT` + `XRT_STATS=1` print C helper accesses by register and 256 byte page, fp_piece / ppu_fp_run / hit_find / first-cycle / batch counts, skip calls, bus_rd calls, DMA block copies.  Per frame the translated program makes only 45-110 `fp_piece` calls, 35-90 `ppu_fp_run` calls, 1-20 `hit_find` calls, about 50 `bus_rd` calls (12 interpreted instructions) and 150-200 accesses through the C helpers; the 5-7 ms per frame of `ppu_ref.o` in the menu / line-up are therefore about 1500 cycles per piece, not a large number of calls.

| game frames in the same 700 tick window | menu | line-up | field |
|---|---|---|---|
| 59ba1a1 | 450 | 333 | 257 |
| this round | 552 | 365 | 297 |
| ms per game frame (window / frames) | 25.9 -> 21.1 | 35.0 -> 31.9 | 45.3 -> 39.2 |
| ppu_ref.o / rt_core.o / rt_x(+_s).o (ms per frame) | 11.4->7.2 / 3.7->3.2 / 1.7->1.6 | 8.5->7.6 / 5.0->3.5 / 3.0->2.6 | 4.5->3.9 / 6.1->3.5 / 6.6->4.9 |
| arender.o + display_ar2.o (ms per game frame, unchanged code) | 1.6 -> 1.7 | 5.9 -> 5.9 | 7.0 -> 6.6 |
The arender figure is per game frame; it only runs on drawn frames (every 3rd-4th), i.e. 15-20 ms per drawn frame in the line-up / field.
