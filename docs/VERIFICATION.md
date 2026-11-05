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

## qemu-m68k layout dependence of the translated host build (investigated)
Symptom: the translated program under `qemu-m68k` (8.2.2) diverges from the reference trace by 1-159 cycles for about 3-5 % of the possible code positions inside a 4 KB page
(`tools/xrt_layout_check.sh` links the translated code at every 32/64 byte offset: 3 of 64 layouts diverge, e.g. at frame 4, 948 and 2650); it vanishes with `qemu-m68k -singlestep`.
Cause (found by bisecting the position with inserted nops and by logging translation blocks with `-d nochain,exec,cpu -dfilter`): qemu ends a translation block about 30 bytes before a
page end; when the end falls between a flag-setting instruction and the Bcc that uses it, and the flags were produced through qemu's lazily evaluated paths, the branch can be decided
wrongly.  Seen with `move.w ccr,d7 ... btst #1,(a2) ; bne` (translated code), in libgcc's `__udivdi3` (`mulul` + `cmpl` + `bhi`, used for the 64-bit cycle arithmetic) and in loop branches after `addq.b`.
A 7 instruction reproduction that needs no ROM is `tools/qemu_tb_split_repro.s` (prints 89 instead of 232 for some positions, correct with `-singlestep`).
The generated code only relies on architectural flag semantics (BTST changes only Z; CMP/ADDQ/TST set N,Z,V,C), so real 68020 / UAE are not affected; the FS-UAE scene and keyboard tests are the arbiter for the Amiga build.
Consequences: run the qemu comparators (`cx.sh`-style trace comparison, `tests/amiga/run_tests.sh` reference frames) with `QEMU_ARGS=-singlestep` when a layout-dependent difference shows up
(about 8 times slower: 2700 frames in 85 s), and use `QEMU_ARGS=-singlestep tools/xrt_layout_check.sh <rom> <p8.trc> <p8.txt> 2700 256` as the regression over layouts.
(Rewriting the BTST of the canonical flag byte into `move.b (a2),d7 ; btst #1,d7` removes the first of these cases but not the others, so it was not adopted.)
