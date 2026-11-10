#!/usr/bin/env python3
"""Every tone of a .ses sound bank (se/*.ses) -> 16-bit mono WAV <prefix>_NNN.wav (PS-ADPCM, as launcher.cpp
decodeSesSound; empty tone slots skipped). Usage: ses_wav.py se/pl00.ses out/pl00"""
import sys, struct, wave
import numpy as np
ses=open(sys.argv[1],'rb').read(); out=sys.argv[2]
u32=lambda o: struct.unpack_from('<I',ses,o)[0]
table,tones,toneBytes,data,dataSize=u32(0x10),u32(0x20),u32(0x24),u32(0x28),u32(0x2C)
F=[(0,0),(60,0),(115,-52),(98,-55),(122,-60)]
n=toneBytes//16; print('tones',n,'sounds',u32(table))
for t in range(n):
    off=u32(tones+16*t); rate=u32(tones+16*t+4)
    if not(4000<=rate<=96000) or off>=dataSize: continue
    s1=s2=0; pcm=[]; p=data+off
    while p+16<=data+dataSize:
        sh=ses[p]&15; fl=min(ses[p]>>4,4)
        for i in range(28):
            b=ses[p+2+i//2]; v=(b>>4) if i&1 else (b&15)
            if v>7: v-=16
            v=(v<<12)>>sh if sh<=12 else 0
            v+=(s1*F[fl][0]+s2*F[fl][1])>>6
            v=max(-32768,min(32767,v)); pcm.append(v); s2=s1; s1=v
        if ses[p+1]&1: break
        p+=16
    w=wave.open(f"{out}_{t:03d}.wav",'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
    w.writeframes(np.array(pcm,dtype='<i2').tobytes()); w.close()
