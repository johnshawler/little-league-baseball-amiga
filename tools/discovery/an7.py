import sys,collections,numpy as np
sys.path.insert(0,'/tmp/claude-0/-home-user/155dae35-0d01-57d8-97ff-58d0b7d66e38/scratchpad/lib')
import d6502
rom=open(sys.argv[2],'rb').read()[16:16+131072]
dt=np.dtype([('t','S1'),('v','u1'),('a','<u2'),('abs','<i4'),('cyc','<u4'),('fr','<u4')])
seen=set(int(l) for l in open(sys.argv[1]))
def off(bank,addr): return bank*16384+(addr-(0xC000 if bank==7 else 0x8000))
def inwin(addr): return 0x8000<=addr<0xC000
work=[(p>>14, (0xC000 if p>>14==7 else 0x8000)+(p&0x3fff)) for p in seen]
done=set(); ramexec=set(); bad=[]; jmpind=set()
while work:
    b,ad=work.pop()
    while True:
        if ad<0x8000: ramexec.add(ad); break
        if ad>=0xC000: b=7
        if (b,ad) in done: break
        bb=b
        o=off(bb,ad)
        d=d6502.decode(rom,o)
        if d is None: bad.append((bb,ad)); break
        done.add((b,ad))
        op,m,mode,n,by=d
        nxt=ad+n
        if mode=='rel':
            t=nxt+(by[0]-256 if by[0]>127 else by[0]); work.append((b,t)); ad=nxt; continue
        if m in('JSR','JMP') and mode=='abs':
            t=by[0]|by[1]<<8
            if inwin(t) and bb==7:
                for k in range(7): work.append((k,t))   # caller in fixed bank: bank unknown -> any switchable bank (upper bound)
            else: work.append((bb,t))
            if m=='JMP': break
            ad=nxt; continue
        if m=='JMP': jmpind.add((bb,ad)); break
        if m in('RTS','RTI','BRK'): break
        ad=nxt
code=set()
for (b,ad) in done:
    o=off(7 if ad>=0xC000 else b,ad); d=d6502.decode(rom,o); code.update(range(o,o+d[3]))
print('static closure from dynamically seen entries: instructions',len(done),'bytes',len(code),'of',len(rom))
print('control transfers into RAM/IO space (<8000):',sorted(hex(x) for x in ramexec)[:20])
print('undecodable (illegal opcode) reached:',len(bad),[ (b,hex(a)) for b,a in bad[:10]])
print('indirect jump sites in closure:',[(b,hex(a)) for b,a in sorted(jmpind)])
percode=collections.Counter(o>>14 for o in code); print('code bytes per bank',sorted(percode.items()))
