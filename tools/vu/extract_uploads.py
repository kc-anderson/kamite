"""Finds the VU microcode the game uploads (VIF MPG commands) in its program files and writes each upload for vu1_recomp.

    python -I tools/vu/extract_uploads.py <out dir> <file> [<file> ...]

The game's VU1 programs sit in its files as DMA packets: an MPG VIFcode (command 0x4A, NUM pairs, load address in
pairs) followed by NUM 64-bit instruction pairs, 8-byte aligned. Programs longer than 256 pairs are a chain of MPGs
back to back. Each upload (a chain merged into one) is written as vup_<pair address hex>_<file tag>_<offset hex>.bin.
A word that only looks like an MPG gives an upload that does not decode as VU code; vu1_recomp skips those.
Inputs here: the boot ELF (build/SLUS_215.03, written by build.py generate) and the SNR2 DLLs (build/snr2/rel/*.rel).
Standard library only.
"""
import os
import re
import struct
import sys

MICRO_PAIRS = 2048      # VU1 micro memory: 16 KB
MAX_GAP = 32            # bytes between two MPGs of one chain (DMA tags, NOPs)


def scan(data):
    """(offset of the code, pair address, pair count) of every plausible MPG, in file order."""
    found = []
    for match in re.finditer(rb"[\x4a\xca]", data):
        at = match.start() - 3              # the command is the VIFcode's top byte (little endian word)
        if at < 0 or at % 8 != 4:          # the code that follows must be 8-byte aligned
            continue
        word = struct.unpack_from("<I", data, at)[0]
        num = (word >> 16) & 0xFF or 256
        address = word & 0xFFFF
        code = at + 4
        if address + num > MICRO_PAIRS or code + num * 8 > len(data):
            continue
        body = data[code:code + num * 8]
        if body.count(0) == len(body):
            continue
        found.append((code, address, num))
    return found


def chains(found):
    """Merges MPGs that continue each other (next address = end of the previous, next one right after it)."""
    merged = []
    for code, address, num in found:
        if merged:
            pcode, paddress, pnum = merged[-1]
            if address == paddress + pnum and 0 <= code - 4 - (pcode + pnum * 8) <= MAX_GAP:
                merged[-1] = (pcode, paddress, pnum + num)
                chains.parts.setdefault((pcode, paddress), [(pcode, pnum)]).append((code, num))
                continue
        merged.append((code, address, num))
    return merged


chains.parts = {}


def upload_bytes(data, code, address, num):
    parts = chains.parts.get((code, address))
    if not parts:
        return data[code:code + num * 8]
    return b"".join(data[c:c + n * 8] for c, n in parts)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    out = argv[1]
    os.makedirs(out, exist_ok=True)
    for name in os.listdir(out):
        if name.startswith("vup_") and name.endswith(".bin") and "_cap_" not in name:  # keep captured uploads
            os.remove(os.path.join(out, name))
    written = pairs = 0
    seen = set()
    for path in argv[2:]:
        with open(path, "rb") as f:
            data = f.read()
        tag = re.sub(r"[^A-Za-z0-9]", "", os.path.basename(path))[:16] or "file"
        chains.parts = {}
        for code, address, num in chains(scan(data)):
            body = upload_bytes(data, code, address, num)
            if (address, body) in seen:
                continue
            seen.add((address, body))
            with open(os.path.join(out, "vup_%03x_%s_%06x.bin" % (address, tag, code)), "wb") as f:
                f.write(body)
            written += 1
            pairs += len(body) // 8
    print("VU microcode uploads: %d (%d pairs) from %d files into %s" % (written, pairs, len(argv) - 2, out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
