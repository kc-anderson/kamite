"""Static re-implementation of the game's SNR2 loader (boot ELF func_00394DD8 -> func_00394828), for one module.

Everything is emulated on a byte image laid out exactly as the game has it in EE RAM (module loaded at `base`, header at
offset 0), so the result can be compared byte-for-byte with a RAM dump. Cross-module side effects (an export of this
module re-patching an already-linked module's import sites through func_003943D0) are recorded, not applied.
"""
from __future__ import annotations

import struct

from snr2fmt import HDR_RELOCATED, Module, u32

M32 = 0xFFFFFFFF


class LinkError(Exception):
    def __init__(self, code, msg):
        super().__init__("load would fail with code %d: %s" % (code, msg))
        self.code = code


class RegSym:
    """A symbol-table entry of a module already in the game's module list (what func_00393FD8 & co. walk)."""
    __slots__ = ("module", "index", "name", "addr", "hash", "type", "flag")

    def __init__(self, module, index, name, addr, hsh, typ, flag):
        self.module, self.index, self.name, self.addr = module, index, name, addr
        self.hash, self.type, self.flag = hsh, typ, flag


class Registry:
    """The game's module list (head at 0x3F852C) reduced to what linking needs: each module's symbol entries."""

    def __init__(self):
        self.modules = []  # [(module_name, [RegSym])]

    def add_module(self, name, syms):
        self.modules.append((name, syms))

    def all(self):
        for _, syms in self.modules:
            yield from syms

    def lookup(self, name, hsh):
        """func_00393FD8: first registered (flag != 0) entry with equal hash and name, in list order; 0 if none."""
        for s in self.all():
            if s.flag and s.hash == hsh and s.name == name:
                return s
        return None

    @staticmethod
    def from_boot_elf(elf: bytes, sndata_va=0x40A400):
        """Module #0 = the boot ELF's own .sndata SNR2 header. Its registered set is reproduced with the same rules
        func_003944E8 / func_003945C0 apply (types 2/4 register unless an earlier strong one exists, type 3 unless any
        registered one exists). The RAM dump of 2026-10-17 has flag=1 on all 1,064 type 2/3/4 entries."""
        segs = _elf_load_segments(elf)

        def rd(va, n):
            for va0, off, fsz in segs:
                if va0 <= va and va + n <= va0 + fsz:
                    return elf[off + va - va0: off + va - va0 + n]
            raise ValueError("boot ELF has no file bytes at %#x" % va)

        def rstr(va):
            out = bytearray()
            while True:
                c = rd(va + len(out), 1)[0]
                if c == 0:
                    return out.decode("latin-1")
                out.append(c)

        h = struct.unpack("<15I", rd(sndata_va, 60))
        if (h[0] & 0xFFFFFF) != 0x524E53:
            raise ValueError("no SNR2 header at %#x in the boot ELF" % sndata_va)
        fa, fc = h[3], h[4]
        reg = Registry()
        syms = []
        reg.add_module("BOOT", syms)
        for k in range(fc):
            na, addr, hsh, typ, flag = struct.unpack("<IIHBB", rd(fa + k * 12, 12))
            syms.append(RegSym("BOOT", k, rstr(na) if na else "", addr, hsh, typ, 0))
        for s in syms:
            if s.type in (2, 4):
                hit = reg.lookup(s.name, s.hash)
                if hit is None or hit.type not in (2, 4):
                    if hit is not None:
                        hit.flag = 0
                    s.flag = 1
            elif s.type == 3:
                if reg.lookup(s.name, s.hash) is None:
                    s.flag = 1
        reg.boot_relocs = [struct.unpack("<IIi", rd(h[1] + k * 12, 12)) for k in range(h[2])]
        return reg


def _elf_load_segments(elf):
    if elf[:4] != b"\x7fELF":
        raise ValueError("not an ELF")
    phoff = u32(elf, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    segs = []
    for i in range(phnum):
        p_type, p_off, p_va, _pa, p_fsz = struct.unpack_from("<5I", elf, phoff + i * phentsize)
        if p_type == 1:
            segs.append((p_va, p_off, p_fsz))
    return segs


class Linked:
    """Result of linking one module at `base`."""

    def __init__(self, module: Module, base: int):
        self.module = module
        self.base = base
        self.img = bytearray(module.raw)
        self.sym_addr = {}            # index -> final address in the symbol table entry
        self.sym_flag = {}            # index -> final flag byte
        self.sym_resolution = {}      # index -> ("own"|"boot"|"unresolved"|"abs", RegSym or None)
        self.patches = []             # (abs_addr, how, rtype, value, ok, detail)
        self.cross_patches = []       # (module_name, abs_addr, rtype, value, symbol) applied to OTHER modules
        self.hi_pairs = []            # (hi_site, lo_site, value) for stream hi16 entries
        self.validate_code = module.validate(base)

    # --------------------------------------------------------------- helpers
    def off(self, addr):
        o = (addr - self.base) & M32
        if o >= len(self.img):
            return None
        return o

    def rw(self, addr):
        return u32(self.img, self.off(addr))

    def ww(self, addr, v):
        struct.pack_into("<I", self.img, self.off(addr), v & M32)

    def rh(self, addr):
        return struct.unpack_from("<H", self.img, self.off(addr))[0]

    def rhs(self, addr):
        return struct.unpack_from("<h", self.img, self.off(addr))[0]

    def wh(self, addr, v):
        struct.pack_into("<H", self.img, self.off(addr), v & 0xFFFF)

    # --------------------------------------------------------------- patch() 0x394088
    def patch(self, addr, rtype, value, how, detail=""):
        """Exact port of patch() 0x394088-0x39428C. Returns 1 on success, 0 on failure (as the game does)."""
        value &= M32
        if self.off(addr) is None and rtype != 0:
            raise LinkError(10, "patch site %#x outside the module image" % addr)
        ok = 1
        if rtype == 0:
            pass
        elif rtype == 1:                                   # 0x3941B4: sh value
            self.wh(addr, value)
        elif rtype == 2:                                   # 0x394128: sw value
            self.ww(addr, value)
        elif rtype == 4:                                   # 0x394130: j26
            w = self.rw(addr)
            if (w & 0xFC00003F) == 0xD:
                ok = 0
            else:
                self.ww(addr, (w & 0xFC000000) | ((value >> 2) & 0x03FFFFFF))
        elif rtype == 5:                                   # 0x394164: hi16 = (value + 0x8000) >> 16
            if (self.rw(addr) & 0xFC00003F) == 0xD:
                ok = 0
            else:
                self.wh(addr, ((value + 0x8000) & M32) >> 16)
        elif rtype == 6:                                   # 0x394198: lo16
            if (self.rw(addr) & 0xFC00003F) == 0xD:
                ok = 0
            else:
                self.wh(addr, value)
        elif rtype == 0x77:                                # 0x3941BC
            w = self.rw(addr)
            self.ww(addr, (w & 0xFFE0003F) | ((value << 3) & 0x001FFFC0))
        elif rtype == 0x78:                                # 0x3941E8
            w = self.rw(addr)
            self.ww(addr, (w & 0xFFFFF800) | ((((value - addr) & M32) >> 3) & 0x7FF))
        elif rtype == 0x79:                                # 0x394208
            w = self.rw(addr)
            self.ww(addr, (w & 0x8000000F) | (value & 0x7FFFFFF0))
        elif rtype == 0x7A:                                # 0x394230
            w = self.rw(addr)
            self.ww(addr, (w & 0xFFFFF800) | ((value >> 4) & 0x7FF))
        elif rtype == 0x7B:                                # 0x394254
            w = self.rw(addr)
            v = value >> 3
            self.ww(addr, (w & 0xFE1FF800) | (v & 0x7FF) | ((v & 0x7800) << 10))
        else:
            ok = 0
        self.patches.append((addr, how, rtype, value, ok, detail))
        return ok


def link(module: Module, base: int, registry: Registry, check_align=True) -> Linked:
    """func_00394DD8 up to (not including) the ctor call: validate, func_00394828 relocate, append to the list."""
    L = Linked(module, base)
    if check_align and L.validate_code:
        raise LinkError(L.validate_code, "func_00393F80 rejects %s at %#x" % (module.name, base))
    H = 0  # header at offset 0 == base

    # 0x394848-0x3948C8: header pointers += base when non-zero; 0x3948D0: next = 0
    for fo in HDR_RELOCATED:
        v = L.rw(base + fo)
        if v:
            L.ww(base + fo, v + base)
    L.ww(base + 0x30, 0)

    fa = L.rw(base + 0x0C)
    fc = L.rw(base + 0x10)
    syms = module.symbols
    cur_regsyms = []
    # 0x3948D4-0x3949A4: symbol table
    for k in range(fc):
        e = fa + k * 12
        L.ww(e, L.rw(e) + base)                 # name pointer += base (unconditional)
        L.img[L.off(e + 0xB)] = 0              # flag = 0
        typ = L.img[L.off(e + 0xA)]
        s = syms[k]
        if typ >= 5:
            raise LinkError(7, "symbol %d %r has type %d" % (k, s.name, typ))
        if typ == 0:                            # 0x39491C
            L.ww(e + 4, L.rw(e + 4) + base)
            L.sym_resolution[k] = ("own", None)
        elif typ == 1:                          # 0x394950: addr = func_00393FD8(name, hash)
            hit = registry.lookup(s.name, s.hash)
            L.ww(e + 4, hit.addr if hit else 0)
            L.sym_resolution[k] = (("boot" if hit.module == "BOOT" else "module:" + hit.module), hit) if hit \
                else ("unresolved", None)
        elif typ in (2, 3, 4):
            if typ != 4:
                L.ww(e + 4, L.rw(e + 4) + base)
            L.sym_resolution[k] = ("own" if typ != 4 else "abs", None)
            ok = _register(L, registry, k, e, typ, s, cur_regsyms)
            if not ok:
                raise LinkError(8 if typ == 2 else 9, "export %r failed to register" % s.name)

    # 0x3949A8-0x394B28: internal relocation stream
    so = L.rw(base + 0x34)
    if so:
        p = so
        cur = 0
        while True:
            b = L.img[L.off(p)]
            kind = b & 3
            p += 1
            if b & 0xFC:
                cur = (cur + (b & 0xFC)) & M32
            else:
                v = L.rw(p)
                p += 4
                if v == 0:
                    break
                cur = (v + base) & M32
            if kind == 0:                       # 0x3949E0/0x394A08: lo16
                L.patch(cur, 6, base + L.rh(cur), "stream")
            elif kind == 1:                     # 0x394A14: hi16 with forward scan for the next kind-0 entry
                a3 = (L.rh(cur) << 16) & M32
                q, a0 = p, cur
                while True:
                    bb = L.img[L.off(q)]
                    q += 1
                    t0 = bb & 3
                    if bb & 0xFC:
                        a0 = (a0 + (bb & 0xFC)) & M32
                    else:
                        v = L.rw(q)
                        q += 4
                        if v == 0:
                            raise LinkError(10, "stream hi16 at %#x has no following lo16" % cur)
                        a0 = (v + base) & M32
                    if t0 == 0:
                        break
                a3 = (a3 + L.rhs(a0)) & M32
                L.hi_pairs.append((cur, a0, (base + a3) & M32))
                L.patch(cur, 5, base + a3, "stream", "lo@%#x" % a0)
            elif kind == 2:                     # 0x394AA8: j26, target = base + (field << 2)
                L.patch(cur, 4, base + ((L.rw(cur) & 0x03FFFFFF) << 2), "stream")
            else:                               # 0x394AD4: w32
                L.patch(cur, 2, base + L.rw(cur), "stream")

    # 0x394B2C-0x394B88: symbol relocations; r_offset += base is stored back
    ra = L.rw(base + 0x04)
    rc = L.rw(base + 0x08)
    for k in range(rc):
        e = ra + k * 12
        site = (base + L.rw(e)) & M32
        L.ww(e, site)
        info = L.rw(e + 4)
        add = L.rw(e + 8)
        sym = info >> 8
        val = (L.rw(fa + sym * 12 + 4) + add) & M32
        if not L.patch(site, info & 0xFF, val, "reloc", "sym %d" % sym):
            raise LinkError(10, "reloc %d at %#x (type %d) rejected by patch()" % (k, site, info & 0xFF))

    # 0x394B8C-0x394BB0: export pointer table
    ea = L.rw(base + 0x20)
    for k in range(L.rw(base + 0x24)):
        v = L.rw(ea + 4 * k)
        if v:
            L.ww(ea + 4 * k, v + base)

    for k in range(fc):
        L.sym_addr[k] = L.rw(fa + k * 12 + 4)
        L.sym_flag[k] = L.img[L.off(fa + k * 12 + 0xB)]
    registry.add_module(module.name, cur_regsyms)
    return L


def _register(L, registry, k, e, typ, s, cur_regsyms):
    """func_003944E8 (types 2, 4) / func_003945C0 (type 3), then func_003943D0 when the symbol gets registered."""
    me = RegSym(L.module.name, k, s.name, L.rw(e + 4), s.hash, typ, 0)
    cur_regsyms.append(me)
    hits = [r for r in registry.all() if r.flag and r.hash == s.hash and r.name == s.name]
    if typ in (2, 4):
        for r in hits:
            if r.type in (2, 4):
                return 1                          # an earlier strong one wins; ours stays unregistered
            r.flag = 0                            # 0x39456C: an earlier weak one is unregistered
            break
    else:
        if hits:
            return 1                              # 0x394628: anything registered wins over a weak one
    # func_003943D0: flag = 1, then re-patch every listed module's type-1/type-3 entries with this name
    L.img[L.off(e + 0xB)] = 1
    me.flag = 1
    for mname, syms in registry.modules:
        for r in syms:
            if r.hash == s.hash and r.name == s.name and r.type in (1, 3):
                L.cross_patches.append((mname, r.index, s.name, me.addr))
    return 1
