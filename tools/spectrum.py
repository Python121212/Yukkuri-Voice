#!/usr/bin/env python3
"""ロング FFT でスペクトル包絡を見て、フォルマントが狙い通り出ているか確認する。
使い方: spectrum.py out.wav t_center [dur]"""
import sys, wave, numpy as np
w = wave.open(sys.argv[1]); sr = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(float)/32768
t = float(sys.argv[2]); dur = float(sys.argv[3]) if len(sys.argv)>3 else 0.06
seg = x[int(t*sr):int((t+dur)*sr)]
seg = seg - seg.mean()
sp = np.abs(np.fft.rfft(seg*np.hanning(len(seg))))
fr = np.fft.rfftfreq(len(seg), 1/sr)
# 1/3 octave smoothing
def smooth(f, s, fc):
    m = (f > fc/1.12) & (f < fc*1.12)
    return s[m].mean() if m.any() else 0
fcs = [100,125,160,200,250,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000,12500]
vals = [smooth(fr, sp, c) for c in fcs]
mx = max(vals)
print("1/3oct spectrum (norm, dB):")
for c, v in zip(fcs, vals):
    bar = '#' * int(40*v/mx)
    print(f"{c:6d}Hz {20*np.log10(max(v,1e-9)/mx):6.1f}dB {bar}")
