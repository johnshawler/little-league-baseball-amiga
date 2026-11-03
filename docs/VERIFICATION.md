# Verification (what was run, with numbers)

| check | result |
|---|---|
| 6502 core vs SingleStepTests `nes6502` (151 official opcodes x 300 vectors, every bus cycle) | 45,300 / 45,300 |
| reference board (`ref/nesref.c`) vs instrumented Mesen: every instruction fetch (pc, value, cycle) and every PPU/APU/mapper write over a 6000-frame scripted run | IDENTICAL, 54.8 M records |
| runtime (lazy PPU + NMI queue, interpreter mode) vs Mesen, incl. all `$2002`/joypad reads and `$4015` values | IDENTICAL, 58.3 M records |
| reference PPU frames vs Mesen frames (learned NES-colour -> RGB map) | 5998 / 6000 frames pixel identical; frames 2451 and 3018 differ in 2 pixels each (a colour 0x1A pixel pair: probably a mid-frame palette effect) |
| translator, per instruction and random sequences vs the core, under qemu-m68k | 658,800 runs, 0 failures (registers, live flags, RAM, stack, cycles) |
| translated program vs Mesen (all writes, joypad reads, every frame) | 6000-frame play script IDENTICAL (frames identical to the reference emulator); coverage scripts cov2/3/4/6 (Championship Series, Power Analysis, 2P) IDENTICAL |
| APU | see docs/APU.md: channel events identical cycle by cycle; PCM bit exact through Mesen's mixer/blip |

Method notes: the translated program is compared through its observable behaviour (writes with cycle stamps, joypad reads, frames); `$2002` polls inside the two skipped wait loops are not individually emitted (verified equal by the writes/frames that follow). Block entries were additionally checked against the reference instruction trace (XL_DEBUG_BLOCKS + XCHK).
Not verified: real hardware (nothing was run on it); blue colour emphasis is not displayed on the Amiga; behaviours the game never exercises in the scripted runs (see FACTS.md section 7).
