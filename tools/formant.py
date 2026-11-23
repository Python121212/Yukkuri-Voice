#!/usr/bin/env python3
"""LPC でフォルマント推定 (設計値との照合用)"""
import sys, wave, numpy as np
def lpc(x, order):
    """自己相関法 + Levinson-Durbin (安定版)"""
    x = np.append(x[0], x[1:] - 0.97*x[:-1])
    x = x * np.hamming(len(x))
    r = np.correlate(x, x, 'full')[len(x)-1:len(x)+order]
    if r[0] <= 0: return None
    a = np.zeros(order+1); a[0] = 1.0; e = r[0]
    for i in range(1, order+1):
        acc = r[i] + np.dot(a[1:i], r[i-1:0:-1]) if i > 1 else r[i]
        k = -acc / e
        prev = a.copy()
        a[i] = k
        for j in range(1, i):
            a[j] = prev[j] + k * prev[i-j]
        e *= (1 - k*k)
        if e <= 0: return None
    return a
path = sys.argv[1]; t = float(sys.argv[2]); dur = float(sys.argv[3]) if len(sys.argv) > 3 else 0.08
w = wave.open(path); sr = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(float)/32768
seg = x[int(t*sr):int((t+dur)*sr)]
a = lpc(seg, int(sr/1000*1.0 + 4))
roots = [r for r in np.roots(a) if np.imag(r) > 0.01]
fs = sorted([abs(np.angle(r))*sr/(2*np.pi) for r in roots])
bw = [ -sr/np.pi*np.log(abs(r)) for r in roots ]
out = [(f, b) for f, b in zip(sorted([abs(np.angle(r))*sr/(2*np.pi) for r in roots]), [bw[i] for i in np.argsort([abs(np.angle(r))*sr/(2*np.pi) for r in roots])]) if 150 < f < sr/2-500 and b < 800]
print('formants:', [round(f) for f, _ in out[:4]], 'bw:', [round(b) for _, b in out[:4]])
