import random,sys
seed=int(sys.argv[1]); mode=int(sys.argv[2]); n=int(sys.argv[3]); two=int(sys.argv[4]) if len(sys.argv)>4 else 0
random.seed(seed)
ev=[(0,0),(40,8),(44,0)]
f=150
for k in range(mode): ev+=[(f,0x20),(f+4,0)]; f+=30
ev+=[(f,0x100),(f+4,0)]; f+=60
if mode==1:   # exhibition: 1P / 2P / watch
    sel=2 if seed%3==0 else (1 if two else 0)
    for k in range(sel): ev+=[(f,0x20),(f+4,0)]; f+=30
    ev+=[(f,0x100),(f+4,0)]; f+=60
while f<n:
    m=random.choice([0x100,0x100,0x1,0x10,0x20,0x40,0x80,0x100|0x10,0x100|0x40,0x8,0x8,0x4])
    if two and random.random()<0.5: m|=random.choice([0x100,0x1,0x10,0x20,0x40,0x80,0x8])<<16
    ev+=[(f,m),(f+random.choice([2,4,8]),0)]; f+=random.choice([15,25,40,70])
open(sys.argv[5] if len(sys.argv)>5 else 'cov.txt','w').write(''.join('%d %x\n'%e for e in ev))
