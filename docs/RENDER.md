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
* `AR_BG`: lines y0..y1 show bitmap rows `row0 .. row0+(y1-y0)` of background bitmap `cfg` (`AR_BMP(cfg,plane,row)`, 4 planes, rows of `AR_BROW`=72 bytes: 64
  bytes = 512 pixels plus 8 guard bytes so a 256 pixel window starting at byte `bytex` (0..63) never wraps).  Plane pointer = `AR_BMP(cfg,plane,row0) + bytex`,
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
