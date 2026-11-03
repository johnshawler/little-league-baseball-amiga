#!/usr/bin/env python3
"""x65.py - mechanical 6502 -> 68020 translator (static, per superblock).  See DESIGN.md "Translator" for the conventions:

  d0=A d1=X d2=Y (X,Y,S zero-extended)  d3=S  d4=C (0x00/0xFF)  d5=cycle budget  d6,d7=temps
  a0=RAM a1=window PRG base(-0x8000 biased)  a2=context  a3/a4=window/fixed dispatch tables (biased)  a5=nz table  a6=fixed PRG base (-0x4000 biased)
  context: nzc (P layout N/Z), vflag (bit6=V), pmisc (I/D bits), ...
"""
import sys,os,collections
sys.path.insert(0,os.path.dirname(__file__))
from opinfo import *

CTX_NZC=0; CTX_V=1; CTX_MISC=2     # context byte offsets (a2)

class Rom:
    def __init__(self,prg): self.prg=prg
    def off(self,ctx,addr): return ctx*16384+(addr-(0xC000 if ctx==7 else 0x8000))
    def b(self,ctx,addr): return self.prg[self.off(ctx,addr)]
def region(a):
    if a<0x2000: return 'ram'
    if a<0x4000: return 'ppu'
    if a<0x8000: return 'io'
    if a<0xC000: return 'win'
    return 'fix'

class Ins:
    __slots__=('ctx','addr','op','m','mode','n','arg','cyc','nxt','live','idx')
    def __init__(s,rom,ctx,addr):
        s.ctx=ctx; s.addr=addr; s.op=rom.b(ctx,addr); s.m,s.mode=T[s.op]; s.n=LEN[s.mode]
        a=0
        if s.n==2: a=rom.b(ctx,addr+1)
        elif s.n==3: a=rom.b(ctx,addr+1)|rom.b(ctx,addr+2)<<8
        s.arg=a; s.cyc=base_cycles(s.m,s.mode); s.nxt=(addr+s.n)&0xFFFF; s.live=0
    def target(s):
        if s.mode=='rel': return (s.nxt+(s.arg-256 if s.arg>127 else s.arg))&0xFFFF
        return s.arg
    def __repr__(s): return '%d:%04x %s %s %x'%(s.ctx,s.addr,s.m,s.mode,s.arg)

def ctx_of(cur_ctx,addr):
    return 7 if addr>=0xC000 else cur_ctx

COND={'BPL':('N',0),'BMI':('N',1),'BNE':('Z',0),'BEQ':('Z',1),'BCC':('C',0),'BCS':('C',1),'BVC':('V',0),'BVS':('V',1)}
TERM_JMP={'JMP','RTS','RTI','BRK'}
IO_TERM_ADDR={0x2000,0x4014}      # stores that must end the superblock (they can change event times)

class Prog:
    def __init__(self,rom,seen):
        self.rom=rom; self.ins={}; self.seen=set(seen)
        for (c,a) in seen:
            if (c,a) not in self.ins:
                try: self.ins[(c,a)]=Ins(rom,c,a)
                except KeyError: pass
        self.find_leaders()
    def succs(self,i):
        """static successors inside the same code context: list of (ctx,addr) that may be reached by direct control flow"""
        s=[]
        if i.m in COND_B: s.append((i.ctx,i.nxt)); s.append((ctx_of(i.ctx,i.target()),i.target()))
        elif i.m=='JMP' and i.mode=='abs': s.append((ctx_of(i.ctx,i.arg),i.arg))
        elif i.m=='JSR': s.append((ctx_of(i.ctx,i.arg),i.arg)); s.append((i.ctx,i.nxt))
        elif i.m in('RTS','RTI','BRK') or (i.m=='JMP'): pass
        else: s.append((ctx_of(i.ctx,i.nxt) if i.nxt>=0xC000 else i.ctx,i.nxt))
        return s
    def find_leaders(self):
        L=set(); haspred=set()
        for k,i in self.ins.items():
            if i.m in COND_B:
                L.add((ctx_of(i.ctx,i.target()),i.target())) 
            if i.m in('JMP','JSR') and i.mode=='abs': L.add((ctx_of(i.ctx,i.arg),i.arg))
            if i.m=='JSR': L.add((i.ctx,i.nxt))
            if is_io_term(i): L.add((i.ctx,i.nxt))
            for s in self.succs(i):
                if i.m in COND_B and s==(i.ctx,i.nxt): haspred.add(s)
                elif i.m not in COND_B and i.m not in ('JSR','JMP'): haspred.add(s)
        # window targets reached from fixed code: every bank where that address was seen
        for (c,a) in list(L):
            if c!=7 and a<0xC000 and (c,a) not in self.ins:
                pass
        for k in self.ins:
            if k not in haspred: L.add(k)       # roots: entered by dispatch / interrupt return
        self.leaders={k for k in L if k in self.ins}
        # fixed-code references to window addresses: leaders in every bank where seen
        for i in self.ins.values():
            if i.ctx==7 and i.m in('JMP','JSR') and i.mode=='abs' and 0x8000<=i.arg<0xC000:
                for b in range(7):
                    if (b,i.arg) in self.ins: self.leaders.add((b,i.arg))
            if i.ctx==7 and i.m in COND_B: pass
COND_B=set(COND)
def is_io_term(i):
    """instructions after which a new superblock must start because they may change event times / mapper state"""
    if i.m in ('STA','STX','STY'):
        if i.mode=='abs' and (i.arg in IO_TERM_ADDR or i.arg>=0x8000 or 0x2000<=i.arg<0x8000): return True
        if i.mode in('zp','zpx','zpy'): return False
        if i.mode=='abs': return False
        if i.mode in('abx','aby'): return not (i.arg+255<0x800)     # static base: RAM only if it cannot leave RAM
        return True                                                 # izx/izy: unknown destination
    if i.m in('INC','DEC','ASL','LSR','ROL','ROR'):
        if i.mode=='abs' and i.arg>=0x2000: return True
        if i.mode in('abx',) and not (i.arg+255<0x800): return True
    return False

# ----------------------------------------------------------------------------------------------------------------------------------------
class Block:
    def __init__(self,key): self.key=key; self.ins=[]; self.tot=0; self.fall=None   # fall: (ctx,addr) the block continues into, or None

def needs_interp(i):
    """instructions that are left to the interpreter (rare or exotic forms)"""
    if i.mode=='izx': return True
    if i.m=='BRK': return True
    if i.m=='JMP' and i.mode=='ind': return not (region(i.arg)=='ram' and (i.arg&0xFF)!=0xFF)
    if i.m in('INC','DEC','ASL','LSR','ROL','ROR') and i.mode!='acc':
        if i.mode=='abs': return region(i.arg)!='ram'
        if i.mode in('abx','aby'): return not (region(i.arg)=='ram' and (i.arg&0x7FF)+255<0x800)
    return False

def build_blocks(prog):
    blocks={}
    for key in sorted(prog.leaders):
        b=Block(key); ctx,a=key; cur=prog.ins.get(key)
        seen_here=set()
        while cur is not None:
            if needs_interp(cur):
                if b.ins: b.fall=(cur.ctx,cur.addr)
                else: b.ins=None
                break
            b.ins.append(cur)
            if cur.m in TERM_JMP or cur.m=='JSR': break
            if cur.m=='JMP': break
            if is_io_term(cur) or cur.m in('STA','STX','STY') and False: pass
            nk=(ctx_of(cur.ctx,cur.nxt) if cur.nxt>=0xC000 else cur.ctx,cur.nxt)
            if is_io_term(cur):
                b.fall=nk; break
            if nk in prog.leaders: b.fall=nk; break
            nxt=prog.ins.get(nk)
            if nxt is None: b.fall=nk; break          # unseen code: continue in the interpreter
            cur=nxt
        if b.ins: blocks[key]=b
    return blocks

def label(key): c,a=key; return ('F_%04X'%a) if c==7 else ('B%d_%04X'%(c,a))

def liveness(prog,blocks):
    live_in={k:0 for k in blocks}
    def target_live(key):
        return live_in[key] if key in live_in else ALL      # not a translated leader: interpreter, assume everything live
    changed=True; it=0
    while changed:
        changed=False; it+=1
        for k,b in blocks.items():
            # live_out at the end of the block
            last=b.ins[-1]
            if last.m in('RTS','RTI','BRK') or (last.m=='JMP' and last.mode=='ind'): live=ALL
            elif last.m=='JMP' or last.m=='JSR': live=target_live((ctx_of(last.ctx,last.arg),last.arg)) if not (last.m=='JMP' and last.ctx==7 and 0x8000<=last.arg<0xC000) else ALL
            elif b.fall is not None: live=target_live(b.fall)
            else: live=ALL
            if last.m=='JSR': live=ALL if not (ctx_of(last.ctx,last.arg),last.arg) in live_in else live_in[(ctx_of(last.ctx,last.arg),last.arg)]
            for i in reversed(b.ins):
                u,d=uses_defs(i.m)
                if i.m in COND:
                    t=(ctx_of(i.ctx,i.target()),i.target()); live|=target_live(t)
                i.live=live            # live AFTER this instruction (for branches: union with taken target)
                live=(live&~d)|u
                if i.m=='JSR' : live=live  # calls: callee flags (conservative: all live into callee if unknown)
            if live!=live_in[k]: live_in[k]=live; changed=True
    return live_in

# ----------------------------------------------------------------------------------------------------------------------------------------
CTX_ADJ=4          # word: adjustment for the access cycle of the next helper call
CTX_PTMP=6
INTERP_ONLY_MODES={'izx'}
class Gen:
    """68020 assembly for one superblock (GNU as, --register-prefix-optional).  Phase 1: every live flag is canonical after every instruction
    (N,Z in context byte nzc in P layout; C in d4 as 0x00/0xFF; V in context byte vflag bit 6), except inside a flag-setter + branch chain
    whose flags are dead afterwards: there the branches test the CCR / registers directly."""
    def __init__(self,prog,blocks,live_in,entries):
        self.prog=prog; self.blocks=blocks; self.live_in=live_in; self.entries=entries; self.out=[]; self.stubs=[]; self.lab=0; self.stats=collections.Counter()
    def e(self,s): self.cur.append('\t'+s)
    def newlab(self): self.lab+=1; return '.L%d'%self.lab
    def adj(self,idx): return self.cum+idx-self.tot
    # ------------------------------------------------------------------ block
    def gen_block(self,b):
        self.cur=[]; self.b=b; key=b.key; ins=b.ins; self.cum=0; self.fnz=None; self.fc=None; self.cconst=None
        self.tot=sum(i.cyc for i in ins)
        self.cur.append(label(key)+':')
        slow=self.newlab()
        if self.tot:
            self.e(f'sub.l #{self.tot},d5'); self.e(f'bmi {slow}')
            self.stubs.append((slow,[f'add.l #{self.tot},d5',f'move.w #0x{key[1]:X},d7','jmp rt_slow']))
        for idx,i in enumerate(ins):
            self.i=i; self.idx=idx; self.next=ins[idx+1] if idx+1<len(ins) else None
            self.gen_ins(i); self.cum+=i.cyc; self.stats[i.m]+=1
        last=ins[-1]
        if last.m not in TERM_JMP and last.m!='JSR': self.exit_fall(b)
        self.out.extend(self.cur)
        for (l,lines) in self.stubs: self.out.append(l+':'); self.out.extend('\t'+s for s in lines)
        self.stubs=[]
    def exit_fall(self,b):
        t=b.fall
        if t in self.entries: self.e(f'jmp {label(t)}')
        else: self.e(f'move.w #0x{t[1]:X},d7'); self.e('jmp rt_unk')

    # ------------------------------------------------------------------ chains / liveness helpers
    def chain(self):
        ch=[]; j=self.idx+1
        while j<len(self.b.ins) and self.b.ins[j].m in COND: ch.append(self.b.ins[j]); j+=1
        return ch
    def live_taken(self,br):
        t=(ctx_of(br.ctx,br.target()),br.target()); return self.live_in[t] if t in self.live_in else ALL
    def live_fall_after(self,k):
        """live set just before instruction k of the block (k may be len = block end)"""
        b=self.b
        if k>=len(b.ins):
            return self.live_in[b.fall] if (b.fall is not None and b.fall in self.live_in) else ALL
        i=b.ins[k]; u,d=uses_defs(i.m)
        if i.m in COND: return i.live|u        # i.live includes the taken side; conservative
        return (i.live&~d)|u
    def chain_ok(self,fl,allow='NZ'):
        """True when the flags fl are consumed only by the immediately following conditional branches and those branches can all be
        evaluated without clobbering the CCR (branches on flags outside `allow` emit a tst/btst of canonical storage, which would destroy it)"""
        ch=self.chain()
        if not ch: return False
        if any(COND[b.m][0] not in allow for b in ch): return False
        k=self.idx+1+len(ch)
        if self.live_fall_after(k)&fl: return False
        return all(not (self.live_taken(b)&fl) for b in ch)
    # ------------------------------------------------------------------ flag capture
    def cap_nz(self):
        self.e('move.w ccr,d7'); self.e('and.w #12,d7'); self.e(f'move.b (a5,d7.w),({CTX_NZC},a2)')
    def setnz(self,reg=None,ccr=False,fuse_ok=True):
        if not self.i.live&(N|Z): self.fnz=None; return
        if fuse_ok and self.chain_ok(N|Z): self.fnz=('ccr',) if ccr else ('reg',reg); return
        if not ccr: self.e(f'tst.b {reg}')
        self.cap_nz()
    def setc_scs(self):
        if self.i.live&C: self.e('scs d4')
    def setv(self):
        if self.i.live&V: self.e('svs d7'); self.e(f'move.b d7,({CTX_V},a2)')
    # ------------------------------------------------------------------ memory access
    def helper_rd(self,idx):
        self.e(f'move.w #{self.adj(idx)},({CTX_ADJ},a2)'); self.e('jsr rt_rd'); return 'd6'
    def helper_wr(self,idx):
        self.e(f'move.w #{self.adj(idx)},({CTX_ADJ},a2)'); self.e('jsr rt_wr')
    def penalty_idx(self,reg,lo):
        if lo==0: return
        l=self.newlab(); self.e(f'cmp.b #{256-lo},{reg}'); self.e(f'bcs.s {l}'); self.e('subq.l #1,d5'); self.cur.append(l+':')
    def ptr_load(self,zp):
        if zp!=0xFF: self.e(f'move.w ({zp},a0),d7'); self.e('ror.w #8,d7')
        else: self.e('moveq #0,d7'); self.e('move.b (0,a0),d7'); self.e('lsl.w #8,d7'); self.e('move.b (255,a0),d7')
    def zp_index(self,r,a):
        self.e(f'move.l {r},d7'); self.e(f'add.b #{a},d7'); return '(a0,d7.w)'
    def is_ram_operand(self,i):
        m=i.mode; a=i.arg
        if m in('zp','zpx','zpy'): return True
        if m=='abs': return region(a)=='ram'
        if m in('abx','aby'): return region(a)=='ram' and (a&0x7FF)+255<0x800
        return False
    def rd(self,i):
        m=i.mode; a=i.arg; bc=i.cyc; pen=i.m in PENALTY_READ
        if m=='imm': return f'#{a}'
        if m=='zp': return f'({a},a0)'
        if m=='zpx': return self.zp_index('d1',a)
        if m=='zpy': return self.zp_index('d2',a)
        if m=='abs':
            r=region(a)
            if r=='ram': return f'({a&0x7FF},a0)'
            if r=='win': return f'({a-0x8000},a1)'
            if r=='fix': return f'({a-0x8000},a6)'
            self.e(f'move.w #0x{a:X},d7'); return self.helper_rd(bc)
        if m in('abx','aby'):
            ir='d1' if m=='abx' else 'd2'; r=region(a)
            if pen: self.penalty_idx(ir,a&0xFF)
            if r=='ram' and (a&0x7FF)+255<0x800: return f'({a&0x7FF},a0,{ir}.w)'
            if r=='win' and a+255<=0xBFFF: return f'({a-0x8000},a1,{ir}.w)'
            if r=='fix' and a+255<=0xFFFF: return f'({a-0x8000},a6,{ir}.w)'
            self.e(f'move.w #0x{a:X},d7'); self.e(f'add.w {ir},d7'); return self.helper_rd(bc)
        if m=='izy':
            self.ptr_load(a)
            if pen:
                l=self.newlab(); self.e('move.b d7,d6'); self.e('add.b d2,d6'); self.e(f'bcc.s {l}'); self.e('subq.l #1,d5'); self.cur.append(l+':')
            self.e('add.w d2,d7'); return self.helper_rd(bc)
        raise NotImplementedError(m)
    def wr(self,i,reg):
        """store register reg (d0/d1/d2) to the instruction's operand"""
        m=i.mode; a=i.arg; bc=i.cyc
        if m in('zp','zpx','zpy','abs','abx','aby') and self.is_ram_operand(i):
            if m=='zp': o=f'({a},a0)'
            elif m=='zpx': o=self.zp_index('d1',a)
            elif m=='zpy': o=self.zp_index('d2',a)
            elif m=='abs': o=f'({a&0x7FF},a0)'
            else: o=f'({a&0x7FF},a0,{"d1" if m=="abx" else "d2"}.w)'
            self.e(f'move.b {reg},{o}'); return
        if m=='abs': self.e(f'move.w #0x{a:X},d7')
        elif m in('abx','aby'): self.e(f'move.w #0x{a:X},d7'); self.e(f'add.w {"d1" if m=="abx" else "d2"},d7')
        elif m=='izy': self.ptr_load(a); self.e('add.w d2,d7')
        else: raise NotImplementedError(m)
        self.e(f'move.b {reg},d6'); self.helper_wr(bc)
    # ------------------------------------------------------------------ stack / control
    def push_byte(self,src): self.e(f'move.b {src},(0x100,a0,d3.w)'); self.e('subq.b #1,d3')
    def pull_byte(self,dst): self.e('addq.b #1,d3'); self.e(f'move.b (0x100,a0,d3.w),{dst}')
    def pack_p(self,dst='d6',brk=True):
        """dst.b = P register built from the canonical flags (bit4/5 as pushed by PHP)"""
        self.e(f'move.b ({CTX_NZC},a2),{dst}'); self.e(f'and.b #0x82,{dst}')
        self.e('move.b d4,d7'); self.e('and.b #1,d7'); self.e(f'or.b d7,{dst}')
        self.e(f'move.b ({CTX_V},a2),d7'); self.e('and.b #0x40,d7'); self.e(f'or.b d7,{dst}')
        self.e(f'move.b ({CTX_MISC},a2),d7'); self.e('and.b #0x0C,d7'); self.e(f'or.b d7,{dst}')
        self.e(f'or.b #{0x30 if brk else 0x20},{dst}')
    def unpack_p(self,src='d6'):
        self.e(f'move.b {src},d7'); self.e('and.b #0x82,d7'); self.e(f'move.b d7,({CTX_NZC},a2)')
        self.e(f'move.b {src},({CTX_V},a2)')
        self.e(f'move.b {src},d7'); self.e('and.b #0x0C,d7'); self.e(f'move.b d7,({CTX_MISC},a2)')
        self.e(f'btst #0,{src}'); self.e('sne d4')
    def goto(self,key,addr_known=True):
        if key in self.entries: self.e(f'jmp {label(key)}')
        else: self.e(f'move.w #0x{key[1]:X},d7'); self.e('jmp rt_unk')
    def jump_to_addr(self,i,addr):
        """static jump/call target in the current context"""
        if addr>=0xC000: return self.goto((7,addr))
        if i.ctx==7:      # fixed code to window address: bank decided at run time
            self.e(f'jmp ([{addr*4},a3])')
        else: self.goto((i.ctx,addr))

    def branch_cond(self,i):
        """emits the test for conditional branch i; returns the 68k condition that is TRUE when the 6502 branch is taken"""
        f,want=COND[i.m]; fz=self.fnz
        if f=='Z':
            if fz and fz[0]=='ccr': return 'eq' if want else 'ne'
            if fz and fz[0]=='reg': self.e(f'tst.b {fz[1]}'); return 'eq' if want else 'ne'
            if fz and fz[0]=='bit': self.e('tst.b d7'); return 'eq' if want else 'ne'
            self.e(f'btst #1,({CTX_NZC},a2)'); return 'ne' if want else 'eq'
        if f=='N':
            if fz and fz[0]=='ccr': return 'mi' if want else 'pl'
            if fz and fz[0]=='reg': self.e(f'tst.b {fz[1]}'); return 'mi' if want else 'pl'
            if fz and fz[0]=='bit': self.e('tst.b d6'); return 'mi' if want else 'pl'
            self.e(f'tst.b ({CTX_NZC},a2)'); return 'mi' if want else 'pl'
        if f=='C':
            if self.fc=='cmpc': return 'cc' if want else 'cs'      # 6502 C = no borrow
            self.e('tst.b d4'); return 'ne' if want else 'eq'
        if f=='V':
            if fz and fz[0]=='bit': self.e('btst #6,d6'); return 'ne' if want else 'eq'
            self.e(f'btst #6,({CTX_V},a2)'); return 'ne' if want else 'eq'

    # ------------------------------------------------------------------ instructions
    def gen_ins(self,i):
        m=i.m; mode=i.mode
        if m not in COND:
            # a fused flag state only lives until the first non-branch instruction after the setter
            self.fnz=None; self.fc=None
        getattr(self,'i_'+m)(i)
        if m not in('CLC','SEC') and (uses_defs(m)[1]&C): self.cconst=None
    # --- loads / stores
    def i_LDA(self,i): o=self.rd(i); self.e(f'move.b {o},d0'); self.setnz('d0',ccr=True)
    def i_LDX(self,i): o=self.rd(i); self.e(f'move.b {o},d1'); self.setnz('d1',ccr=True)
    def i_LDY(self,i): o=self.rd(i); self.e(f'move.b {o},d2'); self.setnz('d2',ccr=True)
    def i_STA(self,i): self.wr(i,'d0')
    def i_STX(self,i): self.wr(i,'d1')
    def i_STY(self,i): self.wr(i,'d2')
    # --- logic
    def i_AND(self,i): o=self.rd(i); self.e(f'and.b {o},d0'); self.setnz('d0',ccr=True)
    def i_ORA(self,i): o=self.rd(i); self.e(f'or.b {o},d0'); self.setnz('d0',ccr=True)
    def i_EOR(self,i):
        o=self.rd(i)
        if o.startswith('#'): self.e(f'eor.b {o},d0')
        else: self.e(f'move.b {o},d6'); self.e('eor.b d6,d0')
        self.setnz('d0',ccr=True)
    def i_BIT(self,i):
        o=self.rd(i); self.e(f'move.b {o},d6'); self.e('move.b d0,d7'); self.e('and.b d6,d7')
        if i.live&V: self.e(f'move.b d6,({CTX_V},a2)')
        if not i.live&(N|Z): return
        if self.chain_ok(N|Z|V) or (self.chain_ok(N|Z) and not i.live&V): self.fnz=('bit',); return
        self.e(f'move.b d6,({CTX_NZC},a2)'); self.e(f'and.b #0x80,({CTX_NZC},a2)'); self.e('tst.b d7'); l=self.newlab(); self.e(f'bne.s {l}'); self.e(f'or.b #2,({CTX_NZC},a2)'); self.cur.append(l+':')
    # --- arithmetic
    def carry_to_x(self): self.e('add.b d4,d4')      # X = C; d4 is dead afterwards unless this instruction re-materialises C
    def i_ADC(self,i):
        o=self.rd(i)
        if self.cconst==0: self.e(f'add.b {o},d0')
        else:
            if o!='d6': self.e(f'move.b {o},d6')
            self.carry_to_x(); self.e('addx.b d6,d0')
        self.setc_scs(); self.setv(); self.setnz('d0',ccr=False)
    def i_SBC(self,i):
        o=self.rd(i)
        if self.cconst==1:
            self.e(f'sub.b {o},d0'); 
            if i.live&C: self.e('scc d4')
        else:
            if o!='d6': self.e(f'move.b {o},d6')
            self.e('not.b d6'); self.carry_to_x(); self.e('addx.b d6,d0'); self.setc_scs()
        self.setv(); self.setnz('d0',ccr=False)
    def _cmp(self,i,r):
        o=self.rd(i); self.e(f'cmp.b {o},{r}')
        usec=bool(i.live&C); livenz=bool(i.live&(N|Z))
        fuse_c=usec and self.chain_ok(C,'NZC') and (not livenz or self.chain_ok(N|Z,'NZC'))
        if usec and not fuse_c: self.e('scc d4')
        if livenz:
            if self.chain_ok(N|Z,'NZC' if fuse_c else 'NZ'): self.fnz=('ccr',)
            else: self.cap_nz()
        if fuse_c: self.fc='cmpc'
    def i_CMP(self,i): self._cmp(i,'d0')
    def i_CPX(self,i): self._cmp(i,'d1')
    def i_CPY(self,i): self._cmp(i,'d2')
    # --- read-modify-write
    def _rmw_mem(self,i,body,carry_in=False):
        if not self.is_ram_operand(i): raise NotImplementedError('rmw io')
        m=i.mode; a=i.arg
        if m=='zp': o=f'({a},a0)'
        elif m=='zpx': o=self.zp_index('d1',a)
        elif m=='zpy': o=self.zp_index('d2',a)
        elif m=='abs': o=f'({a&0x7FF},a0)'
        else: o=f'({a&0x7FF},a0,{"d1" if m=="abx" else "d2"}.w)'
        return o
    def i_INC(self,i): o=self._rmw_mem(i,None); self.e(f'addq.b #1,{o}'); self.setnz(ccr=True)
    def i_DEC(self,i): o=self._rmw_mem(i,None); self.e(f'subq.b #1,{o}'); self.setnz(ccr=True)
    def _shift(self,i,which):
        acc=(i.mode=='acc'); r='d0' if acc else 'd6'
        if not acc:
            o=self._rmw_mem(i,None); self.e(f'move.b {o},d6')
        if which=='ASL': self.e(f'add.b {r},{r}')
        elif which=='LSR': self.e(f'lsr.b #1,{r}')
        elif which=='ROL':
            if self.cconst==0: self.e(f'add.b {r},{r}')
            else: self.carry_to_x(); self.e(f'roxl.b #1,{r}')
        else:
            if self.cconst==0: self.e(f'lsr.b #1,{r}')
            else: self.carry_to_x(); self.e(f'roxr.b #1,{r}')
        self.setc_scs()
        if not acc: self.e(f'move.b d6,{o}')
        self.setnz(ccr=True) if not acc else self.setnz(ccr=True)
    def i_ASL(self,i): self._shift(i,'ASL')
    def i_LSR(self,i): self._shift(i,'LSR')
    def i_ROL(self,i): self._shift(i,'ROL')
    def i_ROR(self,i): self._shift(i,'ROR')
    # --- register ops
    def i_INX(self,i): self.e('addq.b #1,d1'); self.setnz(ccr=True)
    def i_INY(self,i): self.e('addq.b #1,d2'); self.setnz(ccr=True)
    def i_DEX(self,i): self.e('subq.b #1,d1'); self.setnz(ccr=True)
    def i_DEY(self,i): self.e('subq.b #1,d2'); self.setnz(ccr=True)
    def i_TAX(self,i): self.e('move.b d0,d1'); self.setnz(ccr=True)
    def i_TAY(self,i): self.e('move.b d0,d2'); self.setnz(ccr=True)
    def i_TXA(self,i): self.e('move.b d1,d0'); self.setnz(ccr=True)
    def i_TYA(self,i): self.e('move.b d2,d0'); self.setnz(ccr=True)
    def i_TSX(self,i): self.e('move.b d3,d1'); self.setnz(ccr=True)
    def i_TXS(self,i): self.e('move.b d1,d3')
    def i_NOP(self,i): pass
    def i_CLC(self,i): self.e('moveq #0,d4') if i.live&C else None; self.cconst=0
    def i_SEC(self,i): self.e('moveq #-1,d4') if i.live&C else None; self.cconst=1
    def i_CLV(self,i): self.e(f'clr.b ({CTX_V},a2)')
    def i_SEI(self,i): self.e(f'or.b #4,({CTX_MISC},a2)')
    def i_CLI(self,i): self.e(f'and.b #0xFB,({CTX_MISC},a2)')
    def i_SED(self,i): self.e(f'or.b #8,({CTX_MISC},a2)')
    def i_CLD(self,i): self.e(f'and.b #0xF7,({CTX_MISC},a2)')
    # --- stack
    def i_PHA(self,i): self.push_byte('d0')
    def i_PLA(self,i): self.pull_byte('d0'); self.setnz('d0',ccr=True)
    def i_PHP(self,i): self.pack_p('d6',True); self.push_byte('d6')
    def i_PLP(self,i): self.pull_byte('d6'); self.unpack_p('d6')
    # --- control flow
    def i_JMP(self,i):
        if i.mode=='abs': self.jump_to_addr(i,i.arg); return
        a=i.arg
        if region(a)!='ram': raise NotImplementedError('jmp ind non-ram')
        self.e(f'move.w ({a&0x7FF},a0),d7'); self.e('ror.w #8,d7'); self.e('jmp rt_disp')
    def i_JSR(self,i):
        ret=(i.addr+2)&0xFFFF
        self.e(f'move.b #{ret>>8},(0x100,a0,d3.w)'); self.e('subq.b #1,d3'); self.e(f'move.b #{ret&0xFF},(0x100,a0,d3.w)'); self.e('subq.b #1,d3')
        self.jump_to_addr(i,i.arg)
    def i_RTS(self,i):
        self.e('moveq #0,d7'); self.e('moveq #0,d6'); self.pull_byte('d7'); self.pull_byte('d6'); self.e('lsl.w #8,d6'); self.e('or.w d6,d7'); self.e('addq.w #1,d7'); self.e('jmp rt_disp')
    def i_RTI(self,i):
        self.pull_byte('d6'); self.unpack_p('d6')
        self.e('moveq #0,d7'); self.e('moveq #0,d6'); self.pull_byte('d7'); self.pull_byte('d6'); self.e('lsl.w #8,d6'); self.e('or.w d6,d7'); self.e('jmp rt_disp')
    def i_BRK(self,i): raise NotImplementedError('brk')
    def _branch(self,i):
        cond=self.branch_cond(i); t=i.target(); tk=(ctx_of(i.ctx,t),t)
        cross=((i.nxt&0xFF00)!=(t&0xFF00)); extra=2 if cross else 1
        rest=self.tot-(self.cum+i.cyc)        # cycles of the instructions after the branch that were pre-charged
        k=rest-extra
        lines=[]
        if k>0: lines.append(f'add.l #{k},d5')
        elif k<0: lines.append(f'sub.l #{-k},d5')
        if tk in self.entries: lines.append(f'jmp {label(tk)}')
        else: lines.append(f'move.w #0x{t:X},d7'); lines.append('jmp rt_unk')
        l=self.newlab(); self.stubs.append((l,lines)); self.e(f'b{cond} {l}')
    i_BPL=i_BMI=i_BNE=i_BEQ=i_BCC=i_BCS=i_BVC=i_BVS=_branch

# ----------------------------------------------------------------------------------------------------------------------------------------
def translate(prg,seen,out_s,out_tab,report=None,extra_entries=()):
    rom=Rom(prg); prog=Prog(rom,seen); blocks=build_blocks(prog)
    # a leader whose first instruction needs the interpreter has no block; entries = translated blocks (+ externally defined labels)
    entries=set(blocks)|set(extra_entries)
    live_in=liveness(prog,blocks)
    g=Gen(prog,blocks,live_in,entries)
    with open(out_s,'w') as f:
        f.write('\t.text\n\t.even\n')
        for key in sorted(blocks): f.write(f'\t.globl {label(key)}\n')
        for key in sorted(blocks):
            g.out=[]; g.gen_block(blocks[key]); f.write('\n'.join(g.out)+'\n')
    # dispatch tables: window per bank (16384 entries) + fixed (16384)
    with open(out_tab,'w') as f:
        f.write('\t.data\n\t.even\n')
        for bank in range(7):
            f.write(f'\t.globl tab_win{bank}\ntab_win{bank}:\n'); emit_table(f,entries,bank,0x8000)
        f.write('\t.globl tab_fix\ntab_fix:\n'); emit_table(f,entries,7,0xC000)
    if report:
        with open(report,'w') as f:
            f.write(f'blocks {len(blocks)} instructions {sum(len(b.ins) for b in blocks.values())} translated\n')
            f.write(f'mnemonics: {dict(g.stats)}\n')
    return blocks
def emit_table(f,entries,ctx,base):
    run=0
    for a in range(base,base+16384):
        if (ctx,a) in entries:
            if run: f.write(f'\t.rept {run}\n\t.long rt_unk_ent\n\t.endr\n'); run=0
            f.write(f'\t.long {label((ctx,a))}\n')
        else: run+=1
    if run: f.write(f'\t.rept {run}\n\t.long rt_unk_ent\n\t.endr\n')

if __name__=='__main__':
    nes=open(sys.argv[1],'rb').read(); prg=nes[16:16+131072]
    seen=[(o>>14,(0xC000 if o>>14==7 else 0x8000)+(o&0x3FFF)) for o in map(int,open(sys.argv[2]).read().split())]
    blocks=translate(prg,seen,sys.argv[3],sys.argv[4],sys.argv[5] if len(sys.argv)>5 else None)
    print(len(blocks),'blocks')
