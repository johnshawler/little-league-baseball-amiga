#!/usr/bin/env python3
"""fbcmp.py mesen.vid mine.fb : compare frame-by-frame.  Mesen frames are RGB (header line 'fr w h pitch fmt' + data); mine are 16-bit NES colour values.
Learns the NES-colour->RGB map from the data and reports conflicts (= pixels where the two emulators differ)."""
import sys,numpy as np
vid=open(sys.argv[1],'rb'); mine=open(sys.argv[2],'rb')
W,H=256,240
mapping={}   # value -> rgb
bad_frames=[]; nconf=0; n=0; first=None
while True:
    h=vid.readline()
    if not h: break
    fr,w,hh,p,fmt=map(int,h.split()); bpp=4 if fmt==1 else 2
    d=vid.read(w*hh*bpp)
    hdr=mine.read(8)
    if len(hdr)<8: break
    mf=int.from_bytes(hdr,'little'); m=np.frombuffer(mine.read(W*H*2),dtype='<u2')
    assert mf==fr,(mf,fr)
    a=np.frombuffer(d,dtype=np.uint8).reshape(-1,4)[:,:3] if bpp==4 else None
    rgb=(a[:,0].astype(np.uint32)<<16)|(a[:,1].astype(np.uint32)<<8)|a[:,2]
    # learn / check mapping
    keys=np.unique(m)
    frame_bad=0
    for k in keys.tolist():
        vals=np.unique(rgb[m==k])
        if k not in mapping: mapping[k]=int(vals[0]) if len(vals)==1 else None
        if len(vals)!=1 or (mapping[k] is not None and int(vals[0])!=mapping[k]):
            frame_bad+=int((m==k).sum()) if len(vals)!=1 else int((m==k).sum())
    if frame_bad:
        bad_frames.append(fr)
        if first is None: first=fr
    n+=1
print('frames compared',n,'frames with differences',len(bad_frames),'first',first, 'colours seen',len(mapping))
print('first bad frames',bad_frames[:20])
