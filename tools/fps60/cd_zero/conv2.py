import pickle
from census import *
from census2 import same_field_loads
from classify2 import count_path_reaches
from census3 import zero_walk, cont_after_store
segs,rd=load_elf(SLUS)
ins=mk(rd)
res=pickle.load(open('main5.pkl','rb'))
def guard(rd,a,off):
    """nearest zero-compare branch before dec a on a reg loaded from +off"""
    lds=same_field_loads(rd,a-64,a,off)
    best=None
    for b in lds:
        reg=(rd(b)>>16)&31
        for c in range(b+4,a+4,4):
            x=rd(c); o,s_,t_,_,_=fields(x)
            if dest(x)==reg and c!=b: break
            if ((o in (4,5,0x14,0x15) and t_==0) or o in (6,7,0x16,0x17)) and s_==reg:
                if best is None or c>best[0]: best=(c,b)
    return best,lds
out=[]
for r in res:
    if r['k'] not in ('DEC-CONVERT','DEC-MIXED'): continue
    a=r['addr']; off=r['field'][1]
    g,lds=guard(rd,a,off)
    if not g: out.append((r,'NOGUARD',None,lds)); continue
    c,b=g; x=rd(c); name=ins(c).getOpcodeName(); T=btarget(c,x); F=c+8
    zero = T if name in ('beqz','beqzl','blez','blezl','beq','beql') else F
    cnt_reaches = count_path_reaches(rd,a+4,zero)
    st=r.get('store')
    z=zero_walk(rd,zero,cont_after_store(rd,st),r['field'])
    shared = cnt_reaches or (z['end']=='join' and not z['calls'] and not z['stores'])
    r['guard']=(c,b,zero,shared)
    out.append((r,'SHARED' if shared else 'ELSE',(c,b,zero,z),lds))
pickle.dump(res,open('main6.pkl','wb'))
from collections import Counter
print(Counter(o[1] for o in out))
for r,k,g,lds in out:
    a=r['addr']
    print('==',k,r['k'],hex(a),r['fn'],r['hook'],'+0x%X'%r['field'][1],'loads',[hex(x) for x in lds])
    if g:
        c,b,zero,z=g
        print('   guard',ins(c).disassemble(),'@%X'%c,' zero side @%X end %s calls %s stores %s'%(zero,z['end'],[hex(x) for x in z['calls']],[hex(o) for _,o in z['stores']][:8]))
