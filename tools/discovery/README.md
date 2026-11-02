# Discovery tooling (how the numbers in FACTS.md were produced)

1. `git clone https://github.com/libretro/Mesen` (commit 0102910c), `git apply tools/discovery/mesen-trace.patch` (adds an env-var-controlled trace in `Core/MemoryManager.cpp`), `make -C Libretro`.
2. `gcc -O1 -o host tools/discovery/host.c -ldl` (copy `libretro.h` from the Mesen tree next to it). Run:
   `NESTRACE=run.trc AUD=run.pcm ./host mesen_libretro.so game.nes 6000 tools/discovery/inputs/play_watchgame.txt`
   (input script lines are `frame hexmask`, mask bits = libretro joypad ids: B=1, Select=4, Start=8, Up=0x10, Down=0x20, Left=0x40, Right=0x80, A=0x100).
3. `an2.py` (executed code, mapper writes), `an3.py` (hot spots, register counts), `an4.py`/`an5.py` (indirect jumps, RTS dispatch), `an6.c` (busy cycles per frame, write phase histogram),
   `filt.c` + `an8.py` (I/O value histograms, MMC1 serial decode), `uniq.c` + `an7.py` (static closure), `dis.py` (6502 disassembler).
The trace record layout is 16 bytes: char type (X/R/W), u8 value, u16 address, i32 PRG offset (-1 outside ROM), u32 CPU cycle, u32 frame.
ROMs, Kickstart and raw traces are not in the repository (`.gitignore`).
