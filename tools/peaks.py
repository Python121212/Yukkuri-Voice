#!/usr/bin/env python3
"""ケプストラムで包絡を出し、フォルマントのピーク位置を読む (設計値との比較用)"""
import sys, wave, numpy as np
w=wave.open(sys.argv[1]); sr=w.getframerate()
x=np.frombuffer(w.readframes(w.getnframes()),dtype=np.int16).astype(float)/32768
t=float(sys.argv[2]); dur=float(sys.argv[3]) if len(sys.argv)>3 else 0.06
seg=x[int(t*sr):int((t+dur)*sr)]
seg=(seg-seg.mean())*np.hamming(len(seg))
def env(seg, order=40):
    S=np.abs(np.fft.rfft(seg)); L=np.log(S+1e-12)
    C=np.fft.rfft(L); C[order:-order]=0
    return np.exp(np.fft.irfft(C, n=len(seg)*2//2*1 if False else len(S)*2-2)[:len(S)])
E=env(seg); fr=np.fft.rfftfreq(len(seg),1/sr)
# find local maxima below 5k
pk=[]
for i in range(2,len(E)-2):
    if E[i]>E[i-1] and E[i]>=E[i+1] and 150<fr[i]<5500: pk.append((E[i],fr[i]))
pk.sort(reverse=True)
sel=[]
for v,f in pk:
    if all(abs(f-g)>150 for _,g in sel): sel.append((v,f))
    if len(sel)>=5: break
print('envelope peaks:', [round(f) for _,f in sorted(sel,key=lambda z:z[1])])
