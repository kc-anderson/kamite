from census import *
import pickle, bisect, csv
DERIV_R={0x2A,0x2B,0x00,0x03,0x02,0x24,0x25,0x21,0x2D,0x3C,0x3F,0x3E,0x38,0x3A,0x3B}  # slt sltu sll sra srl and or addu daddu dsll32 dsra32 ...
def derived(w, regs):
    """dest reg if w derives a value from a reg in regs (for tests), else -1"""
    o,s_,t_,d_,_=fields(w)
    if o==0:
        f=w&0x3F
        if f in (0x00,0x02,0x03,0x38,0x3A,0x3B,0x3C,0x3E,0x3F) and t_ in regs: return d_
        if f in (0x2A,0x2B,0x24,0x25,0x21,0x2D,0x23,0x2F) and (s_ in regs or t_ in regs): return d_
        return -1
    if o in (0x0A,0x0B,0x0C,0x0D,0x09,0x19) and s_ in regs: return t_
    return -1
def tests_reg(w, regs):
    o,s_,t_,_,_=fields(w)
    return is_cbranch(w) and o!=0x11 and (s_ in regs or (o in (4,5,0x14,0x15) and t_ in regs))

def analyze2(rd,a,fn):
    r=analyze(rd,a,fn)
    w=rd(a)
    if r['mode']=='load?':
        # load in a likely-branch delay slot: decrement at the branch target
        b=rd(a-4)
        if (b>>26) in (0x14,0x15,0x16,0x17):
            T=btarget(a-4,b); src=(w>>16)&31
            for c in range(T,T+40,4):
                x=rd(c)
                if x>>26==9 and (x&0xFFFF)==0xFFFF and ((x>>21)&31)==src:
                    r['mode']='load'; r['likely']=a-4; r['dec']=c; r['dst']=(x>>16)&31
                    r['field']=((w>>21)&31,w&0xFFFF)
                    break
                if dest(x)==src: break
    return r

def same_field_loads(rd, lo, hi, off):
    out=[]
    for b in range(lo,hi,4):
        x=rd(b)
        if x>>26 in LOADS and (x&0xFFFF)==off: out.append(b)
    return out

def tests_of_load(rd, b, limit=10):
    """branches within `limit` instrs after load b testing its value (direct or derived)"""
    regs={(rd(b)>>16)&31}; out=[]
    for c in range(b+4,b+4*limit,4):
        x=rd(c)
        if tests_reg(x,regs): out.append(c)
        d=derived(x,regs)
        if d>0: regs.add(d)
        else:
            dd=dest(x)
            if dd in regs and dd!=-1 and d<=0: regs.discard(dd)
        if is_jal(x): regs-=set(range(1,16))|{24,25,31}
        if not regs: break
    return out

if __name__=='__main__':
    segs,rd=load_elf(SLUS)
    rows=[]
    for r_ in csv.reader(open(__import__('os').path.join(__import__('os').path.dirname(__file__), '../../../recomp/kamite_functions.csv'))):
        try: rows.append((int(r_[1],16),int(r_[2],16) if len(r_)>2 and r_[2] else 0,r_[0]))
        except: pass
    print(rows[:3])

def full(rd, r, bounds):
    a=r['addr']
    if r['mode'] not in ('dec','load','inc'): return r
    if 'field' not in r: return r
    base,off=r['field']
    dec=r.get('dec') or (a if r['mode'] in ('dec','inc') else None)
    if dec is None:
        # load mode: find dec
        for c in range(a+4,a+40,4):
            x=rd(c)
            if x>>26==9 and (x&0xFFFF)==0xFFFF: dec=c;break
    r['dec']=dec
    dst=(rd(dec)>>16)&31; r['dst']=dst
    lo,hi=bounds
    # own pre-tests: same-field loads within 16 instrs before dec, tested at/before dec (branch addr <= dec, or dec in its delay slot)
    pres=[]
    for b in same_field_loads(rd,max(lo,dec-64),dec+4,off):
        for c in tests_of_load(rd,b):
            if c<=dec and c>=b: pres.append(c)
    if r.get('likely'): pres.append(r['likely'])
    r['pres']=sorted(set(pres))
    # post tests on dst (direct/derived) within 12 instrs after dec
    posts=[]; regs={dst}
    for c in range(dec+4,dec+48,4):
        x=rd(c)
        if tests_reg(x,regs): posts.append(c)
        d=derived(x,regs)
        if d>0: regs.add(d)
        else:
            dd=dest(x)
            if dd in regs: regs.discard(dd)
        if not regs: break
    r['posts']=posts
    # other tests of the field in the function
    others=[]
    for b in same_field_loads(rd,lo,hi,off):
        if dec-64<=b<=dec+4: continue
        t=tests_of_load(rd,b)
        if t: others.append((b,t[0]))
    r['others']=others
    # zero sides of pre-tests
    zs=[]
    for c in r['pres']:
        x=rd(c); cond=mk(rd)(c).getOpcodeName()
        T=btarget(c,x); F=c+8
        # which side is "count" (contains dec)? count side = side whose path reaches dec
        if dec==c+4 and not cond.endswith('l'): side_dec='both'
        elif T<=dec<T+40: side_dec='T'
        elif F<=dec<F+40: side_dec='F'
        else: side_dec='?'
        other = F if side_dec=='T' else T if side_dec=='F' else None
        if side_dec=='both':
            # decrement in delay slot executes both; store only on fall through -> zero side = taken
            other=T
        z=zero_summary(rd,other,(base,off)) if other else None
        zs.append((c,cond,side_dec,z))
    r['zs']=zs
    return r

def run():
    segs,rd=load_elf(SLUS)
    rows=[]
    for r_ in csv.reader(open(__import__('os').path.join(__import__('os').path.dirname(__file__), '../../../recomp/kamite_functions.csv'))):
        try: rows.append((int(r_[1],16),int(r_[2],16),r_[0]))
        except: pass
    rows.sort(); starts=[x[0] for x in rows]
    def fb(a):
        i=bisect.bisect_right(starts,a)-1; return rows[i]
    res=[]
    for mod,addr,hook,_ in sites():
        if mod!='kamite': continue
        f=fb(addr)
        r=analyze2(rd,addr,lambda a:f[2]); r['hook']=hook
        r=full(rd,r,(f[0],f[1])); res.append(r)
    pickle.dump(res,open('main2.pkl','wb'))
    return rd,res
if __name__=='__main__':
    rd,res=run()
    from collections import Counter
    def cat(r):
        if r['mode'] not in('dec','load','inc') or 'field' not in r: return 'X:'+r['mode']
        k=[r['mode']]
        k.append('pre' if r['pres'] else '-')
        k.append('post' if r['posts'] else '-')
        k.append('oth' if r['others'] else '-')
        return ' '.join(k)
    c=Counter(cat(r) for r in res)
    for k,v in sorted(c.items()): print(v,k)
