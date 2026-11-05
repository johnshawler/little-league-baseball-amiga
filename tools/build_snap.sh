#!/bin/sh
# tools/build_snap.sh <rom.nes> <frames> <script.txt> <out.snap> : runtime snapshot for Amiga test builds (SNAPSHOT=out.snap tools/build_adf.sh ...)
set -e
cd "$(dirname "$0")/.."
OUT=build/snap; mkdir -p $OUT
m68k-linux-gnu-gcc -static -m68020 -O2 ${FBFLAG:--DPPU_FB5} -DAMIGA -DRT_AUDIO=1 -DAPU_Q_SQ=512 -DAPU_Q_TRI=4096 -DAPU_Q_NOI=8192 -Wno-unused -o $OUT/snap tools/snap.c runtime/rt_core.c runtime/cpu6502.c runtime/ppu_ref.c runtime/arender.c runtime/mapper_mmc1.c runtime/apu.c
qemu-m68k -cpu m68020 $OUT/snap "$1" "$2" "$3" "$4"
