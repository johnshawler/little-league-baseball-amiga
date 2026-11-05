#!/usr/bin/env python3
"""arhw_expect.py shot_full.png pal|ntsc : compare an FS-UAE screenshot of the ARHW_TEST build (platform-a1200/arhw_test.c) with the picture the stage 2b dual playfield must show.
Prints the number of differing pixels and the first differing lines; exit 0 when identical."""
import os, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import gen_palette
ORIGIN = {'pal': (138, 52), 'ntsc': (138, 0)}
shot, std = sys.argv[1:3]; ox, oy = ORIGIN[std]
nes = gen_palette.table()
bgv = lambda c, r, x: (x + 3 * r + 5 * c + ((x >> 4) ^ (r >> 1))) & (7 if c else 15)
spv = lambda y, x: ((x * 3 + y) & 7) if (x + y) % 5 < 2 else 0
x1v = lambda y, x: (x * 5 + y * 3) & 15
x2v = lambda y, x: ((x + 2 * y) & 15) if x % 3 else 0
BG, EX, BL = 0, 1, 2
T = [(0, 31, BG, 0, 0, 0, 0, 0), (32, 63, BG, 0, 100, 5, 3, 1), (64, 95, BG, 1, 300, 63, 7, 2), (96, 107, BG, 0, 500, 16, 0, 2), (108, 127, BG, 0, 0, 16, 0, 3),
     (128, 143, EX, 0, 0, 0, 0, 3), (144, 159, BL, 0, 0, 0, 0, 4), (160, 191, BG, 1, 0, 31, 5, 4), (192, 215, BG, 0, 200, 8, 0, 5), (216, 239, BG, 0, 224, 8, 0, 6)]
exp = np.zeros((240, 256), dtype=np.uint32)
for i, (y0, y1, kind, cfg, row0, bytex, fx, pv) in enumerate(T):
    pal = [((e * 11 + 9 + 5 * pv) & 0x3F) if pv >= 6 else ((e * 7 + 3 + (5 * pv if e % 8 == pv % 8 else 0) + (i if e == 17 else 0)) & 0x3F) for e in range(32)]
    col = lambda e: nes[pal[e]]
    for y in range(y0, y1 + 1):
        for x in range(256):
            if kind == BG:
                b = bgv(cfg, (row0 + y - y0) % 512, (bytex * 8 + fx + x) % 512); s = spv(y, x)
            elif kind == EX:
                b = x1v(y, x); s = x2v(y, x)
            else:
                b = 0; s = 0
            exp[y, x] = (col(16 + s) if s & 3 else col(0)) if s else col(b if b & 3 else 0)
im = np.asarray(Image.open(shot).convert('RGB')).astype(np.uint32)
got = (im[oy:oy + 480:2, ox:ox + 512:2, 0] << 16) | (im[oy:oy + 480:2, ox:ox + 512:2, 1] << 8) | im[oy:oy + 480:2, ox:ox + 512:2, 2]
bad = (exp != got)
print('%s: %d pixels differ' % (os.path.basename(shot), int(bad.sum())))
rows = np.nonzero(bad.any(axis=1))[0]
if len(rows): print('differing lines:', rows[:20].tolist(), '... first x:', np.nonzero(bad[rows[0]])[0][:8].tolist())
sys.exit(0 if not bad.any() else 1)
