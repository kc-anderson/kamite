"""Live enemies in a case's RAM snapshots (<case>.60.snap.bin): skeleton hash (as punish_fit.cpp), body group,
+0x2FE, +0x564, size; then the byte offsets that hold a model id of the right skeleton in every enemy (the hunt for the
model-file field, Nov 8: +0x2FE is not it; +0x1648 matched only by chance). Usage: find_model_field.py <snap.bin>"""
import sys, json, struct, numpy as np
import os; HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,os.path.join(HERE,'..','..','fps60')); sys.path.insert(0,HERE)
from ram_snapshots import snapshots
B=json.load(open(os.path.join(HERE,'bodies.json')))
key2g={k:g for g,b in enumerate(B) for k in b['keys']}
snaps=snapshots(sys.argv[1])
print(len(snaps),'snapshots')
def run(ram):
    mem=ram.view(np.uint8)
    rd32=lambda a: int(ram[(a&0x1FFFFFF)//4])
    rdf=lambda a: float(ram[(a&0x1FFFFFF)//4:(a&0x1FFFFFF)//4+1].view(np.float32)[0])
    def skel(e):
        n=mem[(e+0x2B4)&0x1FFFFFF]
        if n<2 or n>64: return None
        parts=[]; p=rd32(e+0x144)
        for i in range(n):
            parts.append(p&0x1FFFFFF); p=rd32(p+0x144)
        h=2166136261
        def mix(bs):
            nonlocal h
            for b in bs: h=((h^b)*16777619)&0xFFFFFFFF
        mix(struct.pack('<i',int(n)))
        for i in range(1,n):
            par=rd32(parts[i]+0x148)&0x1FFFFFF; idx=-1
            for j in range(n):
                if parts[j]==par and j!=i: idx=j
            mix(struct.pack('<b',idx)); mix(bytes(mem[parts[i]+0xE0:parts[i]+0xEC]))
        return h or 1
    out=[]
    slot=rd32(0x5864F0+0x500)
    for g in range(64):
        if slot<0x100000 or slot>=0x2000000: break
        e=rd32(slot+8)&0x1FFFFFF; slot=rd32(slot+4)
        if e<0x100000: continue
        h=skel(e); grp=key2g.get('%08x'%h) if h else None
        out.append((e,h,grp,int(mem[e+0x2FE])|int(mem[e+0x2FF])<<8,rd32(e+0x564)&0xFFFF,rdf(e+0x114)))
    return out
seen={}
for fr,ram in snaps[::5]:
    for e,h,grp,k2fe,k564,s in run(ram):
        seen[e]=(h,grp,k2fe,k564,s,ram)
print(len(seen),'enemies')
cand=None
for e,(h,grp,k2fe,k564,s,ram) in seen.items():
    files={int(f[2:],16) for f in B[grp]['files']} if grp is not None else None
    print(hex(e),'%08x'%h if h else None,'group',grp,'2FE %x'%k2fe,'564 %x'%k564,'size %.2f'%s, B[grp]['files'][:6] if grp is not None else '')
    if files is None: continue
    mem=ram.view(np.uint8)
    offs={o for o in range(0,0x1800) if mem[(e+o)&0x1FFFFFF] in files}
    cand=offs if cand is None else cand&offs
print('byte offsets holding a model id of the right skeleton in every enemy:',sorted(hex(o) for o in cand)[:40] if cand else cand)
for e,(h,grp,k2fe,k564,s,ram) in seen.items():
    mem=ram.view(np.uint8)
    print(hex(e),grp,'+0x1648: %02x  u16 %04x  u32 %08x'%(mem[e+0x1648],int(mem[e+0x1648])|int(mem[e+0x1649])<<8,int.from_bytes(bytes(mem[e+0x1648:e+0x164C]),'little')),
          'around:',bytes(mem[e+0x1640:e+0x1650]).hex())
