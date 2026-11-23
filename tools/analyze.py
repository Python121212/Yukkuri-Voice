#!/usr/bin/env python3
"""WAV を読み、フォルマント/ピッチ/音量を推定して表示する開発用ツール。
使い方: analyze.py out.wav [--frame 0.05]"""
import sys, wave, math
import numpy as np

def read_wav(path):
    w = wave.open(path)
    n = w.getnframes(); sr = w.getframerate()
    a = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64) / 32768.0
    return a, sr

def lpc(x, order):
    x = x * np.hanning(len(x))
    r = np.correlate(x, x, 'full')[len(x)-1:len(x)+order]
    if r[0] <= 0: return None
    a = np.zeros(order+1); a[0] = 1.0; e = r[0]
    for i in range(1, order+1):
        k = -(r[i] + np.dot(a[1:i], r[i-1:0:-1])) / e
        a[1:i+1] = a[1:i+1] + k * np.concatenate(([1.0], a[1:i][::-1])) if i > 1 else a[1:i+1] + k
        e *= (1 - k*k)
    return a

def formants(x, sr, order=16):
    a = lpc(x, order)
    if a is None: return []
    roots = np.roots(a)
    roots = [r for r in roots if np.imag(r) > 0]
    fs = sorted([abs(np.angle(r)) * sr / (2*np.pi) for r in roots])
    return [f for f in fs if 90 < f < sr/2 - 200][:5]

def f0_of(x, sr, fmin=80, fmax=450):
    x = x - x.mean()
    if np.sqrt((x**2).mean()) < 1e-4: return 0.0
    ac = np.correlate(x, x, 'full')[len(x)-1:]
    lo, hi = int(sr/fmax), int(sr/fmin)
    seg = ac[lo:hi]
    if len(seg) == 0: return 0.0
    i = int(np.argmax(seg)) + lo
    return sr / i if ac[i] > 0.3*ac[0] else 0.0

def main():
    path = sys.argv[1]
    frame = 0.04
    if '--frame' in sys.argv: frame = float(sys.argv[sys.argv.index('--frame')+1])
    x, sr = read_wav(path)
    print(f"{path}: {len(x)} samples, {len(x)/sr:.2f}s @ {sr}Hz, peak={np.abs(x).max():.3f}")
    win = int(frame*sr)
    order = int(2 + sr/1000)  # ~1 formant per kHz
    for i in range(0, len(x)-win, win):
        seg = x[i:i+win]
        rms = np.sqrt((seg**2).mean())
        if rms < 1e-4: continue
        f0 = f0_of(seg, sr)
        fs = formants(seg, sr, order=18)
        # スペクトル重心
        sp = np.abs(np.fft.rfft(seg*np.hanning(len(seg))))
        fr = np.fft.rfftfreq(len(seg), 1/sr)
        centroid = (sp*fr).sum()/max(sp.sum(),1e-9)
        # 高域比率 (4kHz 以上)
        hi = sp[fr>4000].sum()/max(sp.sum(),1e-9)
        print(f"t={i/sr:5.2f}s rms={rms:.3f} f0={f0:6.1f} F={[round(f) for f in fs]} centroid={centroid:6.0f} hi={hi:.2f}")

main()
