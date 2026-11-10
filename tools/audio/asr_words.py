#!/usr/bin/env python3
"""Word-timed English transcripts of WAV files (the cutscene streams from afs_adx_wav), for finding lines and for
subtitles. NVIDIA Parakeet TDT 0.6B v2 (int8) through sherpa-onnx: `pip install sherpa-onnx`, model from the
k2-fsa/sherpa-onnx GitHub release "asr-models" (sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8.tar.bz2, 482 MB).
About 0.1x real time on 2 cores. Good on words, weak on the game's names (Azel, Olivia...).

usage: asr_words.py <model dir> <out.json> <file.wav>...   -> {file: [[word, start s, last token s], ...]}
"""
import json, sys, wave
import numpy as np
from scipy import signal
import sherpa_onnx


def main():
    model, out_path, files = sys.argv[1].rstrip('/') + '/', sys.argv[2], sys.argv[3:]
    rec = sherpa_onnx.OfflineRecognizer.from_transducer(
        encoder=model + 'encoder.int8.onnx', decoder=model + 'decoder.int8.onnx', joiner=model + 'joiner.int8.onnx',
        tokens=model + 'tokens.txt', num_threads=2, model_type='nemo_transducer')
    result = {}
    for f in files:
        w = wave.open(f)
        a = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32).reshape(-1, w.getnchannels()).mean(1) / 32768
        a = signal.resample_poly(a, 16000, w.getframerate()).astype(np.float32)
        words = []
        # 24 s windows every 20 s (the model wants short audio); overlapping words are dropped below
        for s0 in np.arange(0, max(1.0, len(a) / 16000 - 1), 20):
            st = rec.create_stream()
            st.accept_waveform(16000, a[int(s0 * 16000):int((s0 + 24) * 16000)])
            rec.decode_stream(st)
            cur = None
            for tok, t in zip(st.result.tokens, st.result.timestamps):
                if tok.startswith(' ') or cur is None:
                    if cur:
                        words.append(cur)
                    cur = [tok.strip(), s0 + t, s0 + t]
                else:
                    cur[0] += tok
                    cur[2] = s0 + t
            if cur:
                words.append(cur)
        words.sort(key=lambda x: x[1])
        kept = []
        for x in words:
            if kept and abs(x[1] - kept[-1][1]) < 0.15 and x[0].lower() == kept[-1][0].lower():
                continue
            kept.append([x[0], round(float(x[1]), 3), round(float(x[2]), 3)])
        result[f.split('/')[-1]] = kept
        print(f, ' '.join(x[0] for x in kept)[:200], flush=True)
    json.dump(result, open(out_path, 'w'), indent=0)


if __name__ == '__main__':
    main()
