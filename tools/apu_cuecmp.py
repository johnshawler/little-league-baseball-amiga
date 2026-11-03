#!/usr/bin/env python3
"""apu_cuecmp.py exact.raw(int8, apu_render8 PCM) cue.raw(int16 = counts*16, simulated Paula output of the cue mode) [rate]
Per 100 ms window (both signals at Paula's rate, DC removed): normalised cross correlation, relative RMS error with the best gain, log-spectral distance below 8 kHz,
and the loudness (RMS) ratio in dB; per NES frame RMS difference (envelope error)."""
import sys, numpy as np
ex = np.fromfile(sys.argv[1], dtype=np.int8).astype(np.float64) + 128.0
cu = np.fromfile(sys.argv[2], dtype=np.int16).astype(np.float64) / 16.0
rate = float(sys.argv[3]) if len(sys.argv) > 3 else 28604.0
n = min(len(ex), len(cu)); ex = ex[:n]; cu = cu[:n]
W = int(rate * 0.1); nw = n // W; win = np.hanning(W)
ncc = []; rel = []; lsd = []; gdb = []; sc = []; pk = []
for w in range(nw):
    a = ex[w*W:(w+1)*W]; b = cu[w*W:(w+1)*W]; a = a - a.mean(); b = b - b.mean()
    ea = a @ a; eb = b @ b
    if ea < 4.0*W: continue                          # reference rms < 2 counts: silent
    ncc.append(a @ b / np.sqrt(ea*eb + 1e-9)); g = (a @ b) / (b @ b + 1e-9); rel.append(np.sqrt(np.sum((a - g*b)**2) / ea))
    gdb.append(10*np.log10((eb + 1e-9) / ea))
    A = np.abs(np.fft.rfft(a*win))[1:int(W*8000/rate)]; B = np.abs(np.fft.rfft(b*win))[1:int(W*8000/rate)]
    lsd.append(np.sqrt(np.mean((20*np.log10((A + 1)/(B + 1)))**2)))
    sc.append(np.corrcoef(A, B)[0, 1])
    lo = int(60*W/rate); ia = lo + np.argmax(A[lo:]); ib = lo + np.argmax(B[lo:]); pk.append(1200*np.log2((ib + 1)/(ia + 1)))
ncc = np.array(ncc); rel = np.array(rel); lsd = np.array(lsd); gdb = np.array(gdb)
print("windows: %d active of %d (100 ms)" % (len(ncc), nw))
print("waveform NCC (phase dependent: free running oscillators, expected ~0):  mean %.4f  median %.4f  p5 %.4f  min %.4f" % (ncc.mean(), np.median(ncc), np.percentile(ncc, 5), ncc.min()))
print("spectral magnitude correlation 0-8 kHz: mean %.3f median %.3f p5 %.3f" % (np.nanmean(sc), np.nanmedian(sc), np.nanpercentile(sc, 5)))
pk = np.array(pk)
print("dominant spectral peak, cue vs exact: |error| < 25 cents in %.1f %% of windows, < 100 cents in %.1f %%, an octave or more off in %.1f %%" % (100*np.mean(np.abs(pk) < 25), 100*np.mean(np.abs(pk) < 100), 100*np.mean(np.abs(pk) > 1100)))
print("rel RMS error (waveform, best gain; ~1 = phase differs) (best gain): mean %.3f  median %.3f  p95 %.3f" % (rel.mean(), np.median(rel), np.percentile(rel, 95)))
print("loudness cue/exact (dB): mean %.2f  median %.2f  p5 %.2f  p95 %.2f" % (gdb.mean(), np.median(gdb), np.percentile(gdb, 5), np.percentile(gdb, 95)))
print("log-spectral distance < 8 kHz (dB rms): mean %.2f  median %.2f  p95 %.2f" % (lsd.mean(), np.median(lsd), np.percentile(lsd, 95)))
# envelope: RMS per ~480 sample block (one NES frame)
B = 480; nb = n // B; ea = np.array([np.std(ex[i*B:(i+1)*B]) for i in range(nb)]); eb = np.array([np.std(cu[i*B:(i+1)*B]) for i in range(nb)])
m = ea > 2.0
d = 20*np.log10((eb[m] + 0.1)/(ea[m] + 0.1))
print("per-frame RMS level cue/exact (dB): mean %.2f  mean |.| %.2f  p5 %.2f  p95 %.2f  (%d frames)" % (d.mean(), np.abs(d).mean(), np.percentile(d, 5), np.percentile(d, 95), m.sum()))
