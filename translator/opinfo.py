"""6502 (2A03) documented-opcode table: mnemonic, mode, base cycles, flag use/def, and instruction classes."""
import os,sys
# mode names: imp acc imm zp zpx zpy abs abx aby izx izy rel ind
T={}
def add(m,modes):
    for mode,op in modes.items(): T[op]=(m,mode)
add('ADC',dict(imm=0x69,zp=0x65,zpx=0x75,abs=0x6d,abx=0x7d,aby=0x79,izx=0x61,izy=0x71))
add('AND',dict(imm=0x29,zp=0x25,zpx=0x35,abs=0x2d,abx=0x3d,aby=0x39,izx=0x21,izy=0x31))
add('ASL',dict(acc=0x0a,zp=0x06,zpx=0x16,abs=0x0e,abx=0x1e))
add('BIT',dict(zp=0x24,abs=0x2c))
add('CMP',dict(imm=0xc9,zp=0xc5,zpx=0xd5,abs=0xcd,abx=0xdd,aby=0xd9,izx=0xc1,izy=0xd1))
add('CPX',dict(imm=0xe0,zp=0xe4,abs=0xec)); add('CPY',dict(imm=0xc0,zp=0xc4,abs=0xcc))
add('DEC',dict(zp=0xc6,zpx=0xd6,abs=0xce,abx=0xde)); add('EOR',dict(imm=0x49,zp=0x45,zpx=0x55,abs=0x4d,abx=0x5d,aby=0x59,izx=0x41,izy=0x51))
add('INC',dict(zp=0xe6,zpx=0xf6,abs=0xee,abx=0xfe)); add('LDA',dict(imm=0xa9,zp=0xa5,zpx=0xb5,abs=0xad,abx=0xbd,aby=0xb9,izx=0xa1,izy=0xb1))
add('LDX',dict(imm=0xa2,zp=0xa6,zpy=0xb6,abs=0xae,aby=0xbe)); add('LDY',dict(imm=0xa0,zp=0xa4,zpx=0xb4,abs=0xac,abx=0xbc))
add('LSR',dict(acc=0x4a,zp=0x46,zpx=0x56,abs=0x4e,abx=0x5e)); add('ORA',dict(imm=0x09,zp=0x05,zpx=0x15,abs=0x0d,abx=0x1d,aby=0x19,izx=0x01,izy=0x11))
add('ROL',dict(acc=0x2a,zp=0x26,zpx=0x36,abs=0x2e,abx=0x3e)); add('ROR',dict(acc=0x6a,zp=0x66,zpx=0x76,abs=0x6e,abx=0x7e))
add('SBC',dict(imm=0xe9,zp=0xe5,zpx=0xf5,abs=0xed,abx=0xfd,aby=0xf9,izx=0xe1,izy=0xf1))
add('STA',dict(zp=0x85,zpx=0x95,abs=0x8d,abx=0x9d,aby=0x99,izx=0x81,izy=0x91)); add('STX',dict(zp=0x86,zpy=0x96,abs=0x8e)); add('STY',dict(zp=0x84,zpx=0x94,abs=0x8c))
for m,op in dict(BPL=0x10,BMI=0x30,BVC=0x50,BVS=0x70,BCC=0x90,BCS=0xb0,BNE=0xd0,BEQ=0xf0).items(): T[op]=(m,'rel')
for m,op in dict(BRK=0x00,CLC=0x18,CLD=0xd8,CLI=0x58,CLV=0xb8,DEX=0xca,DEY=0x88,INX=0xe8,INY=0xc8,NOP=0xea,PHA=0x48,PHP=0x08,PLA=0x68,PLP=0x28,RTI=0x40,RTS=0x60,SEC=0x38,SED=0xf8,SEI=0x78,TAX=0xaa,TAY=0xa8,TSX=0xba,TXA=0x8a,TXS=0x9a,TYA=0x98).items(): T[op]=(m,'imp')
T[0x4c]=('JMP','abs'); T[0x6c]=('JMP','ind'); T[0x20]=('JSR','abs')
LEN=dict(imp=1,acc=1,imm=2,zp=2,zpx=2,zpy=2,izx=2,izy=2,rel=2,abs=3,abx=3,aby=3,ind=3)
# base cycles (without page-cross / branch penalties)
def base_cycles(m,mode):
    if m in('LDA','LDX','LDY','AND','ORA','EOR','ADC','SBC','CMP','CPX','CPY','BIT'):
        return dict(imm=2,zp=3,zpx=4,zpy=4,abs=4,abx=4,aby=4,izx=6,izy=5)[mode]
    if m in('STA','STX','STY'): return dict(zp=3,zpx=4,zpy=4,abs=4,abx=5,aby=5,izx=6,izy=6)[mode]
    if m in('ASL','LSR','ROL','ROR','INC','DEC'): return dict(acc=2,zp=5,zpx=6,abs=6,abx=7)[mode]
    return dict(BRK=7,CLC=2,CLD=2,CLI=2,CLV=2,DEX=2,DEY=2,INX=2,INY=2,NOP=2,PHA=3,PHP=3,PLA=4,PLP=4,RTI=6,RTS=6,SEC=2,SED=2,SEI=2,TAX=2,TAY=2,TSX=2,TXA=2,TXS=2,TYA=2,
                JSR=6,BPL=2,BMI=2,BVC=2,BVS=2,BCC=2,BCS=2,BNE=2,BEQ=2).get(m, 3 if (m=='JMP' and mode=='abs') else 5 if m=='JMP' else None)
# page-cross penalty on reads (not stores / RMW, which always take the extra cycle already counted in base)
PENALTY_READ={'LDA','LDX','LDY','AND','ORA','EOR','ADC','SBC','CMP'}
N,Z,C,V=1,2,4,8
ALL=15
# flags used / defined (D,I,B not tracked: I is always set in this game, D never used)
FUSE={ 'ADC':(C,N|Z|C|V),'SBC':(C,N|Z|C|V),'AND':(0,N|Z),'ORA':(0,N|Z),'EOR':(0,N|Z),'ASL':(0,N|Z|C),'LSR':(0,N|Z|C),'ROL':(C,N|Z|C),'ROR':(C,N|Z|C),
       'BIT':(0,N|Z|V),'CMP':(0,N|Z|C),'CPX':(0,N|Z|C),'CPY':(0,N|Z|C),'DEC':(0,N|Z),'INC':(0,N|Z),'DEX':(0,N|Z),'DEY':(0,N|Z),'INX':(0,N|Z),'INY':(0,N|Z),
       'LDA':(0,N|Z),'LDX':(0,N|Z),'LDY':(0,N|Z),'TAX':(0,N|Z),'TAY':(0,N|Z),'TXA':(0,N|Z),'TYA':(0,N|Z),'TSX':(0,N|Z),'PLA':(0,N|Z),
       'CLC':(0,C),'SEC':(0,C),'CLV':(0,V),'PHP':(ALL,0),'PLP':(0,ALL),'RTI':(0,ALL),
       'BPL':(N,0),'BMI':(N,0),'BNE':(Z,0),'BEQ':(Z,0),'BCC':(C,0),'BCS':(C,0),'BVC':(V,0),'BVS':(V,0)}
def uses_defs(m): return FUSE.get(m,(0,0))
