"""snr2 - the game's SNR2 DLL tool (list / extract / parse / static link / verify / emit for ps2_recomp).

Run with python -I (standard library only). Paths default to this port tree:
  ISO      KAMITE_ISO, else the first .iso in game/ (read-only)   --iso (before the command)
  rel dir  build/snr2/rel/                (extract output)      --rel-dir
  out dir  build/snr2/out/<module>/       (emit output)         --out
  boot ELF SLUS_215.03 from the ISO root  (import resolution)   --boot-elf

Commands
  list                         the 81 .rel entries of GODHAND.FST (AFS index, size, sha256)
  extract                      write all 81 to build/snr2/rel/ + manifest.json (idempotent)
  info MOD [--symbols] [--relocs] [--stream]
                               header, layout regions, counts, code range, symbol/reloc/stream dumps
  summary [--csv FILE]         one line per module: code range, counts, imports resolved vs boot, checks
  link MOD --base ADDR [--bin FILE]
                               emulate the game's loader (func_00394DD8 -> func_00394828) at ADDR
  verify MOD --base ADDR --ram DUMP [--first N]
                               link, then compare with an EE RAM dump (32 MB, offset = address) per region
  emit MOD --base ADDR         ELF + function CSV + runtime-relocation CSV (+ data-slot CSV, link report JSON)
MOD = a .rel path, or a module name (mcport, r207, event.rel ...) found in --rel-dir or read from the ISO.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # sibling modules (python -I leaves the script directory off sys.path)

import snr2disc  # noqa: E402
import snr2elf  # noqa: E402
from snr2fmt import JR_RA, Module, RELOC_TYPES, STREAM_KINDS, SYM_TYPES, branch_target, u32  # noqa: E402
from snr2link import LinkError, Registry, link  # noqa: E402

PORT = os.path.normpath(os.path.join(HERE, "..", ".."))
# KAMITE_ISO (as build.py) wins, so gen_modules.py's "snr2 emit" calls find the disc too.
def _first_iso():
    game = os.path.join(PORT, "game")
    isos = sorted(f for f in os.listdir(game) if f.lower().endswith(".iso")) if os.path.isdir(game) else []
    return os.path.join(game, isos[0]) if isos else os.path.join(game, "game.iso")


DEF_ISO = os.environ.get("KAMITE_ISO") or _first_iso()
DEF_REL = os.path.join(PORT, "build", "snr2", "rel")
DEF_OUT = os.path.join(PORT, "build", "snr2", "out")


# ------------------------------------------------------------------------------------------------ inputs
class Ctx:
    def __init__(self, a):
        self.a = a
        self._disc = None
        self._boot = None

    @property
    def disc(self):
        if self._disc is None:
            self._disc = snr2disc.Disc(self.a.iso)
        return self._disc

    def boot_elf(self):
        if self._boot is None:
            if getattr(self.a, "boot_elf", None):
                with open(self.a.boot_elf, "rb") as f:
                    self._boot = f.read()
            else:
                self._boot = self.disc.read_root_file("SLUS_215.03")
        return self._boot

    def registry(self):
        return Registry.from_boot_elf(self.boot_elf())

    def rel_names(self):
        return [r[0] for r in self.disc.rel_entries()]

    def load(self, spec) -> Module:
        if os.path.isfile(spec):
            with open(spec, "rb") as f:
                return Module(f.read(), os.path.basename(spec))
        nm = spec.lower()
        if not nm.endswith(".rel"):
            nm += ".rel"
        p = os.path.join(self.a.rel_dir, nm)
        if os.path.isfile(p):
            with open(p, "rb") as f:
                return Module(f.read(), nm)
        for name, _path, idx in self.disc.rel_entries():
            if name.lower() == nm:
                return Module(self.disc.read_afs(idx), name)
        raise SystemExit("unknown module %r (not a file, not in %s, not in GODHAND.FST)" % (spec, self.a.rel_dir))

    def all_modules(self):
        for name, _path, idx in self.disc.rel_entries():
            p = os.path.join(self.a.rel_dir, name)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    raw = f.read()
            else:
                raw = self.disc.read_afs(idx)
            yield idx, Module(raw, name)


def hx(v):
    return "0x%08X" % v


def parse_int(s):
    return int(s, 0)


# ------------------------------------------------------------------------------------------------ analysis
class Analysis:
    """Everything emit/summary need for one module at one base: code range, function list, reloc classification."""

    def __init__(self, m: Module, base: int, reg: Registry, all_exports=None, weak_dynamic=False):
        self.m = m
        self.base = base
        self.weak_dynamic = weak_dynamic
        self.code = m.code_range()
        self.cs, self.ce, self.cev = self.code
        self.L = link(m, base, reg)
        self.all_exports = all_exports or {}
        self.checks = self._checks()
        self.functions, self.aliases = self._functions()
        self.sites = self._classify_relocs()

    def _checks(self):
        m, cs, ce = self.m, self.cs, self.ce
        c = collections.OrderedDict()
        c["pad_zero"] = all(b == 0 for b in m.raw[0x3C:cs])
        ins = m.insn_sites()
        c["insn_sites_in_code"] = "%d/%d" % (sum(cs <= x < ce for x in ins), len(ins))
        tg = m.internal_call_targets()
        c["call_targets_in_code"] = "%d/%d" % (sum(cs <= x < ce for x in tg), len(tg))
        ws = m.word_sites()
        c["w32_sites_outside_code"] = "%d/%d" % (sum(not (cs <= x < ce) for x in ws), len(ws))
        c["ctor_in_code"] = cs <= m.h["ctor_off"] < ce
        c["dtor_in_code"] = cs <= m.h["dtor_off"] < ce
        c["last_code_word_is_jr_ra_pair"] = m.word(ce - 8) == JR_RA
        c["validate_code"] = self.L.validate_code
        ok = (c["pad_zero"] and c["ctor_in_code"] and c["dtor_in_code"] and c["validate_code"] == 0
              and c["insn_sites_in_code"].split("/")[0] == c["insn_sites_in_code"].split("/")[1]
              and c["call_targets_in_code"].split("/")[0] == c["call_targets_in_code"].split("/")[1]
              and c["w32_sites_outside_code"].split("/")[0] == c["w32_sites_outside_code"].split("/")[1])
        c["ok"] = ok
        return c

    def _functions(self):
        m, cs, ce, base = self.m, self.cs, self.ce, self.base
        pref = {2: 0, 4: 1, 3: 2, 0: 3}
        by_addr = collections.defaultdict(list)
        for s in m.symbols:
            if s.type in (0, 2, 3) and s.name and cs <= s.addr < ce:
                by_addr[s.addr].append(s)
        names = {}
        aliases = {}
        for a, lst in by_addr.items():
            lst.sort(key=lambda s: (pref[s.type], s.index))
            names[a] = (lst[0].name, "symbol:" + lst[0].tname)
            if len(lst) > 1:
                aliases[a] = [s.name for s in lst[1:]]
        for t in m.internal_call_targets():
            if cs <= t < ce and t not in names:
                names[t] = ("func_%08X" % (base + t), "j26-target")
        for nm, off in (("ctor", m.h["ctor_off"]), ("dtor", m.h["dtor_off"])):
            if off and cs <= off < ce and off not in names:
                names[off] = ("func_%08X" % (base + off), nm)
        if cs not in names:
            names[cs] = ("func_%08X" % (base + cs), "code-start")
        # code addresses loaded with lui/addiu (stream hi16+lo16) are pointers to routines (callbacks, task bodies)
        # or labels (sjlj landing pads, 'la' of the own pc); only routine starts are promoted
        self.pointer_verdicts = []
        for hi, lo, v in sorted(m.stream_hi_values(), key=lambda t: t[2]):
            if not (cs <= v < ce) or v in names:
                continue
            prev = max(a for a in names if a < v)
            ok, why = m.routine_start_verdict(v, prev, ce)
            self.pointer_verdicts.append((v, hi, ok, why))
            if ok:
                names[v] = ("func_%08X" % (base + v), "hilo-pointer")
        # w32 data words pointing into code. A run of >= 2 consecutive words whose targets all lie in one routine that
        # has a non-ra 'jr' is a jump table (case labels stay inside the routine). Every other code pointer in data is
        # a routine address (vtable slot, callback, libgcc EH hook such as McPort's __get_eh_info -> 0x0111B458) and is
        # promoted; ps2_recomp drops its own discoveries inside authoritative CSV ranges (elf_parser.cpp:1931-1935).
        self.jump_tables = []
        self.w32_promoted = []
        tmp = sorted(names)

        def owner_of(v):
            i = bisect.bisect_right(tmp, v) - 1
            return tmp[i] if i >= 0 else None

        def has_jr_reg(a):
            i = tmp.index(a)
            e = tmp[i + 1] if i + 1 < len(tmp) else ce
            return any((m.word(pc) >> 26) == 0 and (m.word(pc) & 0x3F) == 8 and ((m.word(pc) >> 21) & 31) != 31
                       for pc in range(a, e, 4))

        ptr_words = sorted((e.offset, m.word(e.offset)) for e in m.stream
                           if e.kind == 3 and cs <= m.word(e.offset) < ce)
        # GCC 2.9x exception tables (__EXCEPTION_TABLE__): {region start, region end, handler} triples ended by a
        # 0xFFFFFFFF word (McPort 0x01121400, 7 triples for __check_eh_spec / __throw_bad_cast / __throw_bad_typeid).
        # Region bounds and landing pads are labels, never routine starts.
        raw_runs = []
        for off, v in ptr_words:
            if raw_runs and off == raw_runs[-1][-1][0] + 4:
                raw_runs[-1].append((off, v))
            else:
                raw_runs.append([(off, v)])
        self.eh_tables = []
        eh_words = set()
        for run in raw_runs:
            end = run[-1][0] + 4
            if len(run) >= 3 and len(run) % 3 == 0 and end + 4 <= len(m.raw) and m.word(end) == 0xFFFFFFFF:
                self.eh_tables.append((run[0][0], len(run) // 3))
                eh_words |= {o for o, _v in run}
        runs = []
        # back-to-back jump tables of different routines are split where the owning routine changes
        for off, v in ptr_words:
            if off in eh_words:
                continue
            if runs and off == runs[-1][-1][0] + 4 and owner_of(v) == owner_of(runs[-1][-1][1]):
                runs[-1].append((off, v))
            else:
                runs.append([(off, v)])
        promote = set()
        for run in runs:
            owners = {owner_of(v) for _o, v in run}
            if len(run) >= 2 and len(owners) == 1 and None not in owners and has_jr_reg(next(iter(owners))):
                self.jump_tables.append((run[0][0], len(run), next(iter(owners))))
                continue
            promote |= {v for _o, v in run if v not in names}
        for v in sorted(promote):
            names[v] = ("func_%08X" % (base + v), "w32-pointer")
            self.w32_promoted.append(v)
        starts = sorted(names)
        out = []
        for i, a in enumerate(starts):
            e = starts[i + 1] if i + 1 < len(starts) else ce
            out.append({"name": names[a][0], "start": base + a, "end": base + e, "size": e - a, "source": names[a][1]})
        # w32 data words pointing into code (jump tables, vtables): reported, never promoted
        self.w32_code_ptrs = collections.Counter()
        for e in m.stream:
            if e.kind == 3:
                v = m.word(e.offset)
                if cs <= v < ce and v not in names:
                    self.w32_code_ptrs[v] += 1
        # branches that leave their function for a non-start address (a wrong split would show up here)
        sset = set(starts)
        self.branch_escapes = []
        for i, a in enumerate(starts):
            e = starts[i + 1] if i + 1 < len(starts) else ce
            for pc in range(a, e, 4):
                t = branch_target(m.word(pc), pc)
                if t is not None and not (a <= t < e) and t not in sset:
                    self.branch_escapes.append((pc, t))
        return out, aliases

    def function_at(self, addr):
        starts = [f["start"] for f in self.functions]
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and self.functions[i]["start"] <= addr < self.functions[i]["end"]:
            return self.functions[i]["name"]
        return ""

    def _classify_relocs(self):
        """Every symbol-table reloc site: (site, kind, symbol, sym_index, addend, resolution, dynamic, reason)."""
        m, L, base = self.m, self.L, self.base
        out = []
        for r in m.relocs:
            s = m.symbols[r.sym]
            res, hit = L.sym_resolution.get(r.sym, ("?", None))
            site = base + r.offset
            me = m.name.replace(".rel", "")
            strong_elsewhere = [d for d, t in self.all_exports.get(s.name, []) if t in (2, 4) and d != me]
            if s.type == 1:
                if res == "unresolved":
                    dyn, why = True, "import unresolved at static link (exported by: %s)" % (
                        ",".join("%s:%s" % (d, SYM_TYPES[t]) for d, t in self.all_exports.get(s.name, []))
                        or "no DLL")
                elif hit is not None and hit.type == 3:
                    dyn = bool(strong_elsewhere) or self.weak_dynamic
                    why = ("import resolved to a WEAK %s symbol; strong export in %s re-patches it when loaded"
                           % (res, ",".join(strong_elsewhere)) if strong_elsewhere else
                           "import resolved to a WEAK %s symbol; no DLL exports it strongly" % res)
                else:
                    dyn, why = False, "import resolved to %s %s" % (res, SYM_TYPES.get(hit.type) if hit else "?")
            elif s.type == 3:
                dyn = bool(strong_elsewhere) or self.weak_dynamic
                why = ("own weak symbol; strong export in %s re-patches it when loaded" % ",".join(strong_elsewhere)
                       if strong_elsewhere else "own weak symbol; no other DLL exports it strongly")
            else:
                dyn, why = False, "own %s symbol" % s.tname
            kind = {4: "j26", 5: "hi16", 6: "lo16", 2: "w32"}.get(r.type, RELOC_TYPES.get(r.type, str(r.type)))
            out.append({"address": site, "kind": kind, "symbol": s.name, "sym_index": r.sym, "addend": r.addend,
                        "sym_type": s.tname, "resolution": res, "resolved_to": L.sym_addr.get(r.sym, 0),
                        "dynamic": dyn, "reason": why, "in_code": self.cs <= r.offset < self.ce,
                        "placeholder": u32(L.img, r.offset)})
        return out


def export_index(ctx):
    """name -> [(module, type)] for every type 2/3/4 definition across the 81 DLLs."""
    idx = collections.defaultdict(list)
    for _i, m in ctx.all_modules():
        for s in m.symbols:
            if s.type in (2, 3, 4) and s.name:
                idx[s.name].append((m.name.replace(".rel", ""), s.type))
    return idx


# ------------------------------------------------------------------------------------------------ commands
def cmd_list(ctx):
    rows = ctx.disc.rel_entries()
    print("%-11s %-34s %5s %8s  %s" % ("name", "fst path", "afs", "size", "sha256"))
    for name, path, idx in rows:
        raw = ctx.disc.read_afs(idx)
        print("%-11s %-34s %5d %8d  %s" % (name, path, idx, len(raw), snr2disc.sha256(raw)))
    print("%d .rel entries, AFS indices %d..%d of %d" % (len(rows), rows[0][2], rows[-1][2], len(ctx.disc.afs_entries)))


def cmd_extract(ctx):
    out = ctx.a.rel_dir
    os.makedirs(out, exist_ok=True)
    man = []
    written = same = 0
    for name, path, idx in ctx.disc.rel_entries():
        raw = ctx.disc.read_afs(idx)
        p = os.path.join(out, name)
        if os.path.isfile(p) and open(p, "rb").read() == raw:
            same += 1
        else:
            tmp = p + ".tmp"
            with open(tmp, "wb") as f:
                f.write(raw)
            os.replace(tmp, p)
            written += 1
        man.append({"name": name, "fst_path": path, "afs_index": idx, "size": len(raw), "sha256": snr2disc.sha256(raw)})
    with open(os.path.join(out, "manifest.json"), "w", newline="\n") as f:
        json.dump(man, f, indent=1)
        f.write("\n")
    print("extracted %d .rel to %s (%d written, %d unchanged) + manifest.json" % (len(man), out, written, same))


def print_header(m):
    h = m.h
    print("%s: %s  version '%c'  size %#x (file %#x)" % (m.name, m.image_name, m.version, h["size"], len(m.raw)))
    for k in ("relocs_off", "reloc_count", "funcs_off", "func_count", "name_off", "ctor_off", "dtor_off",
              "exports_off", "export_count", "align", "size", "next", "stream_off", "field38"):
        print("  %-13s %#x" % (k, h[k]))


def cmd_info(ctx):
    m = ctx.load(ctx.a.module)
    print_header(m)
    code = m.code_range()
    print("code range %#x-%#x (%d bytes)  evidence %s" % (code[0], code[1], code[1] - code[0],
                                                          {k: (hex(v) if isinstance(v, int) else v) for k, v in code[2].items()}))
    print("regions:")
    for nm, a, b in m.regions(code):
        print("  %-12s %#7x-%#7x %7d" % (nm, a, b, b - a))
    print("symbols %d by type %s" % (len(m.symbols), dict(collections.Counter(s.tname for s in m.symbols))))
    print("relocs  %d by type %s" % (len(m.relocs), dict(collections.Counter(r.tname for r in m.relocs))))
    print("stream  %d entries by kind %s (%d absolute, %d delta), %d bytes" % (
        len(m.stream), dict(collections.Counter(STREAM_KINDS[e.kind] for e in m.stream)),
        sum(e.absolute for e in m.stream), sum(not e.absolute for e in m.stream), m.stream_end - m.h["stream_off"]))
    if ctx.a.symbols:
        print("symbols: idx type addr hash flag name")
        for s in m.symbols:
            print("  %4d %-6s %#7x %04x %d %s" % (s.index, s.tname, s.addr, s.hash, s.flag, s.name))
    if ctx.a.relocs:
        print("relocs: idx offset type sym addend symbol")
        for r in m.relocs:
            print("  %4d %#7x %-5s %4d %+d %s" % (r.index, r.offset, r.tname, r.sym, r.addend, m.symbols[r.sym].name))
    if ctx.a.stream:
        print("stream: pos offset kind form")
        for e in m.stream:
            print("  %#7x %#7x %-4s %s" % (e.pos, e.offset, STREAM_KINDS[e.kind], "abs" if e.absolute else "delta"))


def cmd_summary(ctx):
    exp = export_index(ctx)
    rows = []
    tot = collections.Counter()
    for idx, m in ctx.all_modules():
        reg = ctx.registry()
        try:
            an = Analysis(m, 0x01000000, reg, exp, ctx.a.weak_dynamic)  # base only matters for names; checks are base-independent
        except (LinkError, ValueError) as e:
            print("%-10s ERROR %s" % (m.name, e))
            tot["errors"] += 1
            continue
        imp = [s for s in m.symbols if s.type == 1]
        unres = sorted({s.name for s in imp if an.L.sym_resolution[s.index][0] == "unresolved"})
        weak_imp = sorted({s.name for s in imp if an.L.sym_resolution[s.index][1] is not None
                           and an.L.sym_resolution[s.index][1].type == 3})
        code_sites = [x for x in an.sites if x["in_code"] and x["kind"] in ("j26", "hi16", "lo16")]
        dyn = [x for x in code_sites if x["dynamic"]]
        tc = collections.Counter(s.type for s in m.symbols)
        row = collections.OrderedDict([
            ("module", m.name), ("afs_index", idx), ("size", len(m.raw)), ("code_start", an.cs), ("code_end", an.ce),
            ("code_bytes", an.ce - an.cs), ("functions", len(an.functions)),
            ("fn_from_symbols", sum(f["source"].startswith("symbol") for f in an.functions)),
            ("fn_from_j26", sum(f["source"] == "j26-target" for f in an.functions)),
            ("fn_from_pointers", sum(f["source"] == "hilo-pointer" for f in an.functions)),
            ("code_ptr_labels", len({v for v, _h, ok, _w in an.pointer_verdicts if not ok})),
            ("w32_code_ptrs", len(an.w32_code_ptrs)), ("branch_escapes", len(an.branch_escapes)),
            ("sym_local", tc[0]), ("sym_import", tc[1]), ("sym_export", tc[2]), ("sym_weak", tc[3]), ("sym_abs", tc[4]),
            ("relocs", len(m.relocs)), ("stream", len(m.stream)),
            ("imports_unresolved", len(unres)), ("imports_weak", len(weak_imp)),
            ("code_reloc_sites", len(code_sites)), ("runtime_sites", len(dyn)),
            ("checks_ok", an.checks["ok"]), ("unresolved_names", " ".join(unres))])
        rows.append(row)
        for k in ("code_bytes", "functions", "fn_from_symbols", "fn_from_j26", "fn_from_pointers", "code_ptr_labels",
                  "w32_code_ptrs", "branch_escapes", "sym_local", "sym_import", "sym_export", "sym_weak", "sym_abs",
                  "relocs", "stream", "code_reloc_sites", "runtime_sites", "imports_weak"):
            tot[k] += row[k]
        tot["modules"] += 1
        tot["checks_ok"] += bool(row["checks_ok"])
        tot["imports_unresolved_total"] += len(unres)
        print("%-10s afs %4d code %#6x-%#6x fn %4d (sym %4d j26 %3d ptr %2d) labels %d esc %d relocs %4d stream %4d "
              "imp %3d unres %2d weakimp %2d code-sites %4d runtime %3d checks %s%s" % (
                  m.name, idx, an.cs, an.ce, row["functions"], row["fn_from_symbols"], row["fn_from_j26"],
                  row["fn_from_pointers"], row["code_ptr_labels"], row["branch_escapes"], row["relocs"],
                  row["stream"], tc[1], len(unres), len(weak_imp), len(code_sites), len(dyn),
                  "ok" if an.checks["ok"] else "FAIL " + json.dumps(an.checks), "  unresolved: " + " ".join(unres)
                  if unres else ""))
    print("TOTAL", dict(tot))
    if ctx.a.csv:
        with open(ctx.a.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            for r in rows:
                w.writerow({k: (hx(v) if k in ("code_start", "code_end") else v) for k, v in r.items()})
        print("wrote", ctx.a.csv)


def cmd_link(ctx):
    m = ctx.load(ctx.a.module)
    L = link(m, ctx.a.base, ctx.registry())
    print("%s linked at %s: %d patches (%d failed), %d stream hi/lo pairs, %d cross-module re-patches" % (
        m.name, hx(ctx.a.base), len(L.patches), sum(1 for p in L.patches if not p[4]), len(L.hi_pairs),
        len(L.cross_patches)))
    unres = [m.symbols[k].name for k, (r, _h) in L.sym_resolution.items() if r == "unresolved"]
    print("imports unresolved against the boot ELF: %d %s" % (len(unres), " ".join(unres)))
    for c in L.cross_patches:
        print("  re-patch in %s: symbol #%d %s -> %s" % (c[0], c[1], c[2], hx(c[3])))
    if ctx.a.bin:
        with open(ctx.a.bin, "wb") as f:
            f.write(L.img)
        print("wrote", ctx.a.bin)


def cmd_verify(ctx):
    m = ctx.load(ctx.a.module)
    base = ctx.a.base
    an = Analysis(m, base, ctx.registry(), export_index(ctx), ctx.a.weak_dynamic)
    L = an.L
    with open(ctx.a.ram, "rb") as f:
        f.seek(base)
        ram = f.read(len(L.img))
    raw = m.raw
    print("verify %s at %s..%s (%d bytes) against %s" % (m.name, hx(base), hx(base + len(L.img)), len(L.img), ctx.a.ram))
    print("%-12s %-23s %8s %8s %8s %8s %8s" % ("region", "range", "bytes", "match", "differ", "relocd", "equalraw"))
    tot = collections.Counter()
    diffs = []
    for nm, a, b in m.regions(an.code):
        n = b - a
        same = sum(1 for i in range(a, b) if L.img[i] == ram[i])
        changed = sum(1 for i in range(a, b) if L.img[i] != raw[i])
        eqraw = sum(1 for i in range(a, b) if ram[i] == raw[i])
        print("%-12s %#9x-%#9x  %8d %8d %8d %8d %8d" % (nm, base + a, base + b, n, same, n - same, changed, eqraw))
        tot["bytes"] += n
        tot["match"] += same
        tot["relocated"] += changed
        for i in range(a, b):
            if L.img[i] != ram[i]:
                diffs.append((nm, i))
    print("%-12s %-23s %8d %8d %8d %8d" % ("TOTAL", "", tot["bytes"], tot["match"], tot["bytes"] - tot["match"],
                                          tot["relocated"]))
    print("columns: match = linked image == RAM; relocd = bytes the emulated loader changed vs the .rel file; "
          "equalraw = RAM bytes still equal to the .rel file")
    if diffs:
        print("first %d differing bytes (region, address, linked, ram, file):" % min(len(diffs), ctx.a.first))
        for nm, i in diffs[:ctx.a.first]:
            print("  %-10s %s linked %02x ram %02x file %02x" % (nm, hx(base + i), L.img[i], ram[i], raw[i]))
        words = sorted({i & ~3 for _n, i in diffs})
        print("differing words: %d" % len(words))
    # instruction-level summary for code
    code_words = (an.ce - an.cs) // 4
    bad = [a for a in range(an.cs, an.ce, 4) if L.img[a:a + 4] != ram[a:a + 4]]
    print("code words: %d, exact %d, differing %d" % (code_words, code_words - len(bad), len(bad)))
    print("code patches applied: %d (stream %d, symbol relocs %d)" % (
        sum(1 for p in L.patches if an.cs <= p[0] - base < an.ce),
        sum(1 for p in L.patches if an.cs <= p[0] - base < an.ce and p[1] == "stream"),
        sum(1 for p in L.patches if an.cs <= p[0] - base < an.ce and p[1] == "reloc")))
    # registered flags in RAM vs emulation (this module and module #0)
    fa = L.rw(base + 0x0C)
    fl_bad = [k for k in range(len(m.symbols)) if ram[fa - base + k * 12 + 0xB] != L.sym_flag[k]]
    print("symbol flags equal to RAM: %d/%d" % (len(m.symbols) - len(fl_bad), len(m.symbols)))
    # module #0 (boot ELF): registered flags and the import sites this module's exports re-patched (func_003943D0)
    with open(ctx.a.ram, "rb") as f:
        fullram = f.read()
    reg = ctx.registry()
    boot = reg.modules[0][1]
    bfa = struct.unpack_from("<I", fullram, 0x40A400 + 0x0C)[0]
    bflag_ok = sum(1 for s in boot if fullram[bfa + s.index * 12 + 0xB] == s.flag)
    print("boot symbol flags (emulated registration) equal to RAM: %d/%d" % (bflag_ok, len(boot)))
    elf = ctx.boot_elf()
    segs = []
    phoff = struct.unpack_from("<I", elf, 0x1C)[0]
    for i in range(struct.unpack_from("<H", elf, 0x2C)[0]):
        t, o, va, _p, fs = struct.unpack_from("<5I", elf, phoff + i * 32)
        if t == 1:
            segs.append((va, o, fs))

    def elf_word(va):
        for v0, o, fs in segs:
            if v0 <= va < v0 + fs:
                return struct.unpack_from("<I", elf, o + va - v0)[0]
        return None
    n = ok = 0
    for mod, symidx, sname, val in L.cross_patches:
        if mod != "BOOT":
            continue
        for r_off, info, add in reg.boot_relocs:
            if info >> 8 != symidx:
                continue
            w = elf_word(r_off)
            t = info & 0xFF
            v = (val + add) & 0xFFFFFFFF
            if t == 2:
                exp = v
            elif t == 4:
                exp = (w & 0xFC000000) | ((v >> 2) & 0x03FFFFFF)
            elif t == 5:
                exp = (w & 0xFFFF0000) | (((v + 0x8000) & 0xFFFFFFFF) >> 16)
            elif t == 6:
                exp = (w & 0xFFFF0000) | (v & 0xFFFF)
            else:
                continue
            n += 1
            got = struct.unpack_from("<I", fullram, r_off)[0]
            ok += got == exp
            if got != exp:
                print("  boot site %s (%s, type %d) expected %08x ram %08x" % (hx(r_off), sname, t, exp, got))
    print("boot import sites re-patched by this module's exports: %d, equal to RAM: %d" % (n, ok))
    return 0 if not diffs else 1


def cmd_emit(ctx):
    m = ctx.load(ctx.a.module)
    base = ctx.a.base
    stem = m.name.replace(".rel", "")
    out = ctx.a.out or os.path.join(DEF_OUT, stem)
    os.makedirs(out, exist_ok=True)
    an = Analysis(m, base, ctx.registry(), export_index(ctx), ctx.a.weak_dynamic)
    if not an.checks["ok"]:
        raise SystemExit("%s: code-range checks failed, not emitting: %s" % (m.name, json.dumps(an.checks)))
    L = an.L
    cs, ce = an.cs, an.ce
    ss = m.strings_start()
    sections = [(".snr2.header", 0, cs, "A"), (".text", cs, ce, "AX"), (".data", ce, ss, "WA"),
                (".snr2.meta", ss, len(L.img), "A")]
    elf = snr2elf.build_exec_elf(bytes(L.img), base, base + m.h["ctor_off"], sections)
    elf_path = os.path.join(out, stem + ".elf")
    with open(elf_path, "wb") as f:
        f.write(elf)
    snr2elf.self_check(elf, bytes(L.img), base, sections)

    # functions
    fn_path = os.path.join(out, stem + "_functions.csv")
    fns = an.functions
    for i, f in enumerate(fns):
        assert f["end"] > f["start"] and f["start"] % 4 == 0 and f["end"] % 4 == 0, f
        if i:
            assert fns[i - 1]["end"] == f["start"], (fns[i - 1], f)
    assert fns[0]["start"] == base + cs and fns[-1]["end"] == base + ce
    names = collections.Counter(f["name"] for f in fns)
    dup = [n for n, c in names.items() if c > 1]
    with open(fn_path, "w", newline="") as f:
        f.write("Name,Start,End,Size\n")
        for fn in fns:
            f.write("%s,%s,%s,%d\n" % (fn["name"], hx(fn["start"]), hx(fn["end"]), fn["size"]))

    # runtime relocations (code sites whose target is not fixed at static-link time)
    rr = [x for x in an.sites if x["in_code"] and x["kind"] in ("j26", "hi16", "lo16") and x["dynamic"]]
    rr_path = os.path.join(out, stem + "_runtime_relocations.csv")
    with open(rr_path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["Address", "kind", "module", "symbol", "sym_index", "addend", "placeholder", "function", "reason"])
        for x in sorted(rr, key=lambda x: x["address"]):
            w.writerow([hx(x["address"]), x["kind"], stem.upper(), x["symbol"], x["sym_index"], x["addend"],
                        hx(x["placeholder"]), an.function_at(x["address"]), x["reason"]])
    # data slots (w32 reloc sites; they stay in guest RAM, listed for review)
    ds = [x for x in an.sites if x["kind"] == "w32"]
    ds_path = os.path.join(out, stem + "_data_slots.csv")
    with open(ds_path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["Address", "kind", "symbol", "sym_type", "addend", "static_value", "dynamic", "reason"])
        for x in sorted(ds, key=lambda x: x["address"]):
            w.writerow([hx(x["address"]), x["kind"], x["symbol"], x["sym_type"], x["addend"], hx(x["placeholder"]),
                        int(x["dynamic"]), x["reason"]])
    bin_path = os.path.join(out, stem + "_linked.bin")
    with open(bin_path, "wb") as f:
        f.write(L.img)

    code_sites = [x for x in an.sites if x["in_code"] and x["kind"] in ("j26", "hi16", "lo16")]
    hi_code = [(h, lo, v) for h, lo, v in L.hi_pairs if base + cs <= v < base + ce]
    report = collections.OrderedDict([
        ("module", m.name), ("image_name", m.image_name), ("base", hx(base)), ("size", len(L.img)),
        ("validate_code", L.validate_code),
        ("code", [hx(base + cs), hx(base + ce)]), ("code_evidence", {k: (hx(base + v) if isinstance(v, int) else v)
                                                                     for k, v in an.cev.items()}),
        ("sections", [[n, hx(base + a), hx(base + b), fl] for n, a, b, fl in sections]),
        ("entry", hx(base + m.h["ctor_off"])), ("ctor", hx(base + m.h["ctor_off"])),
        ("dtor", hx(base + m.h["dtor_off"])),
        ("checks", an.checks),
        ("functions", len(fns)),
        ("function_sources", dict(collections.Counter(f["source"] for f in fns))),
        ("duplicate_names", dup),
        ("aliases", {hx(base + a): v for a, v in an.aliases.items()}),
        ("symbols_by_type", dict(collections.Counter(s.tname for s in m.symbols))),
        ("relocs_by_type", dict(collections.Counter(x["kind"] for x in an.sites))),
        ("code_reloc_sites", len(code_sites)),
        ("runtime_relocations", len(rr)),
        ("runtime_by_reason", dict(collections.Counter(x["reason"].split(" (")[0] for x in rr))),
        ("runtime_by_kind", dict(collections.Counter(x["kind"] for x in rr))),
        ("data_slots", len(ds)), ("data_slots_dynamic", sum(x["dynamic"] for x in ds)),
        ("imports_unresolved", sorted({m.symbols[k].name for k, (r, _h) in L.sym_resolution.items()
                                       if r == "unresolved"})),
        ("imports_resolved_weak", sorted({m.symbols[k].name for k, (r, h) in L.sym_resolution.items()
                                          if h is not None and h.type == 3})),
        ("stream_entries", len(m.stream)), ("stream_hi_pairs", len(L.hi_pairs)),
        ("stream_hi_pairs_into_code", len(hi_code)),
        ("code_pointers_promoted", sorted({hx(base + v) for v, _h, ok, _w in an.pointer_verdicts if ok})),
        ("code_pointers_not_promoted", sorted({"%s (la at %s): %s" % (hx(base + v), hx(base + h), w)
                                               for v, h, ok, w in an.pointer_verdicts if not ok})),
        ("w32_code_pointers_not_function_starts", len(an.w32_code_ptrs)),
        ("w32_pointers_promoted", [hx(base + v) for v in an.w32_promoted]),
        ("jump_tables", len(an.jump_tables)),
        ("eh_tables", ["%s x%d" % (hx(base + o), n) for o, n in an.eh_tables]),
        ("jump_table_list", ["%s x%d in %s" % (hx(base + o), n, hx(base + f)) for o, n, f in an.jump_tables]),
        ("branch_escapes", ["%s -> %s" % (hx(base + a), hx(base + t)) for a, t in an.branch_escapes]),
        ("cross_module_repatches", [[c[0], c[1], c[2], hx(c[3])] for c in L.cross_patches]),
        ("files", [elf_path, fn_path, rr_path, ds_path, bin_path]),
    ])
    rep_path = os.path.join(out, stem + "_link.json")
    with open(rep_path, "w", newline="\n") as f:
        json.dump(report, f, indent=1)
        f.write("\n")
    print(json.dumps({k: report[k] for k in ("module", "base", "code", "entry", "functions", "function_sources",
                                              "duplicate_names", "code_reloc_sites", "runtime_relocations",
                                              "runtime_by_reason", "runtime_by_kind", "data_slots", "imports_unresolved",
                                              "imports_resolved_weak", "code_pointers_promoted",
                                              "code_pointers_not_promoted", "w32_code_pointers_not_function_starts", "w32_pointers_promoted", "jump_tables", "eh_tables",
                                              "branch_escapes")}, indent=1))
    print("cross-module re-patches (this module's exports fill other modules' import sites): %d"
          % len(L.cross_patches))
    print("wrote:", *report["files"], rep_path, sep="\n  ")


def main(argv=None):
    p = argparse.ArgumentParser(prog="snr2", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--iso", default=DEF_ISO)
    p.add_argument("--rel-dir", default=DEF_REL)
    p.add_argument("--boot-elf", default=None)
    p.add_argument("--weak-dynamic", action="store_true",
                   help="treat every weak-symbol site as runtime-relocated, even when no DLL exports the name strongly")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("extract")
    s = sub.add_parser("info")
    s.add_argument("module")
    s.add_argument("--symbols", action="store_true")
    s.add_argument("--relocs", action="store_true")
    s.add_argument("--stream", action="store_true")
    s = sub.add_parser("summary")
    s.add_argument("--csv")
    s = sub.add_parser("link")
    s.add_argument("module")
    s.add_argument("--base", type=parse_int, required=True)
    s.add_argument("--bin")
    s = sub.add_parser("verify")
    s.add_argument("module")
    s.add_argument("--base", type=parse_int, required=True)
    s.add_argument("--ram", required=True)
    s.add_argument("--first", type=int, default=40)
    s = sub.add_parser("emit")
    s.add_argument("module")
    s.add_argument("--base", type=parse_int, required=True)
    s.add_argument("--out")
    a = p.parse_args(argv)
    ctx = Ctx(a)
    rc = {"list": cmd_list, "extract": cmd_extract, "info": cmd_info, "summary": cmd_summary, "link": cmd_link,
          "verify": cmd_verify, "emit": cmd_emit}[a.cmd](ctx)
    return rc or 0


if __name__ == "__main__":
    sys.exit(main())
