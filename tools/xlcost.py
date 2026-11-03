#!/usr/bin/env python3
"""xlcost.py code.s seen_weights.txt : average number of 68020 instructions emitted per 6502 instruction, weighted by execution counts.
weights file: lines 'ctx addr count'."""
import sys,re,collections
cnt=collections.Counter(); cur=None
for line in open(sys.argv[1]):
    m=re.match(r'\t\| @(\d):([0-9A-F]{4})',line)
    if m: cur=(int(m.group(1)),int(m.group(2),16)); continue
    if cur and line.startswith('\t') and not line.startswith('\t.') and not line.startswith('\t|'): cnt[cur]+=1
    elif line[0] not in '\t' : 
        if not line.startswith('.L'): cur=None
tot=0;ins=0
for l in open(sys.argv[2]):
    c,a,n=l.split(); k=(int(c),int(a)); n=int(n)
    if k in cnt: tot+=cnt[k]*n; ins+=n
print('executed 6502 instructions (translated) %d, 68k instructions %d, ratio %.2f'%(ins,tot,tot/max(ins,1)))
