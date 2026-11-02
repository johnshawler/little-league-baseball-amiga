import sys,struct,collections
import numpy as np
dt=np.dtype([('t','S1'),('v','u1'),('a','<u2'),('abs','<i4'),('cyc','<u4'),('fr','<u4')])
# NB struct: char,uint8,uint16,int32,uint32,uint32 -> 1+1+2+4+4+4=16
a=np.fromfile(sys.argv[1],dtype=dt)
print(len(a),'records')
x=a[a['t']==b'X']
print('exec instrs',len(x))
rom=x[x['abs']>=0]
pcs=set(zip(rom['abs'].tolist()))
print('distinct PRG instruction addrs executed:',len(pcs))
ram=x[(x['abs']<0)&(x['a']<0x8000)]
print('exec from non-ROM addrs (RAM etc):',len(ram), sorted(set(ram['a'].tolist()))[:20])
# per-bank coverage
bk=collections.Counter(p[0]>>14 for p in pcs); print('instr addrs per 16K bank',sorted(bk.items()))
# mapper writes
w=a[(a['t']==b'W')&(a['a']>=0x8000)]
c=collections.Counter((int(r['a'])&0xE000, int(r['v'])) for r in w); print('mapper reg writes (reg,val):',len(w)); 
for k,v in sorted(c.items()): print(hex(k[0]),'%02x'%k[1],v)
