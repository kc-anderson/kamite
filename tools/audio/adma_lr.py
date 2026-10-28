#!/usr/bin/env python3
"""Where does a stereo stream go out of step? (the music doubling report, Oct 28)

Reads PS2X_ADMA_DUMP=<file> (the SPU2's ADMA input as played: 4-byte header 'A', core, 0, 0, then 256 s16 L/R pairs
per block) or a raw s16le stereo 48 kHz file (PS2X_AUDIO_DUMP / PS2X_AUDIO_OUT_DUMP, --raw) and prints, per window,
the lag between the left and the right channel that matches best, and the strongest self-repeat inside the left
channel. A steady lag of 0 that jumps to a few hundred ms = left and right came apart; a strong self-repeat at a fixed
lag with L/R still at 0 = the same music mixed twice. Usage: adma_lr.py <file> [--raw] [--core 1] [--window 0.5]
"""
import sys, struct
import numpy as np

def load(path, raw, core):
    data = open(path, 'rb').read()
    if raw:
        return np.frombuffer(data[:len(data) // 4 * 4], dtype='<i2').reshape(-1, 2).astype(np.float32)
    blocks = []
    step = 4 + 256 * 4
    for p in range(0, len(data) - step + 1, step):
        if data[p] != ord('A') or data[p + 1] != core:
            continue
        blocks.append(np.frombuffer(data[p + 4:p + step], dtype='<i2').reshape(256, 2))
    return np.concatenate(blocks).astype(np.float32) if blocks else np.zeros((0, 2), np.float32)

def best_lag(a, b, max_lag):
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    c = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)
    c = np.concatenate([c[-max_lag:], c[:max_lag + 1]])
    norm = np.sqrt(np.dot(a, a) * np.dot(b, b)) + 1e-9
    i = int(np.argmax(c))
    return i - max_lag, c[i] / norm

def main():
    args = sys.argv[1:]
    raw = '--raw' in args
    core = int(args[args.index('--core') + 1]) if '--core' in args else 1
    win = float(args[args.index('--window') + 1]) if '--window' in args else 0.5
    s = load(args[0], raw, core)
    rate = 48000
    w = int(win * rate)
    print(f'{len(s) / rate:.1f} s of audio')
    for start in range(0, len(s) - w, w):
        L = s[start:start + w, 0] - s[start:start + w, 0].mean()
        R = s[start:start + w, 1] - s[start:start + w, 1].mean()
        if np.abs(L).max() < 200:
            continue
        lag, corr = best_lag(L, R, rate // 2)
        # self-repeat in L: best lag between 20 ms and the window length
        n = 1 << int(np.ceil(np.log2(2 * w)))
        ac = np.fft.irfft(np.abs(np.fft.rfft(L, n)) ** 2, n)[:w]
        lo = int(0.02 * rate)
        k = lo + int(np.argmax(ac[lo:]))
        print(f'{start / rate:8.2f} s  L/R lag {lag / 48:8.1f} ms (corr {corr:5.2f})  self-repeat {k / 48:7.1f} ms ({ac[k] / (ac[0] + 1e-9):4.2f})')

if __name__ == '__main__':
    main()
