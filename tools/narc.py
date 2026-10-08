import struct,os,sys
def unpack(path,outdir):
    d=open(path,'rb').read()
    p=16; secs={}
    while p<len(d):
        mg=d[p:p+4]; sz=struct.unpack_from('<I',d,p+4)[0]; secs[mg]=(p,sz); p+=sz
    bp,_=secs[b'BTAF']; n=struct.unpack_from('<H',d,bp+8)[0]
    fat=[struct.unpack_from('<II',d,bp+12+i*8) for i in range(n)]
    tp,_=secs[b'BTNF']; gp,_=secs[b'GMIF']; img=gp+8; fnt=tp+8
    names=[]
    def walk(did,path):
        off,first,_=struct.unpack_from('<IHH',d,fnt+(did&0xfff)*8)
        q=fnt+off; fid=first
        while True:
            l=d[q]; q+=1
            if l==0: break
            nm=d[q:q+(l&0x7f)].decode(); q+=l&0x7f
            if l&0x80: sub=struct.unpack_from('<H',d,q)[0]; q+=2; walk(sub,path+nm+'/')
            else: names.append((fid,path+nm)); fid+=1
    if secs[b'BTNF'][1]>16: walk(0xF000,'')
    else: names=[(i,'%04d.bin'%i) for i in range(n)]
    for fid,nm in names:
        s,e=fat[fid]; o=os.path.join(outdir,nm); os.makedirs(os.path.dirname(o),exist_ok=True)
        open(o,'wb').write(d[img+s:img+e])
    return names
if __name__=='__main__':
    for nm in unpack(sys.argv[1],sys.argv[2]): print(nm)
