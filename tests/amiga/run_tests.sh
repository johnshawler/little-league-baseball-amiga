#!/bin/bash
# Amiga functional tests in FS-UAE (A1200, KS 3.1, no fast RAM, headless under Xvfb).
#   KICK=/path/kickstart-3.1-a1200.rom tests/amiga/run_tests.sh <rom.nes> <workdir> [pal|ntsc|both]
# Test only: the Kickstart ROM is never part of the repository or of any disk image.  The functional tests run with FAST=1 (the emulated CPU runs as fast as the host
# allows; the game is far below real A1200 speed, see docs/AMIGA.md) - results do not depend on the CPU speed because input is frame driven (TESTINPUT) or step by step.
#  1. scenes: scripted input (tools/discovery/inputs/play_watchgame.txt compiled into the build) stopped at frame N; the screenshot must equal the reference frame N pixel for pixel
#  2. real keyboard path (xdotool): Return = Start, arrows, Right Ctrl = A through the title, menus and "Watch Game" to the team selection; every screenshot must equal some reference frame
#  3. controller / keyboard input of both joystick ports and the second button, read back through the telemetry
#  4. audio chain: the PCM of the runtime for 420 frames is replayed through audio.c, recorded from FS-UAE, compared with the reference
#  4b. stage 2b display hardware: synthetic dual playfield scene (platform-a1200/arhw_test.c), Copper list from display_ar2.c, compared with tests/amiga/arhw_expect.py
#  5. display flip logic: how many of 600 game frames can be shown (PAL 50 Hz: 5 of 6)
set -e
ROM=$(readlink -f "$1"); W=$(readlink -f "${2:?workdir}"); STD=${3:-both}
: "${KICK:?set KICK to a Kickstart 3.1 A1200 ROM (test only)}"
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../.." && pwd); T=$REPO/platform-a1200/test
INP=$REPO/tools/discovery/inputs/play_watchgame.txt
mkdir -p "$W"; cd "$REPO"
[ $STD = both ] && STDS="pal ntsc" || STDS=$STD
FRAMES="100,300,400,1200,1600,3000,5000"
echo "== reference frames"
tools/build_xrt.sh "$ROM" >/dev/null
( cd "$W" && FBDUMP=$W/ref_scenes.fb FBFRAMES=$FRAMES qemu-m68k -cpu m68020 $REPO/build/xrt/xrt_host "$ROM" 5001 $INP - 2>&1 | tail -1 )
( cd "$W" && FBDUMP=$W/ref_all.fb FBFRAMES=$(seq -s, 0 700) qemu-m68k -cpu m68020 $REPO/build/xrt/xrt_host "$ROM" 701 $INP - 2>&1 | tail -1 )
XRT_OUT=build/xrt_audio XRT_EXTRA="-DPPU_FB8 -DRT_AUDIO=1 -DAPU_Q_SQ=512 -DAPU_Q_TRI=4096 -DAPU_Q_NOI=8192" tools/build_xrt.sh "$ROM" >/dev/null
for s in pal ntsc; do ( cd "$W" && PCMSTD=$s PCMOUT=$W/ref_$s.pcm qemu-m68k -cpu m68020 $REPO/build/xrt_audio/xrt_host "$ROM" 420 $INP - >/dev/null 2>&1 ); done

# run <build name> <std> <out prefix> <boot s> <script> : build with the EXTRA/env given by the caller, run FS-UAE
run() { local B=$1 S=$2 O=$3; shift 3
  BUILD=$B tools/build_adf.sh "$ROM" $W/$B.adf >/dev/null 2>$W/$B.build.log || { echo "build $B failed"; tail $W/$B.build.log; return 1; }
  ( cd $T && ADF=$W/$B.adf bash run.sh $S $W/$O "$@" ) ; }
cat > $W/shot.sh <<'EOS'
sleep ${WAIT:-60}
full g1
EOS
for s in $STDS; do
  echo "== scenes $s"
  for fr in 100 300 400 1200 1600 3000 5000; do
    ( TESTINPUT=$INP FAST=1 WAIT=$((40 + fr / 22)) EXTRA="-DSTOP_AT=$fr" run sc_${s}_$fr $s sc_${s}_$fr 15 $W/shot.sh ) &
    while [ $(jobs -r | wc -l) -ge 3 ]; do sleep 2; done
  done; wait
  for fr in 100 300 400 1200 1600 3000 5000; do python3 tests/amiga/cmp_shot.py $W/sc_${s}_${fr}_g1_full.png $W/ref_scenes.fb $fr $s || true; done
done
[ "$ONLY" = scenes ] && exit 0
echo "== free-running keyboard path (long waits; every screenshot must equal a reference frame exactly: tolerance 0 pixels)"
cat > $W/kb.sh <<'EOS'
tap() { xdotool keydown $1; sleep ${2:-0.25}; xdotool keyup $1; }
sleep 8; full k0
tap Return; sleep 12; full k1
tap Down; sleep 12; full k2
tap Control_R; sleep 12; full k3
tap Down; sleep 4; tap Down; sleep 12; full k4
tap Control_R; sleep 14; full k5
EOS
KBFAIL=0
for s in $STDS; do
  FAST=1 run kb_$s $s kb_$s 10 $W/kb.sh
  for k in 1 2 3 4 5; do python3 tests/amiga/match_ref.py $W/kb_${s}_k${k}_full.png $W/ref_all.fb $s || KBFAIL=1; done
done
echo "== free-running keyboard path through the HDF (boots from the hard disk image, long waits, 0 pixels tolerance)"
tools/build_hdf.sh "$ROM" $W/llb_test.hdf >/dev/null 2>&1 || echo "HDF build failed"
printf 'sleep 14\ntap() { xdotool keydown $1; sleep ${2:-0.25}; xdotool keyup $1; }\ntap Return; sleep 12; full k1\ntap Down; sleep 12; full k2\n' > $W/hkb.sh
for s in $STDS; do
  ( cd $T && HDF=$W/llb_test.hdf FAST=1 bash run_hdf.sh $s $W/hkb_$s 10 $W/hkb.sh >/dev/null 2>&1 )
  for k in 1 2; do python3 tests/amiga/match_ref.py $W/hkb_${s}_k${k}_full.png $W/ref_all.fb $s || KBFAIL=1; done
done
echo "keyboard regression: $([ $KBFAIL = 0 ] && echo PASS || echo FAIL)"
echo "== stage 2b display hardware (dual playfield, Copper list builder, synthetic scene; 0 pixels tolerance)"
printf 'sleep 12\nfull a1\n' > $W/a2.sh
ARHWFAIL=0
for s in $STDS; do
  ARHW_TEST=1 FAST=1 run arhw_$s $s arhw_$s 10 $W/a2.sh
  python3 tests/amiga/arhw_expect.py $W/arhw_${s}_a1_full.png $s || ARHWFAIL=1
done
echo "stage 2b display: $([ $ARHWFAIL = 0 ] && echo PASS || echo FAIL)"
echo "== controllers (telemetry)"
cat > $W/joy.sh <<'EOS'
sleep 6
for k in Up Down Left Right Control_R x Return space; do xdotool keydown $k; sleep 1.5; xdotool keyup $k; sleep 0.5; done
sleep 3; full j9
EOS
for jp in 1 0; do JOYPORT=$jp FAST=1 EXTRA=-DTELEM run joy$jp pal joy$jp 10 $W/joy.sh; python3 platform-a1200/test/telem.py $W/joy${jp}_j9_full.png | grep -o "sticky_input=[0-9]*"; done
echo "== audio chain"
printf 'sleep 14\nfull z1\n' > $W/au.sh
for s in $STDS; do
  R=$([ $s = pal ] && echo 28604 || echo 28867)
  ARENDER_HW=0 AUDIO_RATE=$R AUDIO_OUT=$W/rec_$s.wav AUDIO_TEST=$W/ref_$s.pcm EXTRA=-DTELEM run audio_$s $s au_$s 8 $W/au.sh
  python3 tests/amiga/audio_cmp.py $W/rec_$s.wav $W/ref_$s.pcm $s || true
  python3 platform-a1200/test/telem.py $W/au_${s}_z1_full.png | grep -o "underruns=[0-9]* chunks_played=[0-9]*"
done
echo "== display flip logic"
python3 - "$W" <<'PY'
import sys
w = open(sys.argv[1] + '/ref_scenes.fb', 'rb').read(); rec = 8 + 240 * 256 * 2
open(sys.argv[1] + '/dtest.bin', 'wb').write(b''.join(bytes(b & 0xFF for b in w[o + 8:o + rec:2]) for o in (0, rec)))
PY
printf 'sleep 30\nfull y1\n' > $W/df.sh
for s in $STDS; do
  ARENDER_HW=0 DISPLAY_TEST=$W/dtest.bin EXTRA="-DTELEM -DDISPLAY_TEST_FLIP" run flip_$s $s flip_$s 12 $W/df.sh
  python3 - <<PY
import sys; sys.path.insert(0, "$T"); import telem
k, w = telem.decode("$W/flip_${s}_y1_full.png")
print("$s: game frames %d, shown %d, skipped %d" % (w[2], w[4], w[5]))
PY
done
