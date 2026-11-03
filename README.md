# Little League Baseball: Championship Series (NES) for the Amiga 1200

Mechanical translation (6502 -> 68020) of the original NES code plus an exact hardware model (PPU, APU, MMC1, joypads, DMA, timing), verified against Mesen.
**Status: correct but far too slow (1.4-3.3 % of real speed in FS-UAE); see docs/STATUS.md.**

* Build: `tools/build_adf.sh <rom.nes> <out.adf>` (needs m68k-linux-gnu toolchain, python3); ROM and Kickstart are never part of the repository.
* Hard disk image (RDB, bootable, FS-UAE/WinUAE/CF): `tools/build_hdf.sh <rom.nes> <out.hdf>`, see docs/HDF.md.
* Docs: FACTS.md (discovery), docs/DESIGN.md, docs/VERIFICATION.md, docs/APU.md, docs/AMIGA.md, docs/STATUS.md.
* Tests: `make -C tests/xlunit` (translator, qemu-m68k), `tests/apu_test.c`, `tests/cpu_test.c`, `tests/amiga/run_tests.sh` (FS-UAE; needs a Kickstart 3.1 ROM for testing only).
