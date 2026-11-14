#!/usr/bin/env python3
"""Self-time by symbol for the thread running the EE scheduler, from a macOS `sample` file.
usage: sample_thread.py FILE [top=45]"""
import re,sys,collections
L=open(sys.argv[1],errors='replace').read().splitlines()
top=int(sys.argv[2]) if len(sys.argv)>2 else 45
start=[k for k,l in enumerate(L) if l.startswith('Call graph:')][0]
end=[k for k,l in enumerate(L) if l.startswith('Total number in stack')][0]
# split into threads
blocks=[];cur=None
for l in L[start+1:end]:
    if re.match(r'^\s{4}\d+ Thread_',l):
        cur=[];blocks.append(cur)
    if cur is not None: cur.append(l)
def parse(block):
    nodes=[]  # (depth,count,sym)
    for l in block[1:]:
        m=re.match(r'^([\s+!:|]*)(\d+) (.+?)(?:  \(in ([^)]+)\))?(?: \+ .*| \[.*)?$',l)
        if not m: continue
        nodes.append((len(m.group(1)),int(m.group(2)),m.group(3).strip()))
    return nodes
best=None
for b in blocks:
    if any('EeScheduler::run' in l for l in b): best=b
nodes=parse(best)
selfc=collections.Counter(); incl=collections.Counter()
for i,(d,c,s) in enumerate(nodes):
    child=0
    for (d2,c2,s2) in nodes[i+1:]:
        if d2<=d: break
        if d2==min(x[0] for x in nodes[i+1:i+1+200] if x[0]>d) : pass
    # direct children: next nodes with depth > d until depth<=d; direct = those whose depth equals first child's depth... depths vary; use stack method below
stack=[]
for (d,c,s) in nodes:
    while stack and stack[-1][0]>=d: stack.pop()
    if stack: stack[-1][3][0]+=c
    ent=[d,c,s,[0]]
    stack.append(ent)
    selfc[s]+=0
    ent.append(None)
# second pass for self: recompute with child sums
stack=[];records=[]
for (d,c,s) in nodes:
    while stack and stack[-1][0]>=d: stack.pop()
    rec=[d,c,s,0]
    if stack: stack[-1][3]+=c
    stack.append(rec);records.append(rec)
total=records[0][1] if records else 0
for d,c,s,ch in records: selfc[s]+=max(c-ch,0)
idle=('__psynch_cvwait','semaphore_wait','mach_msg','__ulock_wait','usleep','nanosleep','__semwait','swtch_pri','thread_switch','mach_wait_until','__psynch_mutexwait')
busy={s:n for s,n in selfc.items() if n>0 and not any(s.startswith(x) for x in idle)}
bt=sum(busy.values())
gen=sum(n for s,n in busy.items() if re.search(r'_0x[0-9a-f]{6}\(',s))
print(f'game thread samples {total}, busy {bt} ({bt*100/max(total,1):.1f}%), generated guest code {gen*100/max(bt,1):.1f}% of busy')
cats=collections.Counter()
for s,n in busy.items():
    if re.search(r'_0x[0-9a-f]{6}\(',s): cats['generated guest functions']+=n
    elif 'iop' in s.lower() or 'Spu2' in s or 'Iop' in s: cats['IOP / SPU2']+=n
    elif 'EeScheduler' in s or 'eeCheckpoint' in s or 'advanceEeTimers' in s or 'dispatchGuestBranch' in s or 'PS2Memory' in s: cats['scheduler / timers / dispatch']+=n
    elif 'p3fes' in s or 'native_renderer' in s: cats['native hooks (p3fes)']+=n
    elif 'ps2_stubs' in s or 'ps2_syscalls' in s or 'PS2Runtime' in s: cats['runtime stubs / syscalls']+=n
    else: cats['other (libc, locks, ...)']+=n
for k,n in cats.most_common(): print(f'  {n*100/bt:5.1f}%  {k}')
print('--- top symbols (self)')
for s,n in sorted(busy.items(),key=lambda x:-x[1])[:top]: print(f'{n*100/bt:5.1f}%  {s[:110]}')
