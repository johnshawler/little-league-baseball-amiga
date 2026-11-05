#!/usr/bin/env python3
"""match_ref.py shot_full.png ref.fb pal|ntsc : find the reference frame (from an FBDUMP file with many frames) that is closest to an Amiga screenshot; prints the frame number and the number of differing pixels
(used for tests where the input comes from the real keyboard/joystick and the frame at which the screenshot is taken is not known)."""
import os, struct, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import gen_palette
ORIGIN = {'pal': (138, 52), 'ntsc': (138, 0)}
shot, ref, std = sys.argv[1:4]; ox, oy = ORIGIN[std]
pal = np.array(gen_palette.table(), dtype=np.uint32)
im = np.asarray(Image.open(shot).convert('RGB')).astype(np.uint32)
got = (im[oy:oy + 480:2, ox:ox + 512:2, 0] << 16) | (im[oy:oy + 480:2, ox:ox + 512:2, 1] << 8) | im[oy:oy + 480:2, ox:ox + 512:2, 2]
d = open(ref, 'rb').read(); rec = 8 + 240 * 256 * 2; best = (10 ** 9, -1)
ign = int(os.environ.get('CMP_IGNORE_LINES', '0'))
for o in range(0, len(d), rec):
    fr = struct.unpack('<Q', d[o:o + 8])[0]
    exp = pal[np.frombuffer(d[o + 8:o + rec], dtype='<u2').reshape(240, 256) & 0xFF]
    n = int((exp[:240 - ign] != got[:240 - ign]).sum())
    if n < best[0]: best = (n, fr)
print('%s: closest reference frame %d, %d pixels differ' % (os.path.basename(shot), best[1], best[0]))
sys.exit(0 if best[0] <= int(os.environ.get('TOL', '0')) else 1)
