# Status, limits and remaining work (end of this session)

**Delivered and verified:** reference emulator, translator, runtime, APU, Amiga 1200 boot/display/input/audio layer, ADF builder (`tools/build_adf.sh`). Correctness is verified against Mesen (docs/VERIFICATION.md, docs/APU.md, docs/AMIGA.md).
**Target not met: speed.** In FS-UAE with real A1200 timing (no fast RAM) the game runs at **1.4 - 3.3 % of an NES** (menu 3.3 %, line-up 1.4 %, field 2.0 %; docs/AMIGA.md section 12). It is not playable. Nothing was run on real hardware.

Test ADF built here (contains the user's ROM, not committed): `build/llb_a1200.adf`, SHA-256 47354f4d8bcc9b7afc36060ab515c5e9b9c98fc393ea7f9cbcadc9d69f309801 (built from commit 1609d47; rebuild with `tools/build_adf.sh`).

## Why 100 % is probably out of reach on this machine (evidence, not proof)
In the field scene the translated 6502 code alone costs ~25 ms per NES frame (frame period 16.6 ms) in FS-UAE's A1200 timing, i.e. about 14 68020 clocks per 6502 cycle instead of the expected 5-7 (everything runs from chip RAM with 8-plane display DMA and a 256-byte instruction cache; the code is 252 KB). The I/O helper chain adds ~80 ms, PPU model ~340-800 ms, APU 24-40 ms, display 0.6-20 ms. Even with the PPU, helpers, APU and display all free, the remaining translated code would run at ~65 % in the busiest scenes (the game itself uses 100 % of an NES CPU on ~16 % of frames). FS-UAE's 68020 timing is approximate; a real A1200 may differ, but no measurement exists.

## Remaining improvements (expected gain / risk / cost)
| item | expected gain | risk | cost |
|---|---|---|---|
| Skip the remaining `$2002` polling loops analytically and make mid-line register accesses lazy (no per-dot stepping) | 2-3x on line-up/menu scenes (PPU+rt_core are 35-60 % there) | low (verified by the existing trace/frame comparators) | medium |
| Direct tile-based planar renderer (5 bitplanes, CHR pre-converted at build time, palette RAM loaded as colours, no chunky buffer, no c2p, less display DMA) | removes ~70 ms full-frame c2p, most of render_line_fast (24-32 %), frees chip-RAM bandwidth for the CPU (5 instead of 8 planes) | high: mid-frame CHR/scroll splits and sprite priority/sprite-0 need exactness | large |
| APU renderer in 68020 assembler / quantised events (24-40 ms -> <3 ms) | 5-10 % of a frame at 100 % target | low-medium (PCM comparison exists) | medium |
| I/O helper chain (per-access limit recomputation, NMI queue, helper call overhead 700-2200 cycles) | ~80 ms -> <20 ms in the field scene | low | medium |
| Translated code: fewer budget updates and dispatches, smaller code (252 KB vs 256 B I-cache), use of the free 445 KB for a faster layout, register allocation of hot zero-page variables | 1.3-2x on 25 ms | medium | large |
| Paula-channel audio instead of software mixing (approximates the APU, loses exactness) | removes APU cost | changes sound fidelity: needs the user's agreement | medium |
| Accelerated/fast-RAM A1200 target | would change the picture entirely | out of the stated target | n/a |
Also open: blue colour emphasis not shown; 2 pixels of 183.9 M differ from Mesen (frame 2451/3018); audio of the running game never listened to at speed; unseen game paths fall back to the interpreter (correct but slow); no real-hardware test.
