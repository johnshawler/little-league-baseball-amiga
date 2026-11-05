#!/bin/sh
# build the translated program + runtime for qemu-m68k:  tools/build_xrt.sh <rom.nes> -> build/xrt/xrt_host
set -e
cd "$(dirname "$0")/.."
# XRT_OUT=dir (default build/xrt), XRT_EXTRA="-D..." extra compiler flags (e.g. the Amiga flags -DPPU_FB8 -DRT_AUDIO=1 for reference PCM, see tools/rt_host.c PCMOUT)
OUT=${XRT_OUT:-build/xrt}; mkdir -p $OUT
XL_DEBUG_BLOCKS=$XL_DEBUG_BLOCKS python3 translator/x65.py "$1" translator/seen_abs.txt $OUT/code.s $OUT/tab.s $OUT/report.txt
AS="m68k-linux-gnu-as -m68020 --register-prefix-optional"
$AS $OUT/code.s -o $OUT/code.o; $AS $OUT/tab.s -o $OUT/tab.o; $AS --defsym XRT_TRACE=1 runtime/rt_x.s -o $OUT/rt_x_s.o
m68k-linux-gnu-gcc -static -m68020 -O2 -DXRT $XRT_EXTRA -Wno-unused -o $OUT/xrt_host tools/rt_host.c runtime/rt_core.c runtime/rt_x.c runtime/cpu6502.c runtime/ppu_ref.c runtime/arender.c runtime/mapper_mmc1.c runtime/apu.c $OUT/code.o $OUT/tab.o $OUT/rt_x_s.o 2>&1 | grep -v "warning\|NOTE" || true
