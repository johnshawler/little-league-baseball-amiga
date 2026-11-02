import sys,zlib,struct,numpy as np
def frames(path, idxs):
    f=open(path,'rb'); out={}
    i=0
    while True:
        h=f.readline()
        if not h: break
        fr,w,hh,p,fmt=map(int,h.split()); bpp=4 if fmt==1 else 2
        data=f.read(w*hh*bpp)
        if fr in idxs:
            a=np.frombuffer(data,dtype=np.uint8 if bpp==4 else '<u2')
            if bpp==4: a=a.reshape(hh,w,4)[:,:,[2,1,0]]
            else:
                a=a.reshape(hh,w); a=np.stack([((a>>10)&31)<<3,((a>>5)&31)<<3,(a&31)<<3],-1).astype(np.uint8)
            out[fr]=a
    return out
def png(path,a):
    h,w,_=a.shape; raw=b''.join(b'\0'+a[y].tobytes() for y in range(h))
    def ch(t,d): c=struct.pack('>I',len(d))+t+d; return c+struct.pack('>I',zlib.crc32(t+d))
    open(path,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))
if __name__=='__main__':
    idx=set(map(int,sys.argv[3:])); fs=frames(sys.argv[1],idx)
    # montage
    ims=[fs[i] for i in sorted(fs)]
    cols=3; rows=(len(ims)+cols-1)//cols
    H,W=ims[0].shape[:2]; m=np.zeros((rows*H,cols*W,3),np.uint8)
    for k,im in enumerate(ims): m[(k//cols)*H:(k//cols+1)*H,(k%cols)*W:(k%cols+1)*W]=im
    png(sys.argv[2],m)
