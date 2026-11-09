"""Builds src/kamite/ghr.ico (the exe's icon, ghr.rc) from assets/icon.svg: each size rendered from the SVG on its own
(sharper than scaling one bitmap down), stored as PNG entries (Windows Vista and later read those).

    python3 -I tools/icon/make_ico.py        (needs rsvg-convert: apt install librsvg2-bin)
"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SVG = os.path.join(ROOT, "assets", "icon.svg")
ICO = os.path.join(ROOT, "src", "kamite", "ghr.ico")
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def main() -> int:
    images = [subprocess.run(["rsvg-convert", "-w", str(s), "-h", str(s), SVG], check=True, capture_output=True).stdout
              for s in SIZES]
    head = struct.pack("<HHH", 0, 1, len(images))
    offset = len(head) + 16 * len(images)
    entries, data = b"", b""
    for size, png in zip(SIZES, images):
        side = 0 if size >= 256 else size  # 0 means 256 in an ICO directory
        entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(png), offset + len(data))
        data += png
    with open(ICO, "wb") as out:
        out.write(head + entries + data)
    print(f"{ICO}: {len(SIZES)} sizes, {os.path.getsize(ICO)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
