"""SDAT (Nitro Sound Data) reader: SSEQ, SSAR, SBNK, SWAR, SWAV."""
import struct

REC = ['SEQ', 'SEQARC', 'BANK', 'WAVEARC', 'PLAYER', 'GROUP', 'PLAYER2', 'STRM']


class SDAT:
    def __init__(self, d):
        self.d = d
        symb, _, info, _, fat, _, _, _ = struct.unpack_from('<8I', d, 16)
        nfat = struct.unpack_from('<I', d, fat + 8)[0]
        self.fat = [struct.unpack_from('<II', d, fat + 12 + i * 16) for i in range(nfat)]
        self.names = {r: [] for r in REC}
        self.seqarc_names = []
        if symb:
            for i, r in enumerate(REC):
                ro = symb + struct.unpack_from('<I', d, symb + 8 + i * 4)[0]
                cnt = struct.unpack_from('<I', d, ro)[0]
                for j in range(cnt):
                    if r == 'SEQARC':
                        no, sub = struct.unpack_from('<II', d, ro + 4 + j * 8)
                        self.names[r].append(self._str(symb + no) if no else None)
                        subs = []
                        if sub:
                            sc = struct.unpack_from('<I', d, symb + sub)[0]
                            for k in range(sc):
                                so = struct.unpack_from('<I', d, symb + sub + 4 + k * 4)[0]
                                subs.append(self._str(symb + so) if so else None)
                        self.seqarc_names.append(subs)
                    else:
                        no = struct.unpack_from('<I', d, ro + 4 + j * 4)[0]
                        self.names[r].append(self._str(symb + no) if no else None)
        self.info = {}
        for i, r in enumerate(REC):
            ro = info + struct.unpack_from('<I', d, info + 8 + i * 4)[0]
            cnt = struct.unpack_from('<I', d, ro)[0]
            ents = []
            for j in range(cnt):
                eo = struct.unpack_from('<I', d, ro + 4 + j * 4)[0]
                ents.append(info + eo if eo else None)
            self.info[r] = ents

    def _str(self, o):
        return self.d[o:self.d.index(b'\0', o)].decode()

    def file(self, fid):
        o, s = self.fat[fid]
        return self.d[o:o + s]

    def seq(self, i):
        o = self.info['SEQ'][i]
        fid, _, bank, vol, cpr, ppr, ply = struct.unpack_from('<HHHBBBB', self.d, o)
        return dict(file=self.file(fid), bank=bank, vol=vol, player=ply)

    def seqarc(self, i):
        o = self.info['SEQARC'][i]
        fid = struct.unpack_from('<H', self.d, o)[0]
        return self.file(fid)

    def bank(self, i):
        o = self.info['BANK'][i]
        fid, _, w0, w1, w2, w3 = struct.unpack_from('<HHhhhh', self.d, o)
        return dict(file=self.file(fid), waves=[w for w in (w0, w1, w2, w3)])

    def wavearc(self, i):
        o = self.info['WAVEARC'][i]
        fid = struct.unpack_from('<H', self.d, o)[0]
        return self.file(fid)


def ssar_entries(d):
    """SSAR: returns (data_base, [(offset, bank, vol, cpr, ppr, ply)])."""
    data_off = struct.unpack_from('<I', d, 0x18)[0]
    n = struct.unpack_from('<I', d, 0x1C)[0]
    out = []
    for i in range(n):
        off, bank, vol, cpr, ppr, ply = struct.unpack_from('<IHBBBB', d, 0x20 + i * 12)
        out.append((off, bank, vol, cpr, ppr, ply))
    return data_off, out


def swar_waves(d):
    n = struct.unpack_from('<I', d, 0x38)[0]
    offs = [struct.unpack_from('<I', d, 0x3C + i * 4)[0] for i in range(n)]
    return [d[o:] for o in offs]


def swav_info(w):
    """w: bytes starting at the SWAV header inside the SWAR."""
    fmt, loop, rate, timer, loopofs, nonloop = struct.unpack_from('<BBHHHI', w, 0)
    size = (loopofs + nonloop) * 4
    return dict(fmt=fmt, loop=loop, rate=rate, timer=timer, loopstart=loopofs * 4,
                looplen=nonloop * 4, data=w[12:12 + size])
