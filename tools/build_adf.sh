#!/bin/sh
# Build the bootable A1200 disk image:  tools/build_adf.sh <rom.nes> <out.adf>
#   EXTRA="-DTELEM"            speed telemetry in the bottom picture lines (test builds)
#   EXTRA="-DPROFILE"          PC-sampling profile page (test builds)
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
CFLAGS="-m68020 -ffreestanding -nostdlib -fno-pic -O2 -fomit-frame-pointer -fno-tree-loop-distribute-patterns -fno-delete-null-pointer-checks -Wall -Wno-unused -Wno-misleading-indentation $EXTRA -DAMIGA -DPPU_FB8 -DRT_AUDIO=1 -DAPU_Q_SQ=512 -DAPU_Q_TRI=4096 -DAPU_Q_NOI=8192"
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
# 1. translated program + dispatch tables (sparse), palette
python3 translator/x65.py "$ROM" translator/seen_abs.txt $B/code.s $B/tab.s $B/report.txt
python3 tools/gen_tabsp.py $B/tab.s $B/tabsp.s
python3 tools/gen_palette.py $B/palette.inc
$AS $B/code.s -o $B/code.o; $AS $B/tabsp.s -o $B/tabsp.o; $AS $R/rt_x.s -o $B/rt_x_s.o
# 2. runtime
for f in rt_core rt_x cpu6502 ppu_ref mapper_mmc1 apu; do $CC $RTFLAGS -c $R/$f.c -o $B/$f.o; done
# 3. platform
for f in main display audio platform telem libc tabs palette; do $CC $CFLAGS -I$P -I$B -I$R -c $P/$f.c -o $B/$f.o; done
$CC -m68020 -x assembler -c $P/c2p.s -o $B/c2p.o; $CC -m68020 -x assembler -c $P/crt0.s -o $B/crt0.o
$CC -m68020 -x assembler -c $P/isr.s -o $B/isr.o; $CC -m68020 -x assembler -c $P/rom.s -o $B/rom.o
$CC -m68020 -x assembler -c $P/boot.s -o $B/boot_o.o
$LD -e 0 -Ttext=0 --oformat=binary -o $B/boot.bin $B/boot_o.o
[ $(stat -c %s $B/boot.bin) -le 1024 ] || { echo "bootblock > 1024 bytes"; exit 1; }
# 4. link
LIBGCC=$($CC -m68020 -print-libgcc-file-name)
$LD -T $P/link.ld -Map=$B/llb.map -o $B/llb.elf $B/crt0.o $B/code.o $B/rt_x_s.o $B/rt_x.o $B/rt_core.o $B/ppu_ref.o $B/cpu6502.o $B/mapper_mmc1.o $B/apu.o \
   $B/c2p.o $B/display.o $B/main.o $B/audio.o $B/platform.o $B/telem.o $B/isr.o $B/libc.o $B/tabs.o $B/palette.o $B/tabsp.o $B/rom.o $LIBGCC
${PREFIX}objcopy -O binary $B/llb.elf $B/llb.bin
# 5. put the ROM (PRG then CHR, without the iNES header) into the .rom placeholder and build the disk
python3 - "$ROM" $B/llb.elf $B/llb.bin <<'PY'
import subprocess, sys
rom, elf, binf = sys.argv[1:4]
nm = subprocess.check_output(['m68k-linux-gnu-nm', elf]).decode()
off = [int(l.split()[0], 16) for l in nm.split('\n') if l.endswith(' rom_start')][0] - 0x800
d = open(rom, 'rb').read()[16:16 + 262144]; assert len(d) == 262144
b = bytearray(open(binf, 'rb').read()); assert b[off:off + 262144] == bytes(262144), 'ROM placeholder not at the expected place'
b[off:off + 262144] = d
open(binf, 'wb').write(b)
PY
python3 tools/mkdisk.py "$OUT" $B/boot.bin $B/llb.bin
${PREFIX}size $B/llb.elf
