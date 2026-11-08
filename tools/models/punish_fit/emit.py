import json, glob, numpy as np, sys
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from punishes import PUNISH
from common import EX, skeleton, runtime_key
FILE={'Suplex':'Suplex','Stinger':'Stinger','Cobra':'CobraTwist','Spanking':'Spanking','Gorilla':'GorillaSuplex'}
GORILLA_GROUPS={2,3,7,8,20,21}  # the fat and wide men: a Suplex pick plays the Gorilla Suplex (K, Nov 8)
REFGROUP={'Suplex':0,'Stinger':2,'Cobra':11,'Spanking':1,'Gorilla':21}
def verdict(m):
    if m['e0']<0.04: return 'fine'
    if m['e1']<0.07 and m['e1p90']<0.13: return 'fitted'
    if m['e1']<0.12: return 'questionable'
    return 'off'
def tracks(name, X, ks):
    out=[]
    stages=PUNISH[name]; o=0
    for si,stg in enumerate(stages):
        fs=ks[si]; K=X[o:o+len(fs)]; o+=len(fs)
        # Gene: one motion per stage
        gm=stg['g'][0]
        out.append(('gene',gm,[(f,K[j,0],K[j,1],np.degrees(K[j,2]),K[j,3]) for j,f in enumerate(fs)]))
        # victim: the stage's first motion (later ones start after the touches end: no fit)
        vm=stg['v'][0]
        from fk import Motion
        L=Motion(vm).frames
        keys=[(f,K[j,4],np.degrees(K[j,5])) for j,f in enumerate(fs) if f<=L-1]
        out.append(('victim',vm,keys))
        if vm==329: out.append(('victim',331,keys))  # the Suplex's second victim version (phase 29 plays either)
    return out
def main(path):
    B=json.load(open('bodies.json'))
    lines=['# Kamite punish fits for Buck Breaker (tools/models/punish_fit fit3.py + emit.py; claude/punish-fit-offline.md).',
           '# fit <punish> <body (skeleton hash)> [off]   then its curves until the next fit line:',
           '#   gene <motion> <frame>:<forward m>,<side m>,<turn deg>,<height m> ...   (placement scaled by the enemy\'s size)',
           '#   victim <motion> <frame>:<height m>,<waist bend deg> ...',
           '# off = doesn\'t fit: Buck Breaker keeps the enemy\'s own punish for that pick.',
           '# gorilla (on a Suplex fit) = a Suplex pick plays the Gorilla Suplex on that body (its GorillaSuplex fit applies).']
    summary=[]
    for f in sorted(glob.glob('fits/*.json')):
        m=json.load(open(f)); name=m['name']; g=m['group']
        if g==REFGROUP[name]: continue
        X=np.load(f[:-5]+'.npy'); v=verdict(m)
        keys=B[g]['keys']
        tr=tracks(name,X,m['ks'])
        summary.append((name,g,B[g]['files'][0],v,m['e0'],m['e1']))
        flag=' off' if v=='off' else ''
        if name=='Suplex' and g in GORILLA_GROUPS: flag=' gorilla'; v='as the Gorilla Suplex'
        for k in keys:
            lines.append('fit %s %s%s   # %s, %s: %.0f -> %.0f cm'%(FILE[name],k,flag,B[g]['files'][0],v,m['e0']*100,m['e1']*100))
            for who,mot,kk in tr:
                if who=='gene': items=' '.join('%d:%.3f,%.3f,%.2f,%.3f'%(int(fr),a,b,c,d) for fr,a,b,c,d in kk)
                else: items=' '.join('%d:%.3f,%.2f'%(int(fr),a,b) for fr,a,b in kk)
                lines.append('%s %d %s'%(who,mot,items))
    open(path,'w').write('\n'.join(lines)+'\n')
    return summary
if __name__=='__main__':
    s=main(sys.argv[1])
    from collections import Counter
    print(Counter(v for _,_,_,v,_,_ in s)); print(len(open(sys.argv[1]).read())/1024,'KB')
