#!/usr/bin/env python3
"""LTAS (長時間平均スペクトル) を帯域ごとの dB で表示する。
   日本語音声の目安: 400-700Hz を基準に、1-3kHz が -6〜-14dB、4-7kHz が -12〜-22dB。"""
import sys, wave, numpy as np
path = sys.argv[1]
w = wave.open(path); sr = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(float) / 32768
win = 1024
acc = np.zeros(win // 2 + 1)
n = 0
for i in range(0, len(x) - win, win // 2):
    seg = x[i:i + win]
    if np.sqrt((seg ** 2).mean()) < 0.02:   # 無音は除く
        continue
    acc += np.abs(np.fft.rfft(seg * np.hanning(win))) ** 2
    n += 1
if n == 0:
    print("有声区間がありません"); sys.exit(1)
acc /= n
fr = np.fft.rfftfreq(win, 1 / sr)
def band(a, b):
    m = (fr >= a) & (fr < b)
    return 10 * np.log10(acc[m].mean() + 1e-20)
base = band(400, 700)
print(f"{path}  ({n} frames)")
for a, b in [(150,300),(300,400),(400,700),(700,1000),(1000,1500),(1500,2000),(2000,3000),(3000,4000),(4000,6000),(6000,9000)]:
    v = band(a, b) - base
    bar = "#" * int(max(0, (v + 30) / 1.5))
    print(f"{a:5d}-{b:5d}Hz: {v:+6.1f} dB  {bar}")
