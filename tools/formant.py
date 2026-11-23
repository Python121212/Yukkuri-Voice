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
a = lpc(seg, 16)
roots = np.roots(a)
forms = []
for r in roots:
    f = abs(np.angle(r))*sr/(2*np.pi)
    bw = -sr/np.pi*np.log(abs(r))
    if np.imag(r) > 0 and bw < 500 and f > 200:
        forms.append((f, bw))
forms.sort()
print('formants:', [round(f) for f, _ in forms[:4]], 'bw:', [round(b) for _, b in forms[:4]])
