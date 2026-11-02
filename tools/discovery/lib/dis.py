import sys; sys.path.insert(0,'/tmp/claude-0/-home-user/155dae35-0d01-57d8-97ff-58d0b7d66e38/scratchpad/lib')
import d6502
rom=open(sys.argv[1],'rb').read()[16:16+131072]
def dis(bank,addr,n):
    base=0xC000 if bank==7 else 0x8000
    o=bank*16384+addr-base; out=[]
    for _ in range(n):
        d=d6502.decode(rom,o)
        if not d: out.append('%04x  .db %02x'%(addr,rom[o])); o+=1; addr+=1; continue
        op,m,mode,l,b=d
        arg=''
        if mode=='imm': arg='#$%02x'%b[0]
        elif mode in('zp','zpx','zpy','izx','izy'): arg='$%02x'%b[0]+{'zp':'','zpx':',X','zpy':',Y','izx':',X)','izy':'),Y'}[mode]; arg=('('+arg) if mode.startswith('iz') else arg
        elif mode in('abs','abx','aby','ind'): v=b[0]|b[1]<<8; arg='$%04x'%v+{'abs':'','abx':',X','aby':',Y','ind':''}[mode]; arg='($%04x)'%v if mode=='ind' else arg
        elif mode=='rel': t=addr+2+(b[0]-256 if b[0]>127 else b[0]); arg='$%04x'%t
        out.append('%04x  %-02x %-12s %s %s'%(addr,op,' '.join('%02x'%x for x in b),m,arg)); o+=l; addr+=l
    return out
if __name__=='__main__':
    for spec in sys.argv[2:]:
        b,a,n=spec.split(',') ; print('--- bank',b,hex(int(a,16)))
        print('\n'.join(dis(int(b),int(a,16),int(n))))
