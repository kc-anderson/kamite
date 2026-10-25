"""the game disc access for the SNR2 tool: ISO9660 root -> _DATA.AFS -> GODHAND.FST names.

Read-only. Standard library only. Layouts (checked against the disc, Oct 17 2026):
  ISO9660: PVD at sector 16; only the root directory is needed (SLUS_215.03, _DATA.AFS, GODHAND.FST).
  AFS: "AFS\\0", u32 count (1445), then count x (u32 offset, u32 size), offsets relative to the AFS file start.
  FST: "FST\\0", f32 1.0, u32 strings_off, u32 entries_off, NUL-terminated strings, then 12-byte entries
       (u8 type 1=dir 2=file 3=end, u8 flags, u16 size_hint, u32 str_off, u32 value); dir value = child-table offset
       inside the entry blob, file value = AFS index. The 81 .rel DLLs sit under dll/debugoptimize/.
"""
from __future__ import annotations

import hashlib
import struct

SECTOR = 2048


class Disc:
    def __init__(self, iso_path: str):
        self.iso_path = iso_path
        self.f = open(iso_path, "rb")
        self.root = self._read_root()
        if "_DATA.AFS" not in self.root:
            raise ValueError("_DATA.AFS not in ISO root of %s" % iso_path)
        lba, size = self.root["_DATA.AFS"]
        self.afs_base = lba * SECTOR
        self.afs_size = size
        self.f.seek(self.afs_base)
        hdr = self.f.read(8)
        if hdr[:4] != b"AFS\0":
            raise ValueError("AFS magic mismatch: %r" % hdr[:4])
        count = struct.unpack_from("<I", hdr, 4)[0]
        table = self.f.read(count * 8)
        self.afs_entries = [struct.unpack_from("<II", table, i * 8) for i in range(count)]

    def close(self):
        self.f.close()

    def _read_root(self):
        self.f.seek(16 * SECTOR)
        pvd = self.f.read(SECTOR)
        if pvd[1:6] != b"CD001":
            raise ValueError("not an ISO9660 image (no PVD at sector 16)")
        rec = pvd[156:156 + 34]
        rlba = struct.unpack_from("<I", rec, 2)[0]
        rlen = struct.unpack_from("<I", rec, 10)[0]
        self.f.seek(rlba * SECTOR)
        d = self.f.read(rlen)
        files = {}
        i = 0
        while i < len(d):
            n = d[i]
            if n == 0:
                i = (i // SECTOR + 1) * SECTOR
                continue
            lba = struct.unpack_from("<I", d, i + 2)[0]
            size = struct.unpack_from("<I", d, i + 10)[0]
            nl = d[i + 32]
            name = d[i + 33:i + 33 + nl].decode("latin-1").split(";")[0]
            if name not in ("\x00", "\x01"):
                files[name] = (lba, size)
            i += n
        return files

    def read_root_file(self, name: str) -> bytes:
        lba, size = self.root[name]
        self.f.seek(lba * SECTOR)
        return self.f.read(size)

    def read_afs(self, index: int) -> bytes:
        off, size = self.afs_entries[index]
        if off + size > self.afs_size:
            raise ValueError("AFS entry %d overruns _DATA.AFS" % index)
        self.f.seek(self.afs_base + off)
        return self.f.read(size)

    def fst_files(self):
        """[(path, afs_index)] in FST tree order."""
        buf = self.read_root_file("GODHAND.FST")
        if buf[:4] != b"FST\0":
            raise ValueError("GODHAND.FST magic mismatch")
        strings_off, entries_off = struct.unpack_from("<II", buf, 8)
        strings = buf[strings_off:entries_off]
        ents = buf[entries_off:]
        out = []
        seen = set()

        def name_at(o):
            return strings[o:strings.index(b"\0", o)].decode("ascii")

        def walk(o, prefix, depth):
            if depth > 16 or o in seen:
                raise ValueError("FST tree malformed at %#x" % o)
            seen.add(o)
            while True:
                etype, _flags, _hint, so, value = struct.unpack_from("<BBHII", ents, o)
                if etype == 3:
                    return
                if etype == 1:
                    walk(value, prefix + (name_at(so),), depth + 1)
                elif etype == 2:
                    out.append(("/".join(prefix + (name_at(so),)), value))
                else:
                    raise ValueError("FST entry type %d at %#x" % (etype, o))
                o += 12

        walk(0, (), 0)
        return out

    def rel_entries(self):
        """[(name, path, afs_index)] for every *.rel in GODHAND.FST, sorted by AFS index."""
        rels = [(p.rsplit("/", 1)[-1], p, idx) for p, idx in self.fst_files() if p.lower().endswith(".rel")]
        rels.sort(key=lambda r: r[2])
        return rels


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()
