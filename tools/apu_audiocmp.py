#!/usr/bin/env python3
"""apu_audiocmp.py mine48.raw(mono int16 mixer units, 48 kHz) mesen.pcm(int16 stereo 48 kHz)
Compares the box-filter rendering of runtime/apu.c with Mesen's band-limited output: mine*10 (libretro MasterVolume) is passed through the same
one-pole high-pass as blip_buf (bass_shift 9), then per 100 ms window: normalised cross correlation, relative RMS error and log-spectral distance."""
import sys, numpy as np
mine = np.fromfile(sys.argv[1], dtype=np.int16).astype(np.float64) * 10.0
ref = np.fromfile(sys.argv[2], dtype=np.int16)[::2].astype(np.float64)
n = min(len(mine), len(ref)); mine = mine[:n]; ref = ref[:n]
# blip high-pass: S' = S + dx - S/512 (output = S), applied to the level signal
def hp(x):
    from scipy.signal import lfilter
    a = 1.0 - 1.0/512.0
    return lfilter([1.0, -1.0], [1.0, -a], x)
try: mh = hp(mine)
except Exception:
    mh = np.empty(n); y = 0.0; p = mine[0]; a = 1 - 1/512
    for i in range(n): y = a*y + mine[i] - p; p = mine[i]; mh[i] = y
# global lag search on a busy stretch (+-24 samples)
seg = slice(48000*20, 48000*30); best = None
for lag in range(-24, 25):
    a = ref[seg]; b = np.roll(mh, lag)[seg]; c = np.dot(a, b)/np.sqrt(np.dot(a, a)*np.dot(b, b)+1e-9)
    if best is None or c > best[1]: best = (lag, c)
print("best lag (samples, mine delayed by lag):", best[0], "ncc %.5f" % best[1])
mh = np.roll(mh, best[0])
W = 4800; nw = n//W
ncc = []; rel = []; lsd = []; lsd_lo = []; active = 0; quiet_max = 0.0
win = np.hanning(W)
for w in range(nw):
    a = ref[w*W:(w+1)*W]; b = mh[w*W:(w+1)*W]
    ea = np.dot(a, a); eb = np.dot(b, b)
    if ea < 1e3*W:                                  # reference quiet (rms < 32): only report the worst mismatch
        quiet_max = max(quiet_max, np.abs(b).max()); continue
    active += 1
    ncc.append(np.dot(a, b)/np.sqrt(ea*eb+1e-9)); rel.append(np.sqrt(np.sum((a-b)**2)/(ea+1e-9)))
    A = np.abs(np.fft.rfft(a*win))[1:W//2]; B = np.abs(np.fft.rfft(b*win))[1:W//2]
    lsd.append(np.sqrt(np.mean((20*np.log10((A+1)/(B+1)))**2))); lsd_lo.append(np.sqrt(np.mean((20*np.log10((A[:800]+1)/(B[:800]+1)))**2)))
ncc = np.array(ncc); rel = np.array(rel); lsd = np.array(lsd)
print("windows: %d of %d active (100 ms)" % (active, nw))
print("NCC   : mean %.4f  median %.4f  min %.4f  p5 %.4f" % (ncc.mean(), np.median(ncc), ncc.min(), np.percentile(ncc, 5)))
print("rel RMS error (|a-b|/|a|): mean %.4f median %.4f max %.4f" % (rel.mean(), np.median(rel), rel.max()))
print("log-spectral distance (dB rms over 0-24 kHz): mean %.2f median %.2f max %.2f" % (lsd.mean(), np.median(lsd), lsd.max()))
lsd_lo = np.array(lsd_lo)
print("log-spectral distance below 8 kHz (dB rms): mean %.2f median %.2f max %.2f" % (lsd_lo.mean(), np.median(lsd_lo), lsd_lo.max()))
print("quiet windows (reference rms < 32): worst |mine| peak %.1f" % quiet_max)
