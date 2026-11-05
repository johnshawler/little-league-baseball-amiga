#!/bin/sh
# xrt_layout_check.sh <rom.nes> <reference trace (Mesen p8.trc)> <input script (p8.txt)> [frames=2700] [step=32] [first=0] : links the translated program (build/xrt objects, run tools/build_xrt.sh first)
# with its code at different offsets inside a 4 KB page (a text pad of `first + k*step` bytes in front of the translated code) and compares the trace with the reference for every layout.
# qemu-m68k 8.2.2 mis-evaluates some Bcc after flag-merging sequences when a translation block ends between the flag setter and the branch (blocks end about 30 bytes before a
# page end), see tools/qemu_tb_split_repro.s: a few percent of the layouts diverge by a few cycles.  QEMU_ARGS=-singlestep (one instruction per block) makes all layouts pass.
# Exit status 1 when a layout differs.  Usage example for the nightly check: QEMU_ARGS=-singlestep tools/xrt_layout_check.sh rom.nes p8.trc p8.txt 2700 256
set -e
cd "$(dirname "$0")/.."
ROM=$1; TRC=$2; INP=$3; FR=${4:-2700}; STEP=${5:-32}; FIRST=${6:-0}; B=${XRT_OUT:-build/xrt}; bad=0; off=$FIRST
TMP=$(mktemp -d)
while [ $off -lt 4096 ]; do
  printf 'const char xpad[%d] __attribute__((section(".text"),used,aligned(2))) = {1};\n' $((off+2)) > $TMP/pad.c
  m68k-linux-gnu-gcc -static -m68020 -O2 -DXRT $XRT_EXTRA -Wno-unused -w -o $TMP/x tools/rt_host.c runtime/rt_core.c runtime/rt_x.c runtime/cpu6502.c runtime/ppu_ref.c runtime/arender.c $TMP/pad.c runtime/mapper_mmc1.c runtime/apu.c $B/code.o $B/tab.o $B/rt_x_s.o 2>&1 | grep -v "ld:\|warning" || true
  r=$(CMP_NO2002=1 CMP_NOX=1 TRACECMP=$TRC qemu-m68k $QEMU_ARGS -cpu m68020 $TMP/x "$ROM" $FR $INP - 2>&1 | grep "IDENTICAL\|DIVERGE" | head -1)
  case "$r" in IDENTICAL*) ;; *) echo "pad $((off+2)): $r"; bad=1;; esac
  off=$((off+STEP))
done
rm -rf $TMP
[ $bad = 0 ] && echo "all layouts identical" || exit 1
