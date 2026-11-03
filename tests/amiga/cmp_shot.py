#!/usr/bin/env python3
"""cmp_shot.py shot_full.png ref.fb frame pal|ntsc [out_diff.png]
Compare an FS-UAE native full-frame screenshot (1 lores pixel = 2x2 screenshot pixels) of the Amiga build, stopped at frame `frame` (build with -DSTOP_AT=frame),
pixel for pixel with the reference frame from tools/rt_host.c (FBDUMP=ref.fb FBFRAMES=...: records of an 8 byte frame number + 240*256 little-endian uint16 NES colour|emphasis<<6).
The expected RGB of a reference pixel is nes_rgb[value & 0xFF] (tools/gen_palette.py: NES colour, red/green emphasis; blue emphasis is not displayed).
The picture window starts at screenshot pixel (138, 52) on PAL and (138, ORIGIN_NTSC_Y) on NTSC (measured: both from a boot screenshot of the grey window).
The last LINES_IGNORE picture lines can be excluded (telemetry builds) with the environment variable CMP_IGNORE_LINES."""
import os, struct, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import gen_palette
ORIGIN = {'pal': (138, 52), 'ntsc': (138, 0)}
def load_ref(path, frame):
    d = open(path, 'rb').read(); rec = 8 + 240 * 256 * 2
    for o in range(0, len(d), rec):
        if struct.unpack('<Q', d[o:o + 8])[0] == frame:
            return np.frombuffer(d[o + 8:o + rec], dtype='<u2').reshape(240, 256)
    raise SystemExit('frame %d not in %s' % (frame, path))
def main():
    shot, ref, frame, std = sys.argv[1:5]
    ox, oy = ORIGIN[std]
    if os.environ.get('ORIGIN_Y'): oy = int(os.environ['ORIGIN_Y'])
    r = load_ref(ref, int(frame))
    pal = np.array(gen_palette.table(), dtype=np.uint32)
    exp = pal[(r & 0xFF)]
    im = np.asarray(Image.open(shot).convert('RGB')).astype(np.uint32)
    got = (im[oy:oy + 480:2, ox:ox + 512:2, 0] << 16) | (im[oy:oy + 480:2, ox:ox + 512:2, 1] << 8) | im[oy:oy + 480:2, ox:ox + 512:2, 2]
    # also require that the 2x2 block is uniform
    blk = (im[oy:oy + 480:2, ox:ox + 512:2] == im[oy + 1:oy + 481:2, ox + 1:ox + 513:2]).all()
    ign = int(os.environ.get('CMP_IGNORE_LINES', '0'))
    bad = (got != exp)
    if ign: bad[240 - ign:, :] = False
    n = int(bad.sum())
    print('%s frame %s: %d of %d pixels differ (2x2 blocks uniform: %s)' % (os.path.basename(shot), frame, n, 240 * 256 - ign * 256, blk))
    if n:
        ys, xs = np.nonzero(bad)
        print('  first differences (x,y expected got):', [(int(x), int(y), hex(int(exp[y, x])), hex(int(got[y, x]))) for y, x in list(zip(ys, xs))[:6]], 'rows', int(ys.min()), '-', int(ys.max()))
        if len(sys.argv) > 5:
            out = np.zeros((240, 256, 3), np.uint8); out[bad] = (255, 0, 0); Image.fromarray(out).resize((512, 480), Image.NEAREST).save(sys.argv[5])
    sys.exit(1 if n else 0)
main()
