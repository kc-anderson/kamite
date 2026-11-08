"""resources/punish_fit.txt from fit_pose.py's fits (fits7/): one fit per punish, body group and enemy size, listing
every runtime key of the group. Verdicts from the touch error (as emit.py's) at each size; a larger size's fit is
off when it can't be made to work (the runtime: own punish from halfway to that size); a body off at 1.0 is off at
every size."""
from common import EX, skeleton, runtime_key
import sys, os, json, glob, numpy as np
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
from punishes import PUNISH
from fit_pose import B, TYPES, REP, REFGROUP, OUT as FITS, type_of
FILE={'Suplex':'Suplex','Stinger':'Stinger','Cobra':'CobraTwist','Spanking':'Spanking','Gorilla':'GorillaSuplex'}
GORILLA_GROUPS={2,3,7,8,20,21}
def verdict(m):
    clip=max(v[1] for v in m['clip1'].values())
    if m['name']=='Gorilla':      # its arms are kept out of the victim on purpose (K): the hands leave the original's spots
        v='fitted' if m['e1']<0.18 else 'questionable' if m['e1']<0.22 else 'off'
    elif m.get('clasp'):          # cobra_clasp: the hands clasped, not the original's spots; K judges it in play
        v='questionable'
    elif m['e1']<0.07 and m['e1p90']<0.13: v='fitted'
    elif m['e1']<0.12: v='questionable'
    else: v='off'
    if v=='fitted' and clip>0.03 and m['group']!=10: v='questionable'
    return v,clip
def tracks(name,X,ks,twist=0.0):
    """twist: Gene's left forearm about its length (deg), eased with the left arm's raise (spank_fat2's held pose)."""
    from fk import Motion
    out=[]; o=0; wmax=np.abs(X[:,8]).max()
    for si,stg in enumerate(PUNISH[name]):
        fs=ks[si]; K=X[o:o+len(fs)]; o+=len(fs)
        items=[]
        for j,f in enumerate(fs):
            g='%d:%.3f,%.3f,%.2f,%.3f'%(f,K[j,0],K[j,1],np.degrees(K[j,2]),K[j,3])
            pose=np.degrees(K[j,6:14])
            if np.any(np.abs(pose)>=0.05):
                g+=','+','.join('%.1f'%a for a in pose)
                if twist and wmax>0: g+=',%.1f'%(twist*abs(K[j,8])/wmax)
            items.append(g)
        out.append('gene %d %s'%(stg['g'][0],' '.join(items)))
        vm=stg['v'][0]; L=Motion(vm).frames
        vk=' '.join('%d:%.3f,%.2f'%(f,K[j,4],np.degrees(K[j,5])) for j,f in enumerate(fs) if f<=L-1)
        out.append('victim %d %s'%(vm,vk))
        if vm==329: out.append('victim 331 %s'%vk)  # the Suplex's second victim version (phase 29 plays either)
    return out
def main(path):
    lines=['# Kamite punish fits for Buck Breaker (tools/models/punish_fit fit7.py + fit_pose.py + emit7.py; claude/punish-fit-offline.md).',
           '# fit <punish> <body (skeleton hash)>[,<body>...] [off] [gorilla] scale=<enemy size> size=<victim size change>',
           '#   gene <motion> <frame>:<forward m>,<side m>,<turn deg>,<height m>[,<torso x>,<torso z>,<L raise>,<L swing>,<L elbow>,',
           '#       <R raise>,<R swing>,<R elbow>[,<L forearm twist>] (deg)] ...   (placement scaled by the enemy\'s size)',
           '#   victim <motion> <frame>:<height m>,<waist bend deg> ...',
           '# off = doesn\'t fit: Buck Breaker keeps the enemy\'s own punish for that pick.',
           '# gorilla (on a Suplex fit) = a Suplex pick plays the Gorilla Suplex on that body (its GorillaSuplex fit applies).',
           '# Between two sizes the runtime interpolates; a body with only a 1.3 fit (the original pairing) is unchanged at 1.0.']
    lines.append('# body <body>[,<body>...] <type>   (the enemy labels\' body type: F2 > Debug > Enemy labels)')
    for g in range(len(B)): lines.append('body %s %s'%(','.join(B[g]['keys']),type_of(g)))
    # K (Nov 8): Mr. Gold / Mr. Silver played unfitted. A skeleton in no body line whose body bones (0-25) match a
    # group's plays that group's fits; the head's extra bones (26..) vary between identical bodies. Same bones in two
    # groups: the type's representative wins, else the first.
    lines.append('# core <hash of bones 0-25> <body>   (a skeleton in no body line with these body bones plays <body>\'s fits)')
    core={}
    for g in range(len(B)):
        for f in B[g]['files']:
            sk=skeleton(os.path.join(EX,'em',f+'.dat'))
            if not sk or len(sk['bones'])<26: continue
            c=runtime_key(sk['bones'][:26]); o=core.get(c)
            if o is not None and o!=g and type_of(o)!=type_of(g): print('core %08x in groups %d and %d (different types)'%(c,o,g))
            if o is None or (g in REP.values() and o not in REP.values()): core[c]=g
    for c,g in sorted(core.items()): lines.append('core %08x %s   # %s'%(c,B[g]['keys'][0],B[g]['files'][0]))
    summary=[]
    fits={}
    for f in sorted(glob.glob(os.path.join(FITS,'*.json'))):
        m=json.load(open(f)); fits[(m['name'],m['group'],m['scale'])]=(m,f[:-5]+'.npy')
    for name in ('Suplex','Stinger','Cobra','Spanking','Gorilla'):
        for g in range(len(B)):
            keys=','.join(B[g]['keys']); t=type_of(g); file=B[g]['files'][0]
            if name=='Suplex' and t=='fat':
                flag='gorilla' if g in GORILLA_GROUPS else 'off'
                lines.append('fit Suplex %s %s   # %s: %s'%(keys,flag,file,'as the Gorilla Suplex' if flag=='gorilla' else 'doesn\'t fit (own punish)'))
                summary.append((name,g,file,t,1.0,flag,None)); continue
            if name=='Spanking' and t=='fat':
                # K (Nov 8, with a clip of the original game): the fat man's bulk hides Gene's clipping arm, so it plays
                # with the game's own placement (an empty fit at 1.0: other sizes scale the pair to it)
                lines.append('fit Spanking %s scale=1.00 size=1.000   # %s: the game\'s own placement (K: the fat man hides the arm)%s'%(
                    keys,file,'; Gene\'s left arm raised onto the back (spank_fat2.py)' if (name,g,1.0) in fits else ''))
                if (name,g,1.0) in fits:
                    m,npy=fits[(name,g,1.0)]; lines+=tracks(name,np.load(npy),m['ks'],m.get('ltwist',0.0))
                summary.append((name,g,file,t,1.0,'own placement',None)); continue
            off10=False
            for s in (1.0,1.15,1.3):
                if (name,g,s) not in fits: continue
                m,npy=fits[(name,g,s)]; X=np.load(npy); v,clip=verdict(m)
                off=v=='off' or off10        # a size that can't be made to work: own punish from halfway there
                if s==1.0: off10=off
                lines.append('fit %s %s%s scale=%.2f size=%.3f   # %s (%s%s), %s: touch %.0f -> %.0f cm, clip +%.1f cm'%(
                    FILE[name],keys,' off' if off else '',s,m['size'],file,t,', hands clasped' if m.get('clasp') else ', pose from the representative' if not m['rep'] and m.get('pose_from')!='original' else '',
                    v,m['e0']*100,m['e1']*100,clip*100))
                if not off: lines+=tracks(name,X,m['ks'])
                summary.append((name,g,file,t,s,v,m))
    open(path,'w').write('\n'.join(lines)+'\n')
    return summary
if __name__=='__main__':
    s=main(sys.argv[1])
    from collections import Counter
    print(Counter((x[4],x[5]) for x in s)); print('%.0f KB'%(os.path.getsize(sys.argv[1])/1024))
