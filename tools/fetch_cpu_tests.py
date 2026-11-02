#!/usr/bin/env python3
"""Download SingleStepTests/65x02 nes6502 vectors for the documented opcodes (first N per opcode) into tests/cpu/<op>.txt (compact text)."""
import sys,json,urllib.request,os
sys.path.insert(0,os.path.join(os.path.dirname(__file__),'discovery','lib'))
import d6502
N=int(sys.argv[1]) if len(sys.argv)>1 else 400
out=os.path.join(os.path.dirname(__file__),'..','tests','cpu'); os.makedirs(out,exist_ok=True)
for op in sorted(d6502.T):
    fn=os.path.join(out,'%02x.txt'%op)
    if os.path.exists(fn): continue
    u='https://raw.githubusercontent.com/SingleStepTests/65x02/main/nes6502/v1/%02x.json'%op
    d=json.load(urllib.request.urlopen(u,timeout=120))[:N]
    with open(fn,'w') as f:
        for t in d:
            i,e=t['initial'],t['final']
            f.write('T %s\n'%t['name'])
            f.write('I %d %d %d %d %d %d\n'%(i['pc'],i['s'],i['a'],i['x'],i['y'],i['p']))
            f.write('E %d %d %d %d %d %d\n'%(e['pc'],e['s'],e['a'],e['x'],e['y'],e['p']))
            f.write('R %d %s\n'%(len(i['ram']),' '.join('%d %d'%(a,v) for a,v in i['ram'])))
            f.write('F %d %s\n'%(len(e['ram']),' '.join('%d %d'%(a,v) for a,v in e['ram'])))
            f.write('C %d %s\n'%(len(t['cycles']),' '.join('%d %d %s'%(a,v,k[0]) for a,v,k in t['cycles'])))
    print(op,end=' ',flush=True)
