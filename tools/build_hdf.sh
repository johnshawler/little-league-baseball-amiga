#!/bin/sh
# Build the bootable A1200 hard disk image:  tools/build_hdf.sh <rom.nes> <out.hdf>
#   NOBUILD=1   reuse build/<BUILD>/llb.bin (made by a previous build_adf.sh run with the same ROM) instead of rebuilding the payload
#   BUILD=name  build directory build/<name> (default amiga)
# Layout: RDB (1 cylinder) + one bootable FFS partition DH0 "LittleLeague" (DOS\1) with  S/Startup-Sequence, LittleLeague (hunk loader), LittleLeague.dat (payload with the ROM).
# 32 cylinders x 16 heads x 63 sectors x 512 bytes = 16,515,072 bytes.  The ROM and the image are never committed.
set -e
ROM="$1"; OUT="$2"
[ -f "$ROM" ] && [ -n "$OUT" ] || { echo "usage: $0 <rom.nes> <out.hdf>"; exit 1; }
cd "$(dirname "$0")/.."
ROOT=$(pwd); B=${BUILD:-amiga}; B=$ROOT/build/$B
PREFIX=${PREFIX:-m68k-linux-gnu-}
command -v rdbtool >/dev/null && command -v xdftool >/dev/null || { echo "need amitools (pip install amitools)"; exit 1; }
if [ -z "$NOBUILD" ]; then
  # the payload is the one of the ADF; build_adf.sh also checks the ROM
  BUILD=${BUILD:-amiga} "$ROOT/tools/build_adf.sh" "$ROM" "$B/payload_only.adf" >/dev/null
fi
[ -f "$B/llb.bin" ] || { echo "missing $B/llb.bin"; exit 1; }
# loader: assemble, check that it is position independent (same bytes linked at two addresses), wrap into a hunk file
AS="${PREFIX}as -m68020 --register-prefix-optional"; LD="${PREFIX}ld -z noexecstack --no-warn-rwx-segments"
[ "${FASTRAM:-1}" = 1 ] && AS="$AS --defsym FASTRAM=1"
$AS "$ROOT/platform-a1200/loader.s" -o "$B/loader.o"
$LD -e 0 -Ttext=0 --oformat=binary -o "$B/loader.bin" "$B/loader.o"
$LD -e 0 -Ttext=0x123400 --oformat=binary -o "$B/loader2.bin" "$B/loader.o"
cmp "$B/loader.bin" "$B/loader2.bin" || { echo "loader is not position independent"; exit 1; }
python3 tools/mkhunk.py "$B/loader.bin" "$B/LittleLeague"
cp "$B/llb.bin" "$B/LittleLeague.dat"
printf 'LittleLeague\n' > "$B/Startup-Sequence"
rm -f "$OUT"
rdbtool "$OUT" create chs=32,16,63 + init rdb_cyls=1 + add start=1 end=31 name=DH0 dostype=DOS1 bootable=True pri=0 >/dev/null
xdftool "$OUT" open part=DH0 + format LittleLeague ffs + makedir S + makedir C + makedir L + makedir Libs + makedir Devs \
  + write "$B/Startup-Sequence" S/Startup-Sequence + write "$B/LittleLeague" LittleLeague + write "$B/LittleLeague.dat" LittleLeague.dat >/dev/null
ls -l "$OUT"; sha256sum "$OUT"
