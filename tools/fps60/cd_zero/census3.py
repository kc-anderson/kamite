from census2 import *
def cont_after_store(rd, st):
    if st is None: return None
    p=rd(st-4)
    if is_uncond(p) and p>>26==4: return btarget(st-4,p)
    if is_uncond(p) and p>>26==2: return (p&0x3FFFFFF)<<2
    return st+4
def zero_walk(rd, z, cont, field, limit=60):
    calls=[];stores=[];a=z;n=0;end='lim';reload=False;branches=0
    while n<limit:
        if a==cont: end='join'; break
        w=rd(a)
        if is_jal(w): calls.append((w&0x3FFFFFF)<<2)
        o=w>>26
        if o in STORES or o in (0x39,0x3F,0x1F):
            b_=(w>>21)&31;of=w&0xFFFF;stores.append((b_,of))
            if of==field[1]: reload=True
        if is_cbranch(w): branches+=1
        if is_uncond(w):
            w2=rd(a+4)
            if is_jal(w2): calls.append((w2&0x3FFFFFF)<<2)
            if w2>>26 in STORES:
                stores.append(((w2>>21)&31,w2&0xFFFF))
                if (w2&0xFFFF)==field[1]: reload=True
            if o==0: end='ret'; break
            t=btarget(a,w) if o==4 else (w&0x3FFFFFF)<<2
            if t==cont: end='join'; break
            end='j:%X'%t; break
        a+=4;n+=1
    return dict(calls=calls,stores=stores,end=end,reload=reload,n=n,branches=branches,start=z)
def zclass(rd,r):
    out=[]
    cont=cont_after_store(rd,r.get('store') or find_store(rd,r))
    for c,cond,side,_ in r['zs']:
        x=rd(c); T=btarget(c,x); F=c+8
        if side in('T',): z=F
        elif side in ('F','both'): z=T
        else: z=None
        if z is None: out.append((c,cond,side,None)); continue
        out.append((c,cond,side,zero_walk(rd,z,cont,r['field'])))
    return out
def find_store(rd,r):
    base,off=r['field']; dec=r['dec']; dst=r['dst']
    for c in range(dec+4,dec+48,4):
        x=rd(c)
        if x>>26 in STORES and ((x>>16)&31)==dst and (x&0xFFFF)==off: return c
    return None
if __name__=='__main__':
    segs,rd=load_elf(SLUS)
    res=pickle.load(open('main2.pkl','rb'))
    from collections import Counter
    C=Counter()
    for r in res:
        if not r.get('pres'): continue
        r['zw']=zclass(rd,r)
        k=[]
        for c,cond,side,z in r['zw']:
            if z is None: k.append('?'+side)
            elif z['end']=='join' and not z['calls'] and not z['stores']: k.append('empty')
            else: k.append('ACT' + ('R' if z['reload'] else '') + ('c' if z['calls'] else '') + ('s' if z['stores'] else '')+'/'+z['end'][:3])
        r['zk']=k
        C[' '.join(k)]+=1
    for k,v in C.most_common(): print(v,k)
    pickle.dump(res,open('main3.pkl','wb'))
