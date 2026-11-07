from census3 import *
from collections import Counter, defaultdict
def follow_dec(rd, a, src):
    """from load at a (maybe in a delay slot of a likely/uncond branch), find the addiu src,-1"""
    b=rd(a-4); starts=[a+4]
    if (b>>26) in (0x14,0x15,0x16,0x17) or (b>>26==0x11 and ((b>>16)&3) in (2,3)) or (b>>26==4 and ((b>>21)&31)==0 and ((b>>16)&31)==0):
        starts=[btarget(a-4,b)]
    for s in starts:
        for c in range(s,s+40,4):
            x=rd(c)
            if x>>26==9 and (x&0xFFFF)==0xFFFF and ((x>>21)&31)==src: return c
            if dest(x)==src: break
    return None
def posts_of(rd, dec, dst, off):
    posts=[]; regs={dst}; st=None
    for c in range(dec+4,dec+64,4):
        x=rd(c)
        if tests_reg(x,regs): posts.append(c)
        if x>>26 in STORES and (x&0xFFFF)==off and ((x>>16)&31) in regs and st is None: st=c
        d=derived(x,regs)
        if d>0: regs.add(d)
        else:
            dd=dest(x)
            if dd in regs: regs.discard(dd)
        if is_jal(x) or (x>>26==0 and (x&0x3F)==9):
            regs-=set(range(1,16))|{24,25,31}
        if is_uncond(x): break
        if not regs and st is not None: break
    # re-loads of the field after the store, tested
    if st:
        for b in range(st+4,st+48,4):
            x=rd(b)
            if is_uncond(rd(b-4)): break
            if x>>26 in LOADS and (x&0xFFFF)==off:
                t=tests_of_load(rd,b,8)
                if t: posts.append(t[0])
    return posts, st
def classify(rd, r):
    a=r['addr']; w=rd(a); op=w>>26
    if op in LOADS:
        src=(w>>16)&31; off=w&0xFFFF
        dec=follow_dec(rd,a,src)
        if dec is None: return 'LOAD-NODEC',{}
        dst=(rd(dec)>>16)&31
        posts,st=posts_of(rd,dec,dst,off)
        info=dict(dec=dec,posts=posts,store=st,off=off,signed=op in (0x20,0x21),bytes=LOADS[op])
        return ('LOAD-KEEP' if posts else 'LOAD-EXACT'), info
    if op==9 and (w&0xFFFF)==0xFFFF:
        posts=r.get('posts',[])
        if 'field' in r:
            p2,st=posts_of(rd,a,(w>>16)&31,r['field'][1]); posts=sorted(set(posts)|set(p2))
        act = r.get('pres') and any(z!='empty' for z in r.get('zk',[]))
        if r.get('pres') and act and not posts: return 'DEC-CONVERT',dict(posts=posts)
        if r.get('pres') and act and posts: return 'DEC-MIXED',dict(posts=posts)
        if posts and not r.get('pres'): return 'DEC-POSTONLY',dict(posts=posts)
        return 'DEC-OK',dict(posts=posts)
    if op==9: return 'INC',{}
    return 'OTHER',{}
if __name__=='__main__':
    segs,rd=load_elf(SLUS)
    res=pickle.load(open('main3.pkl','rb'))
    C=Counter(); byhook=defaultdict(Counter)
    for r in res:
        k,info=classify(rd,r); r['k']=k; r['info']=info
        C[k]+=1; byhook[r['hook']][k]+=1
    print(C)
    for h,c in byhook.items():
        if len(c)>1: print('MIXED NAME',h,dict(c))
    pickle.dump(res,open('main4.pkl','wb'))
