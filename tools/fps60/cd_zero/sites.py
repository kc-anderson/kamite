import tomllib, glob, re, os
R=__import__('os').path.join(__import__('os').path.dirname(__file__), '../../../recomp/')
PAT=re.compile(r'countdown|_cd_|countup')
def sites():
    out=[]
    for f in [R+'kamite.toml']+sorted(glob.glob(R+'modules/*.toml')):
        d=tomllib.load(open(f,'rb'))
        for h in d.get('patches',{}).get('hooks',[]):
            if PAT.search(h['hook']):
                out.append((os.path.basename(f)[:-5], int(h['address'],16), h['hook'], h.get('replace',False)))
    return out
if __name__=='__main__':
    s=sites(); print(len(s))
    from collections import Counter
    print(Counter(x[0] for x in s)); print([x for x in s if not x[3]])
