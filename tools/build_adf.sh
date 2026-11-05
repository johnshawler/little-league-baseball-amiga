#!/bin/sh
# Build the bootable A1200 disk image:  tools/build_adf.sh <rom.nes> <out.adf>
#   EXTRA="-DTELEM"            speed telemetry in the bottom picture lines (test builds)
#   EXTRA="-DPROFILE"          PC-sampling profile page (test builds)
#   TESTINPUT=script.txt       controller driven by a frame script ("frame hexmask" lines, libretro bits as tools/rt_host.c)
#   EXTRA="-DSTOP_AT=N"        show the picture of frame N (hook index) and stop (pixel comparison tests)
#   DISPLAY_TEST=frames.bin    alternate two stored frames on the display (pacing / conversion cost test)
#   AUDIO_TEST=file.pcm        replay a PCM file through the audio module (no emulation)
#   SNAPSHOT=file.snap         start from a runtime snapshot made by tools/snap.c (test builds)
#   ARENDER_HW=1               picture from the planar renderer (runtime/arender.c) instead of chunky fb + c2p (needs the default PPU_FB5)
#   FASTRAM=0                  old layout: everything in chip RAM, no fast RAM needed (default 1: 8 MB fast RAM at 0x200000 is required)
#   BUILD=name                 build directory build/<name> (default amiga)
# The ROM (iNES, mapper 1, 128 KB PRG + 128 KB CHR) is only read; it is copied into the disk image, never into the repository.
set -e
ROM="$1"; OUT="$2"
[ -f "$ROM" ] && [ -n "$OUT" ] || { echo "usage: $0 <rom.nes> <out.adf>"; exit 1; }
cd "$(dirname "$0")/.."
ROOT=$(pwd); B=${BUILD:-amiga}; B=$ROOT/build/$B; mkdir -p "$B"
P=$ROOT/platform-a1200; R=$ROOT/runtime
PREFIX=${PREFIX:-m68k-linux-gnu-}
CC=${PREFIX}gcc; AS="${PREFIX}as -m68020 --register-prefix-optional"; LD="${PREFIX}ld -z noexecstack --no-warn-rwx-segments"
if [ "$ARENDER_HW" = 1 ]; then EXTRA="$EXTRA -DARENDER_HW"; fi    # ARENDER_HW=1: planar picture from runtime/arender.c, Copper list per picture (platform-a1200/display_ar.c); default 0: chunky framebuffer + c2p
FASTRAM=${FASTRAM:-1}      # 1 (default): the program runs from the 8 MB fast RAM at 0x200000 (platform-a1200/link_fast.ld); 0: everything in chip RAM (link.ld, old layout)
if [ "$FASTRAM" = 1 ]; then LDS=link_fast.ld; ASF="-Wa,--defsym,FASTRAM=1"; PBASE=0x200000; else LDS=link.ld; ASF=""; PBASE=0x800; fi
FBFLAG=${FBFLAG:--DPPU_FB5}     # -DPPU_FB5: 5 bitplanes, framebuffer = palette RAM index (default); -DPPU_FB8: 8 planes, framebuffer = NES colour | emphasis
AS5=""; case "$FBFLAG" in *FB5*) AS5="-Wa,--defsym,P5=1";; esac
CFLAGS="-m68020 -ffreestanding -nostdlib -fno-pic -O2 -fomit-frame-pointer -fno-tree-loop-distribute-patterns -fno-delete-null-pointer-checks -Wall -Wno-unused -Wno-misleading-indentation -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 $EXTRA -DAMIGA $FBFLAG -DRT_AUDIO=1 -DAPU_Q_SQ=512 -DAPU_Q_TRI=4096 -DAPU_Q_NOI=8192"
RTFLAGS="$CFLAGS -I$R"
python3 - "$ROM" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'NES\x1a' and d[4] == 8 and d[5] == 16 and ((d[6] >> 4) | (d[7] & 0xF0)) == 1 and not d[6] & 4, 'need an iNES mapper 1 image with 128 KB PRG, 128 KB CHR, no trainer'
PY
if [ -n "$TESTINPUT" ]; then   # test builds: input script file (lines "frame hexmask") compiled in; also add -DTESTINPUT to EXTRA
  python3 -c "
import sys
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) == 2: print('{ %d, 0x%s },' % (int(p[0]), p[1]))
" "$TESTINPUT" > $B/testinput.inc
  EXTRA="$EXTRA -DTESTINPUT"; CFLAGS="$CFLAGS -DTESTINPUT"; RTFLAGS="$RTFLAGS -DTESTINPUT"
fi
SNAPOBJ=""
if [ -n "$SNAPSHOT" ]; then    # test builds: start from a runtime snapshot (tools/snap.c)
  printf '\t.section .rodata\n\t.even\n\t.globl snap_data\nsnap_data:\n\t.incbin "%s"\n' "$SNAPSHOT" > $B/snap.s
  $AS $B/snap.s -o $B/snap.o; SNAPOBJ=$B/snap.o
  CFLAGS="$CFLAGS -DSNAPSHOT"; RTFLAGS="$RTFLAGS -DSNAPSHOT"
fi
if [ -n "$AUDIO_TEST" ]; then  # test build: replay a PCM file (tools/rt_host.c PCMOUT) through the audio module
  printf '\t.section .rodata\n\t.even\n\t.globl audio_test_pcm, audio_test_pcm_end\naudio_test_pcm:\n\t.incbin "%s"\naudio_test_pcm_end:\n' "$AUDIO_TEST" > $B/atest.s
  $AS $B/atest.s -o $B/atest.o; SNAPOBJ="$SNAPOBJ $B/atest.o"; CFLAGS="$CFLAGS -DAUDIO_TEST"
fi
if [ -n "$DISPLAY_TEST" ]; then  # test build: alternate two stored frames (2 x 61440 bytes) on the display, no emulation
  printf '\t.section .rodata\n\t.even\n\t.globl dtest_frames\ndtest_frames:\n\t.incbin "%s"\n' "$DISPLAY_TEST" > $B/dtest.s
  $AS $B/dtest.s -o $B/dtest.o; SNAPOBJ="$SNAPOBJ $B/dtest.o"; CFLAGS="$CFLAGS -DDISPLAY_TEST"
fi
# 1. translated program + dispatch tables (sparse), palette
python3 translator/x65.py "$ROM" translator/seen_abs.txt $B/code.s $B/tab.s $B/report.txt
python3 tools/gen_tabsp.py $B/tab.s $B/tabsp.s
python3 tools/gen_palette.py $B/palette.inc
$AS $B/code.s -o $B/code.o; $AS $B/tabsp.s -o $B/tabsp.o; $AS $R/rt_x.s -o $B/rt_x_s.o
# 2. runtime
for f in rt_core rt_x cpu6502 ppu_ref arender mapper_mmc1 apu; do $CC $RTFLAGS -c $R/$f.c -o $B/$f.o; done
# 3. platform
DISP=display; C2PO=$B/c2p.o; if [ "$ARENDER_HW" = 1 ]; then DISP=display_ar; C2PO=""; fi
for f in main $DISP audio platform telem libc tabs palette; do $CC $CFLAGS -I$P -I$B -I$R -c $P/$f.c -o $B/$f.o; done
$CC -m68020 $AS5 -x assembler -c $P/c2p.s -o $B/c2p.o; $CC -m68020 -x assembler -c $P/crt0.s -o $B/crt0.o
$CC -m68020 -x assembler -c $P/isr.s -o $B/isr.o; $CC -m68020 -x assembler -c $P/rom.s -o $B/rom.o
$CC -m68020 $ASF -x assembler -c $P/boot.s -o $B/boot_o.o
$LD -e 0 -Ttext=0 --oformat=binary -o $B/boot.bin $B/boot_o.o
[ $(stat -c %s $B/boot.bin) -le 1024 ] || { echo "bootblock > 1024 bytes"; exit 1; }
# 4. link
LIBGCC=$($CC -m68020 -print-libgcc-file-name)
$LD -T $P/$LDS -Map=$B/llb.map -o $B/llb.elf $B/crt0.o $B/code.o $B/rt_x_s.o $B/rt_x.o $B/rt_core.o $B/ppu_ref.o $B/arender.o $B/cpu6502.o $B/mapper_mmc1.o $B/apu.o \
   $C2PO $B/$DISP.o $B/main.o $B/audio.o $B/platform.o $B/telem.o $B/isr.o $B/libc.o $B/tabs.o $B/palette.o $B/tabsp.o $B/rom.o $SNAPOBJ $LIBGCC
${PREFIX}objcopy -O binary $B/llb.elf $B/llb.bin
# 5. put the ROM (PRG then CHR, without the iNES header) into the .rom placeholder and build the disk
python3 - "$ROM" $B/llb.elf $B/llb.bin $PBASE <<'PY'
import subprocess, sys
rom, elf, binf, pbase = sys.argv[1:5]; pbase = int(pbase, 0)
nm = subprocess.check_output(['m68k-linux-gnu-nm', elf]).decode()
off = [int(l.split()[0], 16) for l in nm.split('\n') if l.endswith(' rom_start')][0] - pbase
d = open(rom, 'rb').read()[16:16 + 262144]; assert len(d) == 262144
b = bytearray(open(binf, 'rb').read()); assert b[off:off + 262144] == bytes(262144), 'ROM placeholder not at the expected place'
b[off:off + 262144] = d
open(binf, 'wb').write(b)
PY
python3 tools/mkdisk.py "$OUT" $B/boot.bin $B/llb.bin
${PREFIX}size $B/llb.elf
