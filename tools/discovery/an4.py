import sys,collections,numpy as np
sys.path.insert(0,'/tmp/claude-0/-home-user/155dae35-0d01-57d8-97ff-58d0b7d66e38/scratchpad/lib')
import d6502
dt=np.dtype([('t','S1'),('v','u1'),('a','<u2'),('abs','<i4'),('cyc','<u4'),('fr','<u4')])
a=np.memmap(sys.argv[1],dtype=dt,mode='r')
rom=open(sys.argv[2],'rb').read()[16:16+131072]
seen=set(); CH=5_000_000
for s in range(0,len(a),CH):
    c=np.array(a[s:s+CH]); x=c[(c['t']==b'X')&(c['abs']>=0)]
    seen.update(np.unique(x['abs']).tolist())
print('dynamic executed instruction starts:',len(seen))
# bytes covered
cov=set()
bad=0
for p in seen:
    d=d6502.decode(rom,p)
    if d is None: bad+=1; continue
    cov.update(range(p,p+d[3]))
print('undocumented opcodes executed:',bad,'  code bytes covered:',len(cov))
# JMP indirect, JSR/JMP targets, RTS tricks
ind=[p for p in seen if rom[p]==0x6c]; print('JMP (ind) executed at',[hex(0x8000+(p&0x3fff)|(0x4000 if p>>14==7 else 0))+'@b%d'%(p>>14) for p in ind])
# classify: any executed instruction that writes to $8000-$FFFF (mapper) from banks
wr=collections.Counter()
for p in seen:
    d=d6502.decode(rom,p)
    if d and d[1] in('STA','STX','STY') and d[2] in('abs','abx','aby') and d[4][1]>=0x80: wr[(p>>14, d[4][1]<<8|d[4][0])]+=1
print('mapper store sites by (bank,addr):',sorted(wr.items())[:40], len(wr))
# PHA;PHA;RTS style dispatch
sus=[]
for p in seen:
    if rom[p]==0x60:
        sus.append(p)
print('RTS executed sites:',len(sus))
pp=[p for p in seen if rom[p]==0x48 and (p+1 in seen and rom[p+1]==0x48 or (p+3 in seen))]
print('PHA sites followed by PHA soon (dispatch candidates):',[(hex(p&0x3fff|0x8000),p>>14) for p in pp][:30])
