# Hard disk image (A1200, FS-UAE / WinUAE / CF card)

`tools/build_hdf.sh <rom.nes> <out.hdf>` builds a bootable RDB hard disk image of the same program as the ADF (`docs/AMIGA.md`). The image is a build product: it contains the ROM and is **never committed** (`build/` is ignored).
Needs `amitools` (`pip install amitools`: `rdbtool`, `xdftool`) besides the toolchain of `build_adf.sh`. `NOBUILD=1` reuses `build/<BUILD>/llb.bin` of an earlier `build_adf.sh` run (same ROM).

## Layout

* 16,515,072 bytes = 32 cylinders x 16 heads x 63 sectors x 512 bytes (RDB geometry; plain C/H/S, works as a hard file in FS-UAE/WinUAE and when written to a CF card; the card may be larger, the rest stays unused).
* Cylinder 0: RDB. Cylinders 1-31: partition `DH0`, volume `LittleLeague`, **DOS\1 (FFS)**, 512-byte blocks, **bootable, boot priority 0**, automount. FFS is in the Kickstart 3.x ROM, so the RDB carries no filesystem block.
* Files: `S/Startup-Sequence` (one line: `LittleLeague`), `LittleLeague` (AmigaDOS hunk executable, 644 bytes, `platform-a1200/loader.s` + `tools/mkhunk.py`), `LittleLeague.dat` (the payload of the ADF: code, tables and the 256 KB ROM, 579,876 bytes; flat image linked at 0x800),
  and the empty directories `C L Libs Devs`.

## What the loader does (`platform-a1200/loader.s`)

Runs from the Startup-Sequence in user mode (position independent, checked by the build: assembled at two addresses, bytes must be equal).
1. `OpenLibrary("dos.library")`, refuses 68040/68060 (the cache handling is the 68020/68030 one, as in the bootblock).
2. `Open("PROGDIR:LittleLeague.dat")`, length by `Seek`, `AllocMem(len, MEMF_CHIP|MEMF_CLEAR)` as staging buffer, `Read`, `Close`. No fast RAM needed. Errors are printed in the CLI and returned as RETURN_FAIL (not found / unreadable / no chip RAM).
3. `Supervisor()`: SR 0x2700, INTENA/INTREQ off, VBR := 0, wait for the Blitter, DMA off, CACR clear + disabled, the relocator (the one of `boot.s`, memmove ascending or descending) is copied to 0x200 and moves the staging buffer to 0x800 and jumps there.
   From then on it is the same code path as after the bootblock (`crt0.s`); the OS is never used again and never returns.

## Use

* **FS-UAE**: `amiga_model = A1200`, `kickstart_file = <KS 3.1 A1200>`, `hard_drive_0 = llb.hdf`, no floppy. Boots directly (see `platform-a1200/test/pal_hdf.fs-uae`). Keyboard: Return = Start, Right Ctrl = A, X = B, cursor keys.
* **WinUAE**: add a hard file (`llb.hdf`), sectors 63, surfaces 16, reserved 2 and block size 512 are detected from the RDB (check "Bootable", boot priority 0); A1200 / Kickstart 3.1, 2 MB chip RAM. Not tested here.
* **Real A1200**: write the image to a CF card (`dd if=llb.hdf of=/dev/sdX bs=512`, or HDFSaver/WinImage style tools) used through the IDE port or PCMCIA-IDE adapter; Kickstart 3.1 boots the RDB partition. Not tested here.
  A different Kickstart (2.x) should work the same way but only 3.1 was checked.

## Verification

FS-UAE 3.1.66, A1200 (68EC020, KS 3.1 40.68), 2 MB chip RAM, no other memory, **no floppy**, headless under Xvfb (`platform-a1200/test/run_hdf.sh`, variant of `run.sh` for `hard_drive_0`), `FAST=1`. Kickstart is test-only and not part of the repository.

Image of the final run (ROM `Little League Baseball - Championship Series (USA).nes`): see the table in the section "Result" below.

* Boot: Kickstart finds the RDB, mounts `DH0`, the Startup-Sequence starts `LittleLeague`, the game takes over (a first run with a wrongly protected loader file showed the CLI message "file is read protected", proving the boot path up to the Startup-Sequence).
* Title screen, scene build (`-DSTOP_AT=30`, input script compiled in, picture frozen at NES frame 30 = title screen), compared with `tests/amiga/cmp_shot.py` against the reference frame of the translated runtime (`xrt_host`): **PAL 0 of 61440 pixels differ, NTSC 0 of 61440** (2x2 blocks uniform). Same for frame 100 (menu): 0 / 0.
* Keyboard (xdotool, normal build, free running): after Return the screenshot equals reference frame 45 (menu) with 0 differing pixels, after Down it equals reference frame 151 (0 differing pixels): the keyboard reaches the game through the HDF boot path (PAL; NTSC see "Result").

## Limits

* The game is still far below real time (`docs/AMIGA.md`); the HDF only changes the way it is loaded.
* The loader needs one contiguous chip RAM block of the payload size (580 KB) while the OS is running; with the stock A1200 boot this is available (2 MB chip). The loader does not stop other tasks before `Supervisor()`; interrupts are masked right there.
* 68040/68060 are refused. Real hardware (IDE timing, other Kickstart versions, accelerators with fast RAM and MMU set-ups) was not tested.
* Timestamps of the files are the build time, so the SHA-256 differs from build to build.

## Result

| item | value |
|---|---|
| image tested (title/keyboard runs, PAL and NTSC) | `build/llb.hdf`, 16,515,072 bytes, SHA-256 `2066ff04a0c60199a61992bd33f5f22c8e6cc7f7f723d00a3c9dee531ae8c749` |
| title screen (frame 30 scene build), PAL / NTSC | 0 / 0 of 61440 pixels differ |
| menu (frame 100 scene build), PAL / NTSC | 0 / 0 of 61440 pixels differ |
| keyboard Return, then Down (real build) PAL / NTSC | menu = reference frame 45, next screen = frame 151, 0 pixels differ in both |
| full `tools/build_hdf.sh` run (no NOBUILD) | payload byte-identical to the tested one; the image differs only by file timestamps |

The scene builds are `BUILD=x TESTINPUT=tools/discovery/inputs/play_watchgame.txt EXTRA=-DSTOP_AT=30 tools/build_adf.sh` followed by `BUILD=x NOBUILD=1 tools/build_hdf.sh`; the run is
`KICK=<ks31.rom> HDF=<img> FAST=1 platform-a1200/test/run_hdf.sh pal|ntsc <prefix> 10 <script>` and the comparison `tests/amiga/cmp_shot.py shot_full.png ref.fb 30 pal|ntsc`.
(The keyboard run needs about 14 s after power-on before the title is up; the HDF boot itself takes longer than the ADF.)
