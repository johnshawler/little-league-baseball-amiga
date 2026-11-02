import sys,collections,numpy as np
dt=np.dtype([('t','S1'),('v','u1'),('a','<u2'),('abs','<i4'),('cyc','<u4'),('fr','<u4')])
a=np.memmap(sys.argv[1],dtype=dt,mode='r')
rom=np.frombuffer(open(sys.argv[2],'rb').read()[16:16+131072],dtype=np.uint8)
CH=6_000_000
indT=collections.defaultdict(collections.Counter); rtsBad=collections.defaultdict(collections.Counter)
prev=None
for s in range(0,len(a),CH):
    c=np.array(a[s:s+CH]); x=c[c['t']==b'X']
    if prev is not None: x=np.concatenate([prev,x])
    prev=x[-1:]
    op=x['v'][:-1]; pc=x['a'][:-1]; ab=x['abs'][:-1]; npc=x['a'][1:]; nab=x['abs'][1:]
    # JMP ind
    m=np.where(op==0x6c)[0]
    for i in m: indT[(int(ab[i]))][int(nab[i])]+=1
    m=np.where(op==0x60)[0]
    for i in m:
        n=int(nab[i])
        if n<0: rtsBad[int(ab[i])][('nonrom',int(npc[i]))]+=1; continue
        j=n-3
        ok = j>=0 and rom[j]==0x20
        # return into other bank: check at the same bank base if abs<=? accept
        if not ok: rtsBad[int(ab[i])][n]+=1
def nm(p): return '%d:%04x'%(p>>14,(0xC000 if p>>14==7 else 0x8000)+(p&0x3fff))
print('JMP (ind) sites and targets:')
for k,v in sorted(indT.items()): print(' ',nm(k),'->',[(nm(t),n) for t,n in v.most_common(30)])
print('RTS whose return address is not after a JSR (RTS-dispatch / trampolines):',len(rtsBad))
for k,v in sorted(rtsBad.items()): print(' ',nm(k),len(v),'targets, e.g.',[(nm(t) if isinstance(t,int) else t,n) for t,n in v.most_common(4)])
