#!/usr/bin/env python3
"""Generates the synthetic ROM and the test list for the per-instruction tests of the translator (all documented opcodes and addressing forms
that are translated; instructions that are left to the interpreter are excluded)."""
import sys,os,random
sys.path.insert(0,os.path.join(os.path.dirname(__file__),'..','..','translator'))
from opinfo import *
import x65
random.seed(12345)
prg=bytearray(random.getrandbits(8) for _ in range(131072))
# fixed bank layout: $C000.. RTS target for JMP/JSR tests at $C010, test programs from $C100
def fixed(a): return 7*16384+(a-0xC000)
for a in range(0xC000,0xC100): prg[fixed(a)]=0x60
tests=[]; pos=0xC100
def add(code,kind='',name=''):
    global pos
    code=bytes(code)+b'\x60'
    for k,b in enumerate(code): prg[fixed(pos+k)]=b
    if code[0] in T and x65.needs_interp(x65.Ins(x65.Rom(bytes(prg)),7,pos)): return
    tests.append((pos,len(code),code,kind,name)); pos+=(len(code)+3)&~3
    assert pos<0xFF00
ops={}
for op,(m,mode) in T.items(): ops.setdefault(m,{})[mode]=op
ram_abs=[0x0200,0x07FF,0x0345]; zps=[0x00,0x10,0x7F,0xFE,0xFF]
rom_abs=[0x8000,0x8123,0xBFFF,0xC800,0xD0F0,0xFFEE]
READ=['LDA','LDX','LDY','AND','ORA','EOR','ADC','SBC','CMP','CPX','CPY','BIT']
STORE=['STA','STX','STY']
RMW=['ASL','LSR','ROL','ROR','INC','DEC']
for m,modes in sorted(ops.items()):
    for mode,op in sorted(modes.items()):
        if m in('BRK','JMP','RTS','RTI','JSR') or mode in('rel',): continue
        if mode=='izx': continue
        if mode in('imp','acc'):
            if m=='PHA': add([op,0x68],'',m)
            elif m=='PHP': add([op,0x68],'',m)
            elif m in('PLA','PLP'): add([op],'pull',m)
            elif m=='TXS': add([op],'txs',m)
            else: add([op],'',f'{m} {mode}')
            continue
        if mode=='imm':
            for v in (0x00,0x01,0x7F,0x80,0xFF,0x55,0xAA): add([op,v],'',f'{m} #{v:02x}')
            continue
        if mode in('zp','zpx','zpy'):
            for z in zps: add([op,z],'',f'{m} {mode} {z:02x}')
            continue
        if mode=='abs':
            addrs=ram_abs[:]
            if m in READ: addrs+=rom_abs
            for a in addrs: add([op,a&0xFF,a>>8],'',f'{m} abs {a:04x}')
            continue
        if mode in('abx','aby'):
            addrs=[0x0200,0x03FF,0x0480,0x0701,0x07FF-0xFE]
            if m in READ: addrs+=[0x8000,0x80FF,0x9234,0xC800,0xC8FF,0xD0F0]
            if m in RMW and mode=='aby': continue
            for a in addrs:
                if m in RMW+STORE and a>=0x8000: continue
                add([op,a&0xFF,a>>8],'',f'{m} {mode} {a:04x}')
            continue
        if mode=='izy':
            for z in (0x20,0xFF):
                add([op,z],'izy_w' if m in STORE else 'izy_r',f'{m} izy {z:02x}')
            continue
        raise Exception((m,mode))
# branches

for op,(m,mode) in sorted(T.items()):
    if mode=='rel': add([op,2,0xA2,0x55],'',f'{m} rel')
add([0x4C,0x10,0xC0],'', 'JMP abs'); add([0x20,0x10,0xC0],'','JSR abs')
for z in (0x30,0xFF-0xFF+0x40): pass
# JMP indirect through a RAM pointer
for a in (0x0030,0x0200,0x07FE):
    add([0x6C,a&0xFF,a>>8],'jmpind',f'JMP ind {a:04x}')
add([],'', 'RTS only')
add([0x40],'rti','RTI')
# random sequences (exercise the flag liveness / fusion paths): forward branches only, RAM/ROM operands, no stack or control transfers
import random as _r
_r.seed(777)
seqops=[]
for op,(m,mode) in sorted(T.items()):
    if m in('BRK','JMP','JSR','RTS','RTI','PHA','PHP','PLA','PLP','TXS') or mode=='izx': continue
    seqops.append(op)
def rand_ins(op):
    m,mode=T[op]
    if mode in('imp','acc'): return [op]
    if mode=='imm': return [op,_r.choice([0,1,0x7F,0x80,0xFF,_r.randrange(256)])]
    if mode in('zp','zpx','zpy'): return [op,_r.choice([0x10,0x11,0x80,0xFF,0x20])]
    if mode=='abs':
        a=_r.choice([0x0200,0x0345,0x07FF]+([0x8123,0xC800] if m in READ else [])); return [op,a&0xFF,a>>8]
    if mode in('abx','aby'):
        a=_r.choice([0x0200,0x03FF,0x0480]+([0x8000,0x80FF,0xC800] if m in READ else [])); return [op,a&0xFF,a>>8]
    if mode=='izy': return [op,0x20]
    return None
brs=[op for op,(m,mode) in T.items() if mode=='rel']
for n in range(500):
    L=_r.randint(2,7); prog=[]
    for k in range(L):
        if _r.random()<0.3: prog.append(('br',_r.choice(brs)))
        else:
            while True:
                op=_r.choice(seqops); ins=rand_ins(op)
                if ins and not x65.needs_interp(x65.Ins(x65.Rom(bytes(bytearray(ins)+bytes(3))),0,0) if False else type('I',(),{'m':T[op][0],'mode':T[op][1],'arg':(ins[1]|(ins[2]<<8 if len(ins)>2 else 0)) if len(ins)>1 else 0})()): break
            prog.append(('i',ins))
    code=[]
    for k,(t,v) in enumerate(prog):
        if t=='i': code+=v
        else:
            skip=0
            for t2,v2 in prog[k+1:k+3][:_r.randint(1,2)]: skip+=len(v2) if t2=='i' else 2
            code+=[v,skip]
    kind='izy_r' if any(t=='i' and T[v[0]][1]=='izy' for t,v in prog) else ''
    # izy tests need pointer set-up for zp 0x20: only reads/writes of RAM/ROM; the driver handles kind izy_r (ptr into RAM/ROM): stores would hit ROM -> avoid stores
    if kind and any(t=='i' and T[v[0]][0] in STORE+RMW for t,v in prog): continue
    code=bytes(code)
    if any(t=='i' and T[v[0]][1] in('abx','aby') and T[v[0]][0] in STORE+RMW and v[1]|v[2]<<8>=0x8000 for t,v in prog): continue
    add(list(code),kind if kind else '', 'SEQ '+' '.join('%02x'%b for b in code))
# seen = all instruction addresses of the tests
seen=[(7,0xC010)]
for (a,n,code,kind,name) in tests:
    k=0
    while k<n:
        op=code[k]; seen.append(((7,a+k))); k+=LEN[T[op][1]]
out=sys.argv[1]; os.makedirs(out,exist_ok=True)
open(os.path.join(out,'prg.bin'),'wb').write(prg)
with open(os.path.join(out,'tests.txt'),'w') as f:
    for (a,n,code,kind,name) in tests:
        f.write('%d %d %s %s|%s\n'%(a,n,kind or '-',' '.join('%02x'%b for b in code),name))
x65.translate(bytes(prg),seen,os.path.join(out,'code.s'),os.path.join(out,'tab.s'),os.path.join(out,'report.txt'),extra_entries=[(7,0xFFF0)])
print(len(tests),'test programs')
