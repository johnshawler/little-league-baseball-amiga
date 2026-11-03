#!/usr/bin/env python3
"""fb2png.py ref.fb out_prefix  : write every frame of a tools/rt_host.c FBDUMP file as out_prefix_<frame>.png (palette: tools/gen_palette.py)"""
import os, struct, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import gen_palette
pal = np.array(gen_palette.table(), dtype=np.uint32)
d = open(sys.argv[1], 'rb').read(); rec = 8 + 240 * 256 * 2
for o in range(0, len(d), rec):
    fr = struct.unpack('<Q', d[o:o + 8])[0]
    v = pal[np.frombuffer(d[o + 8:o + rec], dtype='<u2').reshape(240, 256) & 0xFF]
    im = np.stack([(v >> 16) & 255, (v >> 8) & 255, v & 255], -1).astype(np.uint8)
    Image.fromarray(im).save('%s_%d.png' % (sys.argv[2], fr))
