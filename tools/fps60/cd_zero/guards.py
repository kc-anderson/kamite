from census import *
from sites import sites
segs,rd=load_elf(SLUS)
ins=mk(rd)
G=[0x1148cc,0x11b7d0,0x25fc98,0x1cbc6c,0x209fa0,0x218b28,0x237214,0x237558,0x23784c,0x237c2c,0x238268,0x239060,0x2393cc,0x25bf68,0x2472f0,0x1bbe10,0x1a7b98,0x1af644,0x1b07e0,0x1b8f2c,0x1bf7a8,0x17b28c,0x17b4f8,0x17e484,0x17e618,0x1a495c,0x1a4b20]
hooked={a for m,a,h,_ in sites() if m=='kamite'}
import tomllib
allh={int(h['address'],16) for h in tomllib.load(open(__import__('os').path.join(__import__('os').path.dirname(__file__), '../../../recomp/kamite.toml'),'rb'))['patches']['hooks']}
out=[]
for g in G:
    w=rd(g); op,base,rt,_,off=fields(w)
    assert op in (0x20,0x21), hex(g)
    b=rd(g+4); bn=ins(g+4).getOpcodeName(); bs=(b>>21)&31
    assert bs==rt and is_cbranch(b), (hex(g),bn)
    T={'beqz':1,'blez':1,'bgtz':1,'bgtzl':1,'blezl':1,'bnez':1,'bgez':0,'bgezl':0,'bltz':0}[bn]
    prev=rd(g-4)
    out.append((g,base,off,rt,2 if op==0x21 else 1,T))
    print(hex(g),ins(g).disassemble(),'|',ins(g+4).disassemble(),'T',T,'prevbranch' if is_cbranch(prev) or is_uncond(prev) else '', 'ALREADY HOOKED' if g in allh else '')
import json; json.dump(out,open('guards.json','w'))
