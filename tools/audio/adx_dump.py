#!/usr/bin/env python3
"""Where does a stream channel go out of step? Reads KAMITE_ADX_DUMP=<file> (with KAMITE_ADX_PROBE=1).

The dump holds, per stream channel (DTR slot), the bytes the ADX decoder committed to its EE source ring ('P') and the
bytes the transfer server sent to the IOP ('T'), in order (record layout in src/kamite/debug_tools.cpp). For each
channel the sent stream should equal the decoded stream byte for byte: every place where it does not is printed with
the displacement the sent bytes actually have in the decoded stream (+16384 = the transfer read a whole ring ahead of
the decoder's data = the source ring lapped). For each stereo stream (two slots created together) the left/right lag
of the decoded and of the sent stream is printed per window (as tools/audio/adma_lr.py does for the SPU2 input).
Usage: adx_dump.py <dump> [--window 1.0] [--rate 48000]
"""
import struct, sys
import numpy as np

HEAD = struct.Struct('<cBHIIId')


def read(path):
    data = open(path, 'rb').read()
    p, recs = 0, []
    while p + HEAD.size <= len(data):
        kind, slot, _, sj, addr, size, t = HEAD.unpack_from(data, p)
        p += HEAD.size
        payload = b''
        if kind in (b'P', b'T'):
            if p + size > len(data):
                break
            payload = data[p:p + size]
            p += size
        recs.append((kind.decode(), slot, sj, addr, size, t, payload))
    return recs


def channels(recs):
    """One entry per slot lifetime: {slot, sj, start, end, P: [(t, bytes)], T: [(t, bytes)]}."""
    live, done = {}, []
    for kind, slot, sj, addr, size, t, payload in recs:
        if kind == 'C':
            if slot in live:
                done.append(live.pop(slot))
            live[slot] = {'slot': slot, 'sj': sj, 'dst': addr, 'start': t, 'end': None, 'P': [], 'T': []}
        elif kind == 'X':
            if slot in live:
                live[slot]['end'] = t
                done.append(live.pop(slot))
        elif kind in ('P', 'T'):
            ch = live.get(slot)
            if ch is None or ch['sj'] != sj:
                continue
            ch[kind].append((t, payload))
    done += live.values()
    return sorted(done, key=lambda c: (c['start'], c['slot']))


def joined(chunks):
    times, out, pos = [], bytearray(), 0
    for t, b in chunks:
        times.append((pos, t))
        out += b
        pos += len(b)
    return bytes(out), times


def time_at(times, offset):
    lo = 0
    for pos, t in times:
        if pos > offset:
            break
        lo = t
    return lo


def compare(ch):
    """Walk the sent stream against the decoded one; print every change of displacement."""
    P, _ = joined(ch['P'])
    T, ttimes = joined(ch['T'])
    d, o, block, events = 0, 0, 64, 0
    while o + block <= len(T):
        want = P[o + d:o + d + block]
        got = T[o:o + block]
        if len(want) < block:
            break
        if got == want:
            o += block
            continue
        # find the sent block in the decoded stream near here
        lo = max(0, o + d - 0x10000)
        found = P.find(T[o:o + 256], lo, o + d + 0x10000 + 256) if any(T[o:o + 256]) else -1
        if found < 0:
            # a block that is in no nearby decoded data (stale bytes, silence): step over it
            print(f'  slot {ch["slot"]}: {time_at(ttimes, o):9.4f}s sent offset {o:#x}: {block} bytes not in the decoded stream'
                  f' (displacement stays {d:+d})')
            o += block
            events += 1
        else:
            nd = found - o
            print(f'  slot {ch["slot"]}: {time_at(ttimes, o):9.4f}s sent offset {o:#x}: displacement {d:+d} -> {nd:+d}'
                  f' ({(nd - d) / 2:+.0f} samples)')
            d = nd
            events += 1
        if events > 40:
            print(f'  slot {ch["slot"]}: more than 40 changes, stopping')
            break
    return len(P), len(T), events


def best_lag(a, b, max_lag):
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    c = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)
    c = np.concatenate([c[-max_lag:], c[:max_lag + 1]])
    norm = np.sqrt(np.dot(a, a) * np.dot(b, b)) + 1e-9
    i = int(np.argmax(c))
    return i - max_lag, c[i] / norm


def lr_lags(left, right, rate, window, label):
    L = np.frombuffer(left[:len(left) // 2 * 2], '<i2').astype(np.float64)
    R = np.frombuffer(right[:len(right) // 2 * 2], '<i2').astype(np.float64)
    n = min(len(L), len(R))
    w = int(window * rate)
    last = None
    for s in range(0, n - w, w):
        a, b = L[s:s + w] - L[s:s + w].mean(), R[s:s + w] - R[s:s + w].mean()
        if np.abs(a).max() < 200 or np.abs(b).max() < 200:
            continue
        lag, corr = best_lag(a, b, min(12000, w // 2))
        if lag != last:
            print(f'  {label}: from sample {s} ({s / rate:7.2f} s into the stream) L/R lag {lag:+d} samples (corr {corr:.2f})')
            last = lag


def main():
    args = sys.argv[1:]
    window = float(args[args.index('--window') + 1]) if '--window' in args else 1.0
    rate = int(args[args.index('--rate') + 1]) if '--rate' in args else 48000
    chans = channels(read(args[0]))
    for ch in chans:
        end = f'{ch["end"]:.3f}' if ch['end'] is not None else 'end'
        print(f'slot {ch["slot"]} source SJ {ch["sj"]:#x} -> {ch["dst"]:#x}, {ch["start"]:.3f}..{end} s')
        p, t, events = compare(ch)
        print(f'  decoded {p} bytes, sent {t} bytes, {events} change(s)')
    # stereo streams: two slots created within 1 ms of each other
    for a, b in zip(chans, chans[1:]):
        if abs(a['start'] - b['start']) < 0.001 and a['slot'] != b['slot']:
            left, right = (a, b) if a['slot'] < b['slot'] else (b, a)
            print(f'stereo stream slots {left["slot"]}/{right["slot"]} from {left["start"]:.3f} s:')
            lr_lags(joined(left['P'])[0], joined(right['P'])[0], rate, window, 'decoded')
            lr_lags(joined(left['T'])[0], joined(right['T'])[0], rate, window, 'sent   ')


if __name__ == '__main__':
    main()
