#!/usr/bin/env python3
"""profread.py shot_full.png llb.elf [n] : decode the PROFILE page (platform-a1200/telem.c) of an FS-UAE screenshot and print categories and the top code bins with symbols"""
import bisect, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import telem
CATS = ['translated 6502 code', 'rt_x.s glue', 'rt_x.c helpers', 'rt_core (time model, PPU regs)', 'ppu_ref (PPU)', 'cpu6502 + mapper', 'apu', 'arender (planar renderer)', 'display + main/audio/platform', 'idle (STOP)', 'libgcc/other']
shot, elf = sys.argv[1:3]; top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
kind, w = telem.decode(shot, 'prof')
if not w: raise SystemExit('no profile page')
tot = w[1]
print('samples %d (outside text %d); game frames %d..%d during the window (%d frames), window %d ticks' % (tot, w[2], w[14], w[15], w[15] - w[14], int(os.environ.get('PROF_TICKS', '0'))))
frames = max(w[15] - w[14], 1); ticks = int(os.environ.get('PROF_TICKS', '0'))
for i, c in enumerate(CATS):
    ms = 1000.0 * ticks / 60.0988 * w[3 + i] / tot / frames if ticks else 0
    print('  %-34s %6d %5.1f%%  %8.1f ms/frame' % (c, w[3 + i], 100.0 * w[3 + i] / tot, ms))
nm = [l.split() for l in subprocess.check_output(['m68k-linux-gnu-nm', '-n', elf]).decode().split('\n') if len(l.split()) == 3 and l.split()[1] in 'tTwW']
addrs = [int(a, 16) for a, _, _ in nm]
agg = {}
for r in range(44):
    a, c = w[16 + 2 * r], w[17 + 2 * r]
    if not c: break
    i = bisect.bisect_right(addrs, a) - 1
    agg[nm[i][2]] = agg.get(nm[i][2], 0) + c
print('top bins (32 bytes) aggregated by function (only the 44 hottest bins are in the page):')
for k, v in sorted(agg.items(), key=lambda x: -x[1])[:top]: print('  %-28s %6d %5.1f%%' % (k, v, 100.0 * v / tot))
