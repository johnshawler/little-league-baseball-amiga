import sys,collections,numpy as np
dt=np.dtype([('t','S1'),('v','u1'),('a','<u2'),('abs','<i4'),('cyc','<u4'),('fr','<u4')])
a=np.memmap(sys.argv[1],dtype=dt,mode='r')
n=len(a)
# chunk processing
hot=collections.Counter(); regw=collections.Counter(); regr=collections.Counter()
frstart={}
nmi=[]  # cycles of NMI vector entries (exec at the NMI handler address)
pos=collections.defaultdict(list)
CH=5_000_000
for s in range(0,n,CH):
    c=np.array(a[s:s+CH])
    x=c[c['t']==b'X']
    u,cn=np.unique(x['abs'][x['abs']>=0],return_counts=True)
    for k,v in zip(u.tolist(),cn.tolist()): hot[k]+=v
    w=c[c['t']==b'W']
    u,cn=np.unique(w['a'],return_counts=True)
    for k,v in zip(u.tolist(),cn.tolist()):
        if k<0x8000: regw[k]+=v
    r=c[c['t']==b'R']
    u,cn=np.unique(r['a'],return_counts=True)
    for k,v in zip(u.tolist(),cn.tolist()): regr[k]+=v
    for f,(lo) in []: pass
    for reg in (0x2000,0x2001,0x2005,0x2006,0x2003,0x4014,0x4015,0x4010,0x4011,0x4017,0xa000,0xc000,0xe000):
        m=w[(w['a']&0xFFFF)==reg] if reg<0x8000 else w[(w['a']&0xE000)==reg]
        if len(m): pos[reg].append((m['cyc'],m['fr']))
print('top hot abs addrs (bank:addr  count)'); 
for k,v in hot.most_common(12): print('  %d:%04x'%(k>>14,0x8000+(k&0x3fff) if (k>>14)!=7 else 0xC000+(k&0x3fff)),v)
print('reg writes'); 
for k,v in sorted(regw.items()): print('  %04x %d'%(k,v))
print('reg reads');
for k,v in sorted(regr.items()): print('  %04x %d'%(k,v))
