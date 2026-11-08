import sys, json, os, numpy as np, time
from multiprocessing import Pool
OUT=os.environ.get('PUNISH_FIT_FITS','fits'); os.makedirs(OUT,exist_ok=True)
def job(args):
    name,gi,rep=args
    sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
    from fit3 import prepare, solve, NAMES, EX
    from fit import reference
    import pickle
    fr_ref,tch=reference(name)
    P=prepare(name,tch,EX+'/em/%s.dat'%rep)
    r=solve(P)
    np.save('%s/%s_%02d.npy'%(OUT,name,gi),r['X'])
    m=dict(name=name,group=gi,rep=rep,e0=float(r['e0'].mean()),e0p90=float(np.percentile(r['e0'],90)),e1=float(r['e1'].mean()),e1p90=float(np.percentile(r['e1'],90)),
           ntouch=len(r['e0']),ks=P['ks'],ranges={n:[float(r['X'][:,i].min()),float(r['X'][:,i].max())] for i,n in enumerate(NAMES)})
    json.dump(m,open('%s/%s_%02d.json'%(OUT,name,gi),'w'))
    return name,gi,rep,m['e0'],m['e1']
if __name__=='__main__':
    bodies=json.load(open('bodies.json'))
    which=sys.argv[1:] or ['Stinger','Cobra','Spanking','Suplex']
    groups={'Gorilla':[2,3,7,8,20]}
    jobs=[(n,gi,b['files'][0]) for n in which for gi,b in enumerate(bodies) if gi in groups.get(n,range(len(bodies)))]
    t0=time.time()
    with Pool(4) as p:
        for res in p.imap_unordered(job,jobs):
            print('%-8s g%02d %-5s before %.3f after %.3f  %.0fs'%(res+(time.time()-t0,)),flush=True)
