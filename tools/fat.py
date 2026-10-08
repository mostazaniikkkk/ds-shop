import struct
class Fat:
    def __init__(s,nand,base):
        s.n=nand; s.b=base; bs=nand.read(base,512)
        if bs[510:512]!=b'\x55\xaa':
            raise ValueError('the NAND does not decrypt correctly: the dump is incomplete or the CID/CPU footer belongs to another console')
        s.bps,s.spc,s.rsv,s.nf,s.nroot,_,_,s.spf=struct.unpack_from('<HBHBHHBH',bs,11)
        s.cs=s.bps*s.spc
        s.fat=nand.read(base+s.rsv*s.bps,s.spf*s.bps)
        s.root=base+(s.rsv+s.nf*s.spf)*s.bps
        s.data=s.root+s.nroot*32
    def chain(s,c):
        while 2<=c<0xFFF8:
            yield c; c=struct.unpack_from('<H',s.fat,c*2)[0]
    def readclusters(s,c):
        return b''.join(s.n.read(s.data+(x-2)*s.cs,s.cs) for x in s.chain(c))
    def entries(s,raw):
        for i in range(0,len(raw),32):
            e=raw[i:i+32]
            if e[0]==0: break
            if e[0]==0xE5 or e[11]==0x0F or e[11]&8: continue
            nm=e[:8].decode('latin1').rstrip(); ex=e[8:11].decode('latin1').rstrip()
            name=nm+('.'+ex if ex else '')
            if name in('.','..'): continue
            yield name, bool(e[11]&0x10), struct.unpack_from('<H',e,26)[0], struct.unpack_from('<I',e,28)[0]
    def ls(s,path=''):
        raw=s.n.read(s.root,s.nroot*32)
        for p in [x for x in path.split('/') if x]:
            for nm,d,c,sz in s.entries(raw):
                if nm.lower()==p.lower(): raw=s.readclusters(c); break
            else: raise FileNotFoundError(path)
        return list(s.entries(raw))
    def get(s,path):
        d,_,f=path.rpartition('/')
        for nm,isd,c,sz in s.ls(d):
            if nm.lower()==f.lower(): return s.readclusters(c)[:sz]
        raise FileNotFoundError(path)
    def walk(s,path=''):
        for nm,d,c,sz in s.ls(path):
            p=path+'/'+nm
            yield p,d,sz
            if d: yield from s.walk(p)
