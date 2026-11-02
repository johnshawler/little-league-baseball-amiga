import sys,collections,numpy as np
dt=np.dtype([('t','S1'),('v','u1'),('a','<u2'),('abs','<i4'),('cyc','<u4'),('fr','<u4')])
a=np.fromfile(sys.argv[1],dtype=dt)
w=a[a['t']==b'W']
def hist(reg,mask=0xFFFF):
    m=w[(w['a']&mask)==reg]; u,c=np.unique(m['v'],return_counts=True); return {'%02x'%k:int(v) for k,v in zip(u,c)}
print('W2000',hist(0x2000)); print('W2001',hist(0x2001)); print('W4015',hist(0x4015)); print('W4016',hist(0x4016)); 
for r in (0x4000,0x4001,0x4002,0x4003,0x4004,0x4008,0x400a,0x400b,0x400c,0x400e,0x400f): print('W%04x'%r,len(w[w['a']==r]), 'distinct',len(np.unique(w[w['a']==r]['v'])))
print('writes to $4010-$4013 (DMC):',len(w[(w['a']>=0x4010)&(w['a']<=0x4013)]),' $4017:',len(w[w['a']==0x4017]), ' $6000-7fff:',len(w[(w['a']>=0x6000)&(w['a']<0x8000)]), ' $4018-$5fff:',len(w[(w['a']>=0x4018)&(w['a']<0x6000)]))
r=a[a['t']==b'R']; print('reads $6000-7fff',len(r[(r['a']>=0x6000)&(r['a']<0x8000)]),'reads 4018-5fff',len(r[(r['a']>=0x4018)&(r['a']<0x6000)]), 'reads 4015',len(r[r['a']==0x4015]),'reads 2004',len(r[r['a']==0x2004]),'reads 2007',len(r[r['a']==0x2007]))
# MMC1 serial decode
m=w[w['a']>=0x8000]
sh=0;cnt=0; regs={0:[],1:[],2:[],3:[]}; last=[None]*4; seq=collections.Counter(); vals=collections.defaultdict(collections.Counter)
for addr,v in zip(m['a'].tolist(),m['v'].tolist()):
    if v&0x80: sh=0;cnt=0; continue
    sh=(sh>>1)|((v&1)<<4); cnt+=1
    if cnt==5:
        reg=(addr>>13)&3; vals[reg][sh]+=1; sh=0;cnt=0
print('MMC1 completed register loads (reg: value:count)')
for k in range(4): print(' reg%d'%k, dict(sorted(vals[k].items())))
print('MMC1 reset (bit7) writes', int(((m['v']&0x80)!=0).sum()))
