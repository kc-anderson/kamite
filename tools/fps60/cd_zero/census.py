import sys, struct, bisect, csv
sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__file__), '..'))
from countdown_census import load_elf
import rabbitizer as rz
from sites import sites
rz.config.regNames_gprAbiNames = rz.Abi.O32
SLUS=__import__('os').environ.get('SLUS', 'SLUS_215.03')
LOADS={0x20:1,0x24:1,0x21:2,0x25:2,0x23:4}
STORES={0x28:1,0x29:2,0x2B:4}
def mk(rd):
    def ins(a): return rz.Instruction(rd(a), a, category=rz.InstrCategory.R5900)
    return ins
def fields(w):
    return w>>26,(w>>21)&31,(w>>16)&31,(w>>11)&31,w&0xFFFF
def simm(w):
    i=w&0xFFFF; return i-0x10000 if i&0x8000 else i
def btarget(a,w): return a+4+(simm(w)<<2)
def is_jal(w): return w>>26==3
def is_cbranch(w):
    op,rs,rt,_,_=fields(w)
    if op in (4,5,6,7,0x14,0x15,0x16,0x17): return True
    if op==1 and rt in (0,1,2,3,0x10,0x11,0x12,0x13): return True
    if op==0x11 and rs==8: return True
    return False
def is_uncond(w):
    op,rs,rt,rd_,_=fields(w)
    if op==2: return True
    if op==0 and (w&0x3F)==8: return True  # jr
    if op==4 and rs==0 and rt==0: return True  # b
    return False
def dest(w):
    op,rs,rt,rd_,_=fields(w)
    if op==0: return rd_
    if 8<=op<=15 or op in LOADS or op in (0x27,0x1E,0x37): return rt  # lwu,lq,ld
    if op==3: return 31
    return -1

def analyze(rd, a, fn=lambda a:'?'):
    ins=mk(rd); w=rd(a); op,rs,rt,_,im=fields(w)
    r={'addr':a,'fn':fn(a)}
    if op==9 and im in (0xFFFF,1):
        mode='dec' if im==0xFFFF else 'inc'; src,dst=rs,rt; dec=a
        ld=None
        for b in range(a-4,a-48,-4):
            x=rd(b)
            if x>>26 in LOADS and ((x>>16)&31)==src: ld=b;break
            if dest(x)==src: break
    elif op in LOADS:
        mode='load'; ld=a; src=rt; dec=None
        for c in range(a+4,a+40,4):
            x=rd(c)
            if x>>26==9 and (x&0xFFFF) in (0xFFFF,) and ((x>>21)&31)==src: dec=c; dst=(x>>16)&31; break
        if dec is None: r['mode']='load?'; return r
    else:
        r['mode']='other:'+ins(a).getOpcodeName(); return r
    r['mode']=mode
    if ld is None: r['cls']='noload'; return r
    lw=rd(ld); base=(lw>>21)&31; off=lw&0xFFFF
    r['field']=(base,off)
    st=None
    for c in range(dec+4,dec+48,4):
        x=rd(c)
        if x>>26 in STORES and ((x>>16)&31)==dst and ((x>>21)&31)==base and (x&0xFFFF)==off: st=c;break
    r['store']=st
    # pre-test: branch between ld and dec (inclusive of branch whose delay slot is dec) testing src, or slt on src
    pre=None; tests={src}
    for b in range(ld+4,dec+4,4):
        x=rd(b); o,s_,t_,d_,_=fields(x)
        if o==0 and (x&0x3F) in (0x2A,0x2B) and (s_==src or t_==src): tests.add(d_); continue
        if o in (0x0A,0x0B) and s_==src: tests.add(t_); continue
        if is_cbranch(x) and (s_ in tests or (o in (4,5,0x14,0x15) and t_ in tests)):
            pre=b;break
    r['pre']=pre
    if pre is not None:
        x=rd(pre); o,s_,t_,_,_=fields(x)
        T=btarget(pre,x); F=pre+8
        cond=ins(pre).getOpcodeName()
        r['cond']=cond
        # zero side
        slt = s_!=src and not (o in (4,5,0x14,0x15) and t_==src)
        if slt:
            r['zero']=None; r['cls']='slt'
        else:
            zero={'beq':T,'beqz':T,'beql':T,'beqzl':T,'blez':T,'blezl':T,'bne':F,'bnez':F,'bnel':F,'bnezl':F,'bgtz':F,'bgtzl':F}.get(cond)
            r['zero']=zero
            if zero is None: r['cls']='nozerosplit'  # bltz/bgez: 0 goes with positive
    # post-test after dec: branch on dst within ~10 instrs
    post=None
    for b in range(dec+4,dec+44,4):
        x=rd(b); o,s_,t_,_,_=fields(x)
        if is_cbranch(x) and (s_==dst or (o in (4,5,0x14,0x15) and t_==dst)): post=b;break
        if dest(x)==dst and b!=dec: break
    r['post']=post
    return r

def zero_summary(rd, z, field, limit=40):
    """Linear walk of the zero side: calls, stores, reload of the field, end kind."""
    calls=[];stores=[];reload=False;n=0;a=z;end='?'
    while n<limit:
        w=rd(a)
        if is_jal(w): calls.append((w&0x3FFFFFF)<<2)
        o=w>>26
        if o in STORES or o in (0x39,0x3F,0x1F):
            b_=(w>>21)&31;of=w&0xFFFF;stores.append((b_,of))
            if (b_,of)==field: reload=True
        if is_uncond(w):
            end=('ret' if (w>>26)==0 else 'j:%X'%(btarget(a,w) if w>>26==4 else ((w&0x3FFFFFF)<<2)))
            # include delay slot
            w2=rd(a+4)
            if is_jal(w2): calls.append((w2&0x3FFFFFF)<<2)
            if w2>>26 in STORES:
                b_=(w2>>21)&31;of=w2&0xFFFF;stores.append((b_,of))
                if (b_,of)==field: reload=True
            n+=1; break
        a+=4;n+=1
    return dict(calls=calls,stores=stores,reload=reload,end=end,n=n,start=z)

def dis(rd,lo,hi):
    ins=mk(rd)
    return '\n'.join('  %08X: %s'%(a,ins(a).disassemble()) for a in range(lo,hi,4))

if __name__=='__main__':
    segs,rd=load_elf(SLUS)
    names={}
    for r_ in csv.reader(open(__import__('os').path.join(__import__('os').path.dirname(__file__), '../../../recomp/kamite_functions.csv'))):
        try: names[int(r_[1],16)]=r_[0]
        except: pass
    keys=sorted(names)
    fn=lambda a: names[keys[bisect.bisect_right(keys,a)-1]]
    res=[]
    for mod,addr,hook,_ in sites():
        if mod!='kamite': continue
        r=analyze(rd,addr,fn); r['hook']=hook; res.append(r)
    import pickle; pickle.dump(res,open('main.pkl','wb'))
    from collections import Counter
    print(Counter(r['mode'] for r in res))
    print(Counter((r['mode'], 'pre' if r.get('pre') else 'nopre', 'post' if r.get('post') else 'nopost', r.get('cls','')) for r in res))
