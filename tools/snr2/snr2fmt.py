"""SNR2 module format (the game's DLL / '.rel' format, also used for the boot ELF's own .sndata module #0).

Read-only parsing. Every field below is read by the game's loader in SLUS_215.03; addresses cite the boot ELF
(see port/generated/<name>_0x<addr>.cpp for the per-instruction listing):

  header (15 x u32 at offset 0; func_00393F80 validates, func_00394828 relocates in place):
    0x00 magic        'S','N','R', version byte '1' or '2'           (0x393F88-0x393FB4)
    0x04 relocs_off   symbol relocation table, 12-byte entries        (+base, 0x394848)
    0x08 reloc_count
    0x0C funcs_off    symbol ("function") table, 12-byte entries      (+base, 0x39485C)
    0x10 func_count
    0x14 name_off     image name string, e.g. "..\\PS2_EE_Release\\McPort.elf" (+base, 0x39486C)
    0x18 ctor_off     ctor routine; func_00394DD8 does jalr hdr[0x18] after linking (0x394EEC-0x394EF8)
    0x1C dtor_off     dtor routine                                    (+base)
    0x20 exports_off  table of export_count u32 pointers, each +base if nonzero (0x394B8C-0x394BB0)
    0x24 export_count (0 in all 81 DLLs and the boot ELF)
    0x28 align        load alignment; validate returns 4 if (align-1) & load_address (0x393FBC-0x393FCC)
    0x2C size         file size of the module image
    0x30 next         module-list link; zeroed by the loader, set when the next module is appended (0x3948D0, 0x394BEC)
    0x34 stream_off   compact internal-relocation byte stream (+base, 0x3948AC; decoded 0x3949A8-0x394B28)
    0x38 field38      +base (0x3948BC); equals stream_off and exports_off in every DLL; use unknown
  symbol entry (12 bytes): u32 name_ptr (+base always), u32 addr, u16 hash, u8 type, u8 flag (registered)
    type 0 local (addr+base), 1 import (addr = lookup(name, hash) over registered symbols, func_00393FD8),
         2 strong export (addr+base, func_003944E8), 3 weak export (addr+base, func_003945C0),
         4 absolute export (addr unchanged, func_003944E8); >=5 aborts the load with code 7 (0x394914).
  relocation entry (12 bytes): u32 r_offset (+base, stored back), u32 r_info = (sym << 8) | type, s32 addend;
    value = symtab[sym].addr + addend, applied by patch() 0x394088 (0x394B40-0x394B88; failure aborts with 10).
  stream: per entry one byte b: delta = b & 0xFC, kind = b & 3. delta != 0: addr += delta. delta == 0: next u32 is an
    absolute module offset (addr = base + u32), u32 == 0 ends the stream. kind 0 LO16 (half = base + lhu),
    1 HI16 (value = base + (hi << 16) + lh(next kind-0 entry's site), found by a forward scan 0x394A14-0x394A80),
    2 J26 (target = base + (field << 2)), 3 W32 (word = base + word).
"""
from __future__ import annotations

import struct

HDR_FIELDS = ("magic", "relocs_off", "reloc_count", "funcs_off", "func_count", "name_off", "ctor_off", "dtor_off",
              "exports_off", "export_count", "align", "size", "next", "stream_off", "field38")
# header words the loader adds the base to when non-zero (func_00394828 0x394848-0x3948C8)
HDR_RELOCATED = (0x04, 0x0C, 0x14, 0x18, 0x1C, 0x20, 0x34, 0x38)

SYM_TYPES = {0: "local", 1: "import", 2: "export", 3: "weak", 4: "abs"}
STREAM_KINDS = {0: "lo16", 1: "hi16", 2: "j26", 3: "w32"}
# r_info types patch() 0x394088 accepts (others return 0 = load failure)
RELOC_TYPES = {0: "none", 1: "r16", 2: "w32", 4: "j26", 5: "hi16", 6: "lo16",
               0x77: "mips15_s3", 0x78: "dvp_11_pcrel", 0x79: "dvp_27_s4", 0x7A: "dvp_11_s4", 0x7B: "dvp_u15_s3"}

JR_RA = 0x03E00008


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _is_transfer(w):
    """Unconditional control transfer without fall-through: jr (any rs), j, b (beq $0,$0), beql $0,$0."""
    if (w >> 26) == 0 and (w & 0x3F) == 8:
        return True
    if (w >> 26) == 2:
        return True
    if (w >> 16) in (0x1000, 0x5000):
        return True
    return False


def branch_target(w, pc):
    """PC-relative branch target of an R5900 word at pc, or None (beq/bne/blez/bgtz and -likely, REGIMM, BC0/1/2)."""
    op = w >> 26
    rs = (w >> 21) & 31
    rt = (w >> 16) & 31
    if op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or (op == 1 and rt in (0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13)) \
            or (op in (0x10, 0x11, 0x12) and rs == 8):
        imm = w & 0xFFFF
        if imm & 0x8000:
            imm -= 0x10000
        return (pc + 4 + (imm << 2)) & 0xFFFFFFFF
    return None


def cstr(b, o):
    e = b.index(b"\0", o)
    return b[o:e].decode("latin-1")


class Symbol:
    __slots__ = ("index", "name_off", "name", "addr", "hash", "type", "flag")

    def __init__(self, index, name_off, name, addr, hsh, typ, flag):
        self.index, self.name_off, self.name, self.addr = index, name_off, name, addr
        self.hash, self.type, self.flag = hsh, typ, flag

    @property
    def tname(self):
        return SYM_TYPES.get(self.type, "type%d" % self.type)


class Reloc:
    __slots__ = ("index", "offset", "type", "sym", "addend")

    def __init__(self, index, offset, typ, sym, addend):
        self.index, self.offset, self.type, self.sym, self.addend = index, offset, typ, sym, addend

    @property
    def tname(self):
        return RELOC_TYPES.get(self.type, "type%#x" % self.type)


class StreamEntry:
    __slots__ = ("pos", "offset", "kind", "absolute")

    def __init__(self, pos, offset, kind, absolute):
        self.pos, self.offset, self.kind, self.absolute = pos, offset, kind, absolute


class Module:
    """One SNR2 module image as stored on disc (module-relative offsets, nothing relocated)."""

    def __init__(self, raw: bytes, name: str = "?"):
        self.raw = bytes(raw)
        self.name = name
        if len(raw) < 0x3C:
            raise ValueError("%s: too short for an SNR2 header" % name)
        self.h = dict(zip(HDR_FIELDS, struct.unpack_from("<15I", raw, 0)))
        self.version = raw[3]
        self.symbols = self._parse_symbols()
        self.relocs = self._parse_relocs()
        self.stream, self.stream_end = self._parse_stream()
        self.image_name = cstr(raw, self.h["name_off"]) if self.h["name_off"] else ""

    # ---------------------------------------------------------------- validation (func_00393F80)
    def validate(self, load_address: int) -> int:
        """Return the game's validate code: 0 ok, 1 bad magic, 2 bad version, 4 misaligned."""
        if (self.h["magic"] & 0x00FFFFFF) != 0x524E53:
            return 1
        if ((self.version - 0x31) & 0xFFFFFFFF) >= 2:
            return 2
        if ((self.h["align"] - 1) & 0xFFFFFFFF) & load_address:
            return 4
        return 0

    # ---------------------------------------------------------------- tables
    def _parse_symbols(self):
        out = []
        fa, fc = self.h["funcs_off"], self.h["func_count"]
        for k in range(fc):
            na, addr, hsh, typ, flag = struct.unpack_from("<IIHBB", self.raw, fa + k * 12)
            out.append(Symbol(k, na, cstr(self.raw, na), addr, hsh, typ, flag))
        return out

    def _parse_relocs(self):
        out = []
        ra, rc = self.h["relocs_off"], self.h["reloc_count"]
        for k in range(rc):
            off, info, add = struct.unpack_from("<IIi", self.raw, ra + k * 12)
            out.append(Reloc(k, off, info & 0xFF, info >> 8, add))
        return out

    def _parse_stream(self):
        out = []
        so = self.h["stream_off"]
        if not so:
            return out, 0
        p, cur = so, 0
        while True:
            b = self.raw[p]
            pos = p
            p += 1
            if b & 0xFC:
                cur += b & 0xFC
                out.append(StreamEntry(pos, cur, b & 3, False))
            else:
                v = u32(self.raw, p)
                p += 4
                if v == 0:
                    return out, p
                cur = v
                out.append(StreamEntry(pos, cur, b & 3, True))

    # ---------------------------------------------------------------- derived layout
    def strings_start(self):
        offs = [s.name_off for s in self.symbols if s.name_off]
        return min(offs) if offs else self.h["name_off"]

    def insn_sites(self):
        """Module offsets of instruction words some relocation rewrites (stream lo16/hi16/j26 + reloc j26/hi16/lo16)."""
        s = {e.offset for e in self.stream if e.kind in (0, 1, 2)}
        s |= {r.offset for r in self.relocs if r.type in (4, 5, 6)}
        return s

    def word_sites(self):
        s = {e.offset for e in self.stream if e.kind == 3}
        s |= {r.offset for r in self.relocs if r.type == 2}
        return s

    def word(self, off):
        return u32(self.raw, off)

    def boundary_starts(self):
        """Module offsets known to begin a routine or object: defined symbols, j26 call targets, ctor, dtor."""
        s = {x.addr for x in self.symbols if x.type in (0, 2, 3) and x.addr}
        s |= self.internal_call_targets()
        s |= {self.h["ctor_off"], self.h["dtor_off"]} - {0}
        return s

    def code_range(self):
        """(start, end, evidence) of the EE code, module-relative.

        The linker output is .text first (the gap 0x3C..start is zero), then data. Upper bound B = first relocated
        data word (stream w32 / reloc w32) or the symbol strings. A linear sweep from `start` follows routines: a
        routine ends at a 'jr $ra' + delay slot past its furthest forward branch when the next word is zero, a known
        start, or B. The sweep continues past zero padding while relocated instructions / call targets / the ctor
        remain ahead, or while the next non-zero word is a symbol whose bytes end in a 'jr $ra' before B (relocation-
        free leaf routines such as the weak lexicographical_compare copies at the end of most room DLLs). It stops at
        the first non-zero word that is neither (data).
        """
        insn = self.insn_sites()
        words = self.word_sites()
        targets = self.internal_call_targets()
        starts = self.boundary_starts()
        start = 0x3C
        while start < len(self.raw) and self.word(start) == 0:
            start += 4
        B = min(words) if words else self.strings_start()
        evidence = max(list(insn) + list(targets) + [self.h["ctor_off"], self.h["dtor_off"], start])
        end = None
        pos = start
        while pos < B:
            e = self._extent(pos, B, starts)
            if e is None:
                if pos <= evidence:
                    raise ValueError("%s: routine at %#x has no end before the data bound %#x" % (self.name, pos, B))
                break
            end = e
            nxt = e
            while nxt < B and self.word(nxt) == 0:
                nxt += 4
            if nxt >= B or not (nxt <= evidence or nxt in starts):
                break
            pos = nxt
        if end is None:
            raise ValueError("%s: no code found" % self.name)
        ev = {"last_insn_site": max(insn) if insn else None, "last_call_target": max(targets) if targets else None,
              "first_w32_site": min(words) if words else None, "data_bound": B, "evidence_max": evidence}
        return start, end, ev

    def _extent(self, s, B, starts):
        furthest = s
        pc = s
        while pc + 4 <= B:
            if pc > s and pc in starts and pc >= furthest:
                return pc
            w = self.word(pc)
            t = branch_target(w, pc)
            if t is not None and s <= t < B:
                furthest = max(furthest, t)
            elif (w >> 26) == 2:                     # j: module-relative target; ignore tail jumps to known starts
                t = (w & 0x03FFFFFF) << 2
                if pc < t < B and t not in starts:
                    furthest = max(furthest, t)
            if w == JR_RA and pc + 8 > furthest:
                n = pc + 8
                if n >= B or self.word(n) == 0 or n in starts:
                    return n
            pc += 4
        return None

    def regions(self, code=None):
        """Contiguous (name, start, end) cover of [0, size): header, pad, code, data, strings, name, relocs, symtab,
        exports, stream, plus 'gap@...' for anything between tables."""
        h = self.h
        cs, ce, _ = code or self.code_range()
        marks = [("header", 0, 0x3C), ("pad", 0x3C, cs), ("code", cs, ce), ("data", ce, self.strings_start()),
                 ("strings", self.strings_start(), h["name_off"])]
        tables = [("name", h["name_off"], h["name_off"] + len(self.image_name) + 1),
                  ("relocs", h["relocs_off"], h["relocs_off"] + 12 * h["reloc_count"]),
                  ("symtab", h["funcs_off"], h["funcs_off"] + 12 * h["func_count"]),
                  ("exports", h["exports_off"], h["exports_off"] + 4 * h["export_count"]),
                  ("stream", h["stream_off"], self.stream_end)]
        tables = [t for t in tables if t[2] > t[1]]
        tables.sort(key=lambda t: t[1])
        out = [m for m in marks if m[2] > m[1]]
        cur = out[-1][2]
        for nm, a, b in tables:
            if a < cur:
                raise ValueError("%s: %s at %#x overlaps the previous region (ends %#x)" % (self.name, nm, a, cur))
            if a > cur:
                out.append(("gap@%x" % cur, cur, a))
            out.append((nm, a, b))
            cur = b
        if cur < len(self.raw):
            out.append(("tail", cur, len(self.raw)))
        return out

    def stream_hi_values(self):
        """[(hi_site, lo_site, module_offset)] for every stream hi16 entry, paired exactly like 0x394A14-0x394A80
        (forward scan to the next kind-0 entry), value = (hi << 16) + (int16)lo, module-relative (base 0)."""
        out = []
        ents = self.stream
        for i, e in enumerate(ents):
            if e.kind != 1:
                continue
            j = i + 1
            while j < len(ents) and ents[j].kind != 0:
                j += 1
            if j == len(ents):
                raise ValueError("%s: stream hi16 at %#x has no following lo16" % (self.name, e.offset))
            lo = struct.unpack_from("<h", self.raw, ents[j].offset)[0]
            hi = struct.unpack_from("<H", self.raw, e.offset)[0]
            out.append((e.offset, ents[j].offset, ((hi << 16) + lo) & 0xFFFFFFFF))
        return out

    def routine_start_verdict(self, p, prev_start, B):
        """Is code offset p the start of a routine (vs. a label inside the routine that begins at prev_start)?
        Yes when the last non-zero instruction pair before p is an unconditional transfer with no fall-through
        (jr, j, b / beq $0,$0 / beql $0,$0) and no branch of [prev_start, p) reaches p or beyond."""
        q = p - 4
        while q >= prev_start and self.word(q) == 0:
            q -= 4
        if q < prev_start:
            return False, "no code before it"
        cand = [q - 4, q]  # q is the delay slot, or q is the transfer itself with a nop delay slot
        if not any(_is_transfer(self.word(x)) for x in cand if x >= prev_start):
            return False, "falls through from module+%#x" % q
        furthest = 0
        for pc in range(prev_start, p, 4):
            t = branch_target(self.word(pc), pc)
            if t is not None and t < B:
                furthest = max(furthest, t)
        if furthest >= p:
            return False, "a branch of the routine at module+%#x reaches module+%#x" % (prev_start, furthest)
        return True, "after an unconditional transfer"

    def internal_call_targets(self):
        """Module offsets targeted by stream j26 entries (jal/j to own code)."""
        t = set()
        for e in self.stream:
            if e.kind == 2:
                t.add((self.word(e.offset) & 0x03FFFFFF) << 2)
        return t
