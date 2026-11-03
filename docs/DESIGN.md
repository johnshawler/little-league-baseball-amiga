# Design (as built)

Approach (A): the 2A03 code is translated mechanically to 68020 code; the board hardware (PPU, APU, MMC1, joypads, OAM DMA, time) is modelled around it.
Nothing of the game logic is re-implemented.

## Layers (all target independent except `platform-a1200/`)
| layer | files | what it is |
|---|---|---|
| CPU reference | `runtime/cpu6502.c` | bus-cycle exact 2A03 core (documented opcodes); checked against 45,300 SingleStepTests vectors (every bus cycle) |
| PPU reference | `runtime/ppu_ref.c` (`ppu_step`) | per-dot 2C02 model following the instrumented reference emulator's rules (pipeline, sprite evaluation, sprite-0 hit, scroll copies, odd-frame skip) |
| PPU fast path | `ppu_sync_fast`, `render_line_fast`, `reconstruct` | whole idle visible lines are drawn at once from the same state; the pipeline state at the end of the line is rebuilt so the result is identical to the per-dot model (`rt_fast_ppu`) |
| board / time | `runtime/rt_core.c` | access in CPU cycle c sees 3c+4 PPU dots; the NMI line is sampled at the end of each cycle (queued level transitions reproduce this lazily); OAM DMA; joypads; MMC1; APU hooks |
| APU | `runtime/apu.c`, `docs/APU.md` | event-driven, cycle exact status, exact mixer integration to any output rate |
| translator | `translator/x65.py` | static 6502->68020 per superblock with flag liveness |
| glue | `runtime/rt_x.c`, `rt_x.s` | helpers with exact access cycles, slow path (interpreter) and cycle limit |

## Translation conventions
d0=A d1=X d2=Y d3=S (X,Y,S zero extended) d4=C (0x00/0xFF) d5=cycle budget (limit - now) d6,d7 temps; a0=RAM a1=PRG window base (bank switch = pointer reload) a2=context
(canonical N,Z in P layout, V, I/D) a3/a4 dispatch tables (per bank window / fixed) a5=N,Z table a6=fixed PRG base.
* N/Z are tracked as "result of register R" or "in the CCR" and only materialised when live across a branch chain/block end (liveness over the whole translated program, calls and returns conservative).
* Every superblock starts with `sub.l #cycles,d5; bmi slow`. The cycle limit is the earliest cycle at which an NMI could be taken, a frame event happens, or a DMA is pending, so a block that fits can never be interrupted; the slow path runs the interpreter (exact bus timing, NMI at the same instruction boundary) until it reaches a translated entry again.
* Page-crossing penalties and taken branches are charged dynamically; the entry check reserves the possible penalties.
* JSR/RTS use the emulated stack only (the game manipulates return addresses: inline data after `JSR $C1E5`); RTS/RTI/JMP(ind) dispatch through bank-aware tables. Unseen code falls back to the interpreter (`rt_unk`).
* Loops that only poll `$2002` (sprite-0 wait in the NMI handler) are skipped analytically to the cycle where the flag can change (`ppu_predict_hit`), exactly equivalent.
