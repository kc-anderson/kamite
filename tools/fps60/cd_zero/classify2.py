from classify import *
def count_path_reaches(rd, start, z, limit=80):
    """walk linearly from start; True if we reach z (fallthrough or uncond jump to z) before return/other jump"""
    a=start;n=0
    while n<limit:
        if a==z: return True
        w=rd(a)
        if is_uncond(w):
            if w>>26==0: return False
            t=btarget(a,w) if w>>26==4 else (w&0x3FFFFFF)<<2
            return t==z
        a+=4;n+=1
    return False
def true_else(rd,r):
    """for dec sites: list of pre-test zero sides that are NOT shared with the count path"""
    out=[]
    dec=r['addr']
    for c,cond,side,z in r.get('zw',[]):
        if z is None: continue
        if z['end']=='join' and not z['calls'] and not z['stores']: continue
        if count_path_reaches(rd,dec+4,z['start']): continue
        out.append((c,z))
    return out
if __name__=='__main__':
    segs,rd=load_elf(SLUS)
    res=pickle.load(open('main4.pkl','rb'))
    for r in res:
        k,info=classify(rd,r); r['k']=k; r['info']=info
        if k in ('DEC-CONVERT','DEC-MIXED'):
            te=true_else(rd,r)
            if not te: r['k']='DEC-OK(shared)'
            r['te']=te
    from collections import Counter
    print(Counter(r['k'] for r in res))
    lh=Counter((r['hook'],r['k']) for r in res if r['k'].startswith('LOAD'))
    for (h,k),v in sorted(lh.items()): print(v,h,k)
    pickle.dump(res,open('main5.pkl','wb'))
