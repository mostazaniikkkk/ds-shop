import hashlib, sys
from Crypto.Cipher import AES
M=(1<<128)-1
def rol(x,n): return ((x<<n)|(x>>(128-n)))&M
class Nand:
    def __init__(s,path):
        s.f=open(path,'rb'); s.f.seek(-64,2); ft=s.f.read(64)
        if s.f.seek(0,2)<0xF000000+64:
            raise ValueError('%s is too small to be a DSi NAND (240 MB or more): the dump is incomplete'%path)
        if ft[:16]!=b'DSi eMMC CID/CPU':
            raise ValueError('%s does not end with the "DSi eMMC CID/CPU" footer: dump the NAND with dumpTool or GodMode9i (see README, step 0)'%path)
        s.cid=ft[16:32]; cpu=int.from_bytes(ft[32:40],'little'); s.cpu=cpu
        lo,hi=cpu&0xffffffff,cpu>>32
        X=lo|((lo^0x24EE6906)<<32)|((hi^0xE65B601D)<<64)|(hi<<96)
        Y=0x0AB9DC76|(0xBD4DC4D3<<32)|(0x202DDD1D<<64)|(0xE1A00005<<96)
        k=rol(((X^Y)+0xFFFEFB4E295902582A680F5F1A4F3E79)&M,42)
        s.aes=AES.new(k.to_bytes(16,'big'),AES.MODE_ECB)
        s.ctr=int.from_bytes(hashlib.sha1(s.cid).digest()[:16],'little')
    def read(s,off,n):
        a=off&~15; e=(off+n+15)&~15
        s.f.seek(a); d=s.f.read(e-a)
        ks=b''.join(s.aes.encrypt(((s.ctr+(a>>4)+i)&M).to_bytes(16,'big'))[::-1] for i in range((e-a)//16))
        out=bytes(x^y for x,y in zip(d,ks))
        return out[off-a:off-a+n]
