#!/usr/bin/env python3
"""audio_cmp.py rec.wav ref.pcm pal|ntsc
Compare a recording of FS-UAE's audio output (OpenAL wave backend, 44.1 kHz stereo 16 bit) with the PCM the runtime produced (tools/rt_host.c PCMOUT: per frame u16 words + 2*words signed 8 bit
samples; Paula plays them at 3546895/124 Hz (PAL) or 3579545/124 Hz (NTSC)).  The reference is resampled to 44.1 kHz by sample-and-hold (FS-UAE with uae_sound_interpol = none),
aligned by cross-correlation, and a linear fit rec = a*ref + b is made.  Prints correlation, gain, relative residual, and the fraction of 10 ms windows that are well matched."""
import struct, sys
import numpy as np
def read_wav(path):
    d = open(path, 'rb').read(); assert d[:4] == b'RIFF'
    i = 12; fmt = None
    while i < len(d) - 8:
        cid = d[i:i + 4]; n = struct.unpack('<I', d[i + 4:i + 8])[0]
        if cid == b'fmt ': fmt = struct.unpack('<HHIIHH', d[i + 8:i + 24])
        if cid == b'data':
            ch = fmt[1]; assert fmt[5] == 16
            n = min(n, len(d) - i - 8); x = np.frombuffer(d[i + 8:i + 8 + n // 2 * 2], dtype='<i2').reshape(-1, ch)
            return fmt[2], x
        i += 8 + n + (n & 1)
def read_pcm(path):
    d = open(path, 'rb').read(); i = 0; out = []; words = []
    while i < len(d):
        w = (d[i] << 8) | d[i + 1]; out.append(np.frombuffer(d[i + 2:i + 2 + 2 * w], dtype=np.int8)); words.append(w); i += 2 + 2 * w
    return np.concatenate(out).astype(np.float64), words
def main():
    wav, pcm, std = sys.argv[1:4]
    rate_w, x = read_wav(wav)
    ref, words = read_pcm(pcm)
    prate = (3546895 if std == 'pal' else 3579545) / 124.0
    # reference at the wav rate, sample and hold
    n = int(len(ref) * rate_w / prate)
    r = ref[np.minimum((np.arange(n) * prate / rate_w).astype(np.int64), len(ref) - 1)]
    left = x[:, 0].astype(np.float64); right = x[:, -1].astype(np.float64)
    print('recording: %d Hz, %d frames (%.2f s), channels %d; reference %d samples at %.2f Hz (%d chunks, words per chunk %s)' % (rate_w, len(x), len(x) / rate_w, x.shape[1], len(ref), prate, len(words), sorted(set(words))))
    print('left == right: %s' % bool((x[:, 0] == x[:, -1]).all()))
    # coarse alignment: first sample that leaves the idle level + the first non-silent reference sample
    idle = np.median(left[-int(rate_w * 0.5):])                      # level of the silence after the replay (DMA keeps running)
    nz = np.nonzero(ref != -128)[0]
    near = np.nonzero(np.abs(left - idle) < 0.01 * (left.max() - left.min()))[0]
    near = near[near > rate_w * 0.5]
    p0 = int(near[0])                                                 # the replay starts: the DMA plays the idle level (silence of the first chunks)
    dev = np.nonzero(np.abs(left[p0:] - idle) > 0.03 * (left.max() - left.min()))[0]
    start = p0 + int(dev[0])                                          # first audible sample
    # fine alignment per window of 4096 reference samples (at the wav rate): best lag within +-48 samples of the running lag, on 3-tap smoothed signals
    sm = lambda v: np.convolve(v, np.ones(3) / 3, mode='same')
    ls, rs = sm(left), sm(r)
    W = 4096; lag = start - int(nz[0] * rate_w / prate); lags = []; cors = []; rows = []
    for w0 in range(int(nz[0] * rate_w / prate), len(rs) - W, W):
        seg = rs[w0:w0 + W]
        if seg.std() < 1: continue
        best = (-2, 0)
        for dl in range(-48, 49):
            a_ = w0 + lag + dl
            if a_ < 0 or a_ + W > len(ls): continue
            c = float(np.corrcoef(seg, ls[a_:a_ + W])[0, 1])
            if c > best[0]: best = (c, dl)
        lag += best[1]; lags.append(lag); cors.append(best[0])
    cors = np.array(cors)
    print('windows of %d samples with signal: %d; correlation (3-tap smoothed, local lag): median %.4f, 5th percentile %.4f, min %.4f' % (W, len(cors), np.median(cors), np.percentile(cors, 5), cors.min()))
    print('lag of the recording behind the reference: first %d samples (%.1f ms), last %d (%.1f ms), drift %.3f ms over %.2f s' % (lags[0], 1000.0 * lags[0] / rate_w, lags[-1], 1000.0 * lags[-1] / rate_w, 1000.0 * (lags[-1] - lags[0]) / rate_w, len(lags) * W / rate_w))
    # gain: least squares over the whole replay with the running lag
    xs = []; ys = []
    for j, w0 in enumerate(range(int(nz[0] * rate_w / prate), len(rs) - W, W)):
        if j >= len(lags): break
        xs.append(r[w0:w0 + W]); ys.append(left[w0 + lags[j]:w0 + lags[j] + W])
    xs = np.concatenate(xs); ys = np.concatenate(ys)
    (g, off), *_ = np.linalg.lstsq(np.vstack([xs, np.ones_like(xs)]).T, ys, rcond=None)
    print('fit recording = %.2f * reference %+.0f (Paula 8 bit * volume 64 -> FS-UAE 16 bit; silence -128 plays at %.0f)' % (g, off, -128 * g + off))
    sys.exit(0 if np.median(cors) > 0.97 and cors.min() > 0.5 else 1)
main()
