import sys, os, struct
sys.path.insert(0,os.path.join(os.path.dirname(os.path.abspath(__file__)),'..'))
from gamemodel import dat_read, scr_read, mdb_read, strip_tris
from mot import Mot
EX=os.environ.get('GODHAND_EXTRACT','extract')
_em00=None
def em00():
    global _em00
    if _em00 is None: _em00=dat_read(open(EX+'/em/em00.dat','rb').read())
    return _em00
def motion(i):
    t,b,o=em00()[i]
    return t,b

def skeleton(path, md=None):
    ents=dat_read(open(path,'rb').read())
    if md is None:
        md=next((i for i,(t,b,o) in enumerate(ents) if t=='MD' and b),None)
    if md is None: return None
    b=ents[md][1]
    kind,nodes=scr_read(b)
    m=mdb_read(b,nodes[0]['mdb_off'])
    return dict(bones=m['bones'],md=md,nodes=nodes,raw=b,ents=ents)

def runtime_key(bones):
    n=len(bones)
    h=2166136261
    def mix(bs):
        nonlocal h
        for x in bs: h=((h^x)*16777619)&0xFFFFFFFF
    mix(struct.pack('<i',n))
    for i in range(1,n):
        p=bones[i]['parent']
        idx=p if 0<=p<n and p!=i else -1
        mix(struct.pack('<b',idx))
        mix(struct.pack('<3f',*bones[i]['t']))
    return h or 1
