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
