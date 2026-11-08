"""The step 3 review page (site3/index.html): cards by body type, the 1.0 and 1.3 sheets, numbers, the all-bodies table."""
import os, sys, json, glob, html
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
from fit_pose import B, TYPES, REFTYPE, REPFILE, REF, OUT as FITS, type_of
from emit7 import verdict
SITE=os.path.join(HERE,'site3')
TNAME={'men':'Standard men','women':'Women','fat':'Fat and wide men','tall':'Tall skinny men','small':'Small men'}
PNAME={'Suplex':'Suplex','Stinger':'Stinger','Cobra':'Cobra Twist','Spanking':'Spanking','Gorilla':'Gorilla Suplex (for a Suplex pick)'}
ORDER=['Suplex','Gorilla','Stinger','Cobra','Spanking']
def load():
    F={}
    for f in glob.glob(os.path.join(FITS,'*.json')):
        m=json.load(open(f)); F[(m['name'],m['group'],m['scale'],m['rep'])]=m
    return F
def cur3(name,g):
    p=os.path.join(HERE,'fits','%s_%02d.json'%(name,g))
    return json.load(open(p)) if os.path.exists(p) else None
def badge(v):
    col={'fitted':'#2e7d32','questionable':'#b26a00','off':'#b00020'}.get(v,'#555')
    return '<span class="badge" style="background:%s">%s</span>'%(col,html.escape(v))
def clipline(m):
    return ', '.join('%s +%.1f'%(k,v[1]*100) for k,v in m['clip1'].items())
def main():
    F=load(); out=[]
    out.append('''<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Punish fits, step 3: Gene's pose</title><style>
:root{--ink:#1d1d1f;--muted:#5f6368;--line:#e3e3e3;--bg:#fafaf8}
body{margin:0;font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif;color:var(--ink);background:var(--bg)}
main{max-width:1000px;margin:0 auto;padding:28px 20px 60px}
h1{font-size:26px;margin:0 0 6px}h2{font-size:20px;margin:36px 0 10px;padding-top:10px;border-top:1px solid var(--line)}
h3{font-size:16px;margin:0}
.lede{color:var(--muted);max-width:760px}
.card{background:#fff;border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin:14px 0}
.head{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.badge{color:#fff;font-size:12px;padding:1px 8px;border-radius:9px}
.nums{color:var(--muted);font-size:13.5px;margin:6px 0 8px}
img{max-width:100%;height:auto;border:1px solid var(--line);border-radius:6px;display:block;margin:6px 0}
details summary{cursor:pointer;color:var(--muted);font-size:13.5px}
table{border-collapse:collapse;font-size:13px;width:100%;background:#fff}th,td{border:1px solid var(--line);padding:3px 6px;text-align:left}
th{background:#f3f3f1}td.f{color:#2e7d32}td.q{color:#b26a00}td.o{color:#b00020}
ul{padding-left:20px}
</style></head><body><main>
<h1>Punish fits, step 3: Gene's pose</h1>
<p class="lede">Gene's torso and arms are now fitted per body type, on top of placement and heights. Gene is blue and the
victim orange. Numbers: touch error (how far Gene's touching spots are from where they touch on the original pair) and
how much deeper Gene goes into the victim than in the original pair at the same frame (worst frame, per part).</p>
<ul>
<li>Gene's clipping limit is the original's own at that frame, plus 1 cm. The fitter turns his torso (up to 15&deg;)
and swings, raises and bends his arms (up to 35&deg; / 30&deg; at the elbow), keeping his hands on their touch spots.</li>
<li>Cobra Twist on women and fat men: Gene's arm touches move from the chest to round the neck.</li>
<li>Gorilla Suplex on the fat men: Gene's arms may not go more than 1 cm into the victim, even where the original
lets them, so they fold in less.</li>
<li>Scaled-up enemies: each pairing is also fitted at enemy size 1.3. The game interpolates between 1.0 and 1.3 by the
enemy's size. The original pairings, which are untouched at 1.0, now get a 1.3 fit too.</li>
<li>The victim can be resized by up to 5% for the pairing. The change eases in over a third of a second after the catch
and eases back out after the punish.</li>
<li>Spanking on fat men stays off, as agreed: their own punish plays.</li>
</ul>''')
    for t in TYPES:
        out.append('<h2>%s</h2>'%TNAME[t])
        for name in ORDER:
            for s in (1.0,1.3):
                m=F.get((name,{'men':0,'women':1,'fat':2,'tall':11,'small':5}[t],s,True))
                if not m: continue
                v,clip=verdict(m); tag='%s_%s_%.2f'%(name,t,s)
                orig=(t==REFTYPE[name] and name!='Gorilla')
                title='%s on %s%s'%(PNAME[name],TNAME[t].lower(),'' if s==1.0 else ', enemy size 1.3')
                c3=cur3(name,m['group']) if s==1.0 else None
                nums='touch %.0f cm with no fit'%(m['e0']*100)
                if c3: nums+=', %.0f cm in the current build'%(c3['e1']*100)
                nums+=', <b>%.0f cm now</b> (90%% within %.0f cm). Deeper than the original: %s cm. Victim size %.3f.'%(m['e1']*100,m['e1p90']*100,clipline(m),m['size'])
                out.append('<div class="card"><div class="head"><h3>%s</h3>%s<span class="nums">%s, pose fitted on %s</span></div>'%(html.escape(title),badge(v),'original pairing' if orig else 'fitted',REPFILE[t]))
                out.append('<div class="nums">%s</div>'%nums)
                if os.path.exists(os.path.join(SITE,tag+'.png')): out.append('<img src="%s.png" alt="%s: side-on frames, original / before / new" loading="lazy">'%(tag,html.escape(title)))
                if os.path.exists(os.path.join(SITE,tag+'_three.png')): out.append('<img src="%s_three.png" alt="%s, three-quarter view" loading="lazy">'%(tag,html.escape(title)))
                if os.path.exists(os.path.join(SITE,tag+'.gif')): out.append('<details><summary>Animation of the new fit</summary><img src="%s.gif" alt="%s animation" loading="lazy"></details>'%(tag,html.escape(title)))
                out.append('</div>')
    # every body
    out.append('<h2>Every body</h2><p class="nums">Touch error now (cm) per enemy size; bodies other than each type\'s representative keep its pose and get their own placement, heights and size. Red = doesn\'t fit (own punish), amber = questionable.</p>')
    out.append('<table><tr><th>group</th><th>files</th><th>type</th>'+''.join('<th>%s 1.0</th><th>%s 1.3</th>'%(n,n) for n in ORDER)+'</tr>')
    for g in range(len(B)):
        t=type_of(g); row='<tr><td>%d</td><td>%s</td><td>%s</td>'%(g,html.escape(' '.join(B[g]['files'][:4])+(' ...' if len(B[g]['files'])>4 else '')),t)
        for name in ORDER:
            for s in (1.0,1.3):
                m=F.get((name,g,s,True)) or F.get((name,g,s,False))
                if name=='Suplex' and t=='fat': row+='<td>%s</td>'%('Gorilla' if g!=17 and s==1.0 else ('off' if s==1.0 else '')); continue
                if name=='Spanking' and t=='fat': row+='<td class="o">%s</td>'%('off' if s==1.0 else ''); continue
                if not m:
                    row+='<td>%s</td>'%('original' if s==1.0 and ((name=='Suplex' and g==0) or (name=='Stinger' and g==2) or (name=='Cobra' and g==11) or (name=='Spanking' and g==1) or (name=='Gorilla' and g==21)) else ''); continue
                v,clip=verdict(m); cls={'fitted':'f','questionable':'q','off':'o'}[v]
                row+='<td class="%s">%.0f%s</td>'%(cls,m['e1']*100,' off' if v=='off' and s==1.0 else '')
        out.append(row+'</tr>')
    out.append('</table></main></body></html>')
    open(os.path.join(SITE,'index.html'),'w').write('\n'.join(out))
if __name__=='__main__': main()
