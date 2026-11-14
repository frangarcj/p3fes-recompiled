#!/usr/bin/env python3
"""Summarize [ee-sample] windows of a runtime log. usage: sample_report.py LOG [windows=4]"""
import re,csv,bisect,collections,sys
L=open(sys.argv[1],errors='replace').read()
for tag in ('[ee-sample]','[speed]','[perf]'): L=L.replace(tag,'\n'+tag)
L=L.splitlines(); nw=int(sys.argv[2]) if len(sys.argv)>2 else 4
sp=[l for l in L if l.startswith('[speed]')]
print('vblanks/frames:',' '.join(re.sub(r'\[speed\] ([0-9.]+) vblanks.*, ([0-9.]+) game.*',r'\1/\2',l) for l in sp)[-330:])
starts=[i for i,l in enumerate(L) if l.startswith('[ee-sample] total=')]
fn=[]
for r in csv.reader(open('/Users/frangar/Fun/pes3fes/p3fes/p3fes_map.csv')):
    try: fn.append((int(r[1],16),int(r[2],16),r[0]))
    except: pass
fn.sort(); ks=[f[0] for f in fn]
def name(pc):
    if pc==0: return '(top level / scheduler / wait)'
    if pc==0xFFFFFFF0: return '(IOP)'
    i=bisect.bisect_right(ks,pc)-1
    return fn[i][2] if i>=0 and fn[i][0]<=pc<fn[i][1] else hex(pc)
agg=collections.Counter(); W=starts[-nw:]; tot=nat=0
for st in W:
    m=re.match(r'\[ee-sample\] total=(\d+) native=(\d+)',L[st]); tot+=int(m.group(1)); nat+=int(m.group(2))
    for l in L[st+1:]:
        m=re.match(r'\[ee-sample\] pc=([0-9A-F]+) n=(\d+)( native)?',l)
        if not m:
            if l.startswith('[ee-sample] total'): break
            continue
        agg[(name(int(m.group(1),16)),bool(m.group(3)))]+=int(m.group(2))
print('windows',len(starts),'| last',len(W),': total',tot,'native %.1f%%'%(nat*100/max(tot,1)))
for (nm,isn),n in agg.most_common(36): print(f'{n*100/tot:5.1f}%  {"NATIVE " if isn else "       "}{nm}')
print('listed %.1f%%'%(sum(agg.values())*100/max(tot,1)))
