#!/usr/bin/env python3
"""telem.py shot_full.png [...]  - decode the telemetry words (see platform-a1200/telem.c) from FS-UAE native full-frame screenshots
(1 lores pixel = 2x2 screenshot pixels).  Finds the page by its magic long (first long of a page: 'LLB1' = HUD page at display line 224,
'LLB0' ^ 1 = 'LLB0' profile page at line 0).  Prints the words; the module API: decode(path) -> (kind, words)."""
import sys
from PIL import Image
MAGICS = {0x4C4C4231: 'hud', 0x4C4C4230: 'prof', 0x4C4C4233: 'pc'}
def bits_at(px, x0, y0, n=32, one=None):
    """read n bits (1 pixel = 1 bit, 2 screenshot pixels per lores pixel); `one` = the colour of a 1 bit (None: black = 1, as with 8 planes)"""
    v = 0
    for i in range(n):
        p = px[x0 + 2 * i + 1, y0 + 1]
        bit = (sum(p) < 3 * 0x33) if one is None else (tuple(p) == one)
        v = (v << 1) | (1 if bit else 0)
    return v
def find_all(im):
    """all pages in the screenshot: {kind: (x, y, one)}.  The first long of a page starts with 0x4C = 01001100: its first pixels tell which colour is a 1 bit
    (works for any palette: 8 planes colour 255, 5 planes colour 31)."""
    W, H = im.size; px = im.load(); out = {}
    for y in range(0, H - 4):
        for x in range(0, W - 64):
            q = [tuple(px[x + 2 * i + 1, y + 1]) for i in range(8)]
            if not (q[0] == q[2] == q[3] == q[6] == q[7] and q[1] == q[4] == q[5] and q[0] != q[1]): continue
            m = bits_at(px, x, y, 32, q[1])
            if m in MAGICS and MAGICS[m] not in out: out[MAGICS[m]] = (x, y, q[1])
    return out
def decode(path, want=None, nwords=None):
    im = Image.open(path).convert('RGB'); px = im.load()
    pages = find_all(im)
    kinds = [want] if want else [k for k in ('hud', 'prof', 'pc') if k in pages]
    if not kinds or kinds[0] not in pages: return None, None
    kind = kinds[0]; x0, y0, one = pages[kind]
    n = nwords or (18 if kind == 'hud' else 7 if kind == 'pc' else 104)
    w = []
    for i in range(n):
        r, c = divmod(i, 8)
        w.append(bits_at(px, x0 + c * 64, y0 + r * 4, 32, one))
    return kind, w
HUD = ['magic','tick','frames','vbl','underruns','chunks_played','disp_conv','disp_skip','idle_e','audio_e','disp_e','late','maxlag','pairs','pal','apu_dropped','input','sticky_input']
if __name__ == '__main__':
    for f in sys.argv[1:]:
        kind, w = decode(f)
        if not w: print(f, 'no telemetry found'); continue
        if 'pc' in find_all(Image.open(f).convert('RGB')): print('  pc page', [hex(x) for x in decode(f, 'pc')[1]])
        if kind == 'hud': print(f.split('/')[-1], ' '.join('%s=%d' % (k, v) for k, v in zip(HUD, w) if k != 'magic'))
        else: print(f.split('/')[-1], [hex(x) if i >= 16 and i % 2 == 0 else x for i, x in enumerate(w)])
