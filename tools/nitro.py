"""Minimal readers for Nitro formats (NCLR/NCGR/NSCR/NCER), Yaz0 and BMG."""
import struct
from PIL import Image


def yaz0(b):
    n = struct.unpack_from('>I', b, 4)[0]
    o = bytearray()
    p = 16
    while len(o) < n:
        c = b[p]; p += 1
        for i in range(8):
            if len(o) >= n:
                break
            if c & (0x80 >> i):
                o.append(b[p]); p += 1
            else:
                b1, b2 = b[p], b[p + 1]; p += 2
                dist = ((b1 & 15) << 8 | b2) + 1
                ln = b1 >> 4
                if ln == 0:
                    ln = b[p] + 0x12; p += 1
                else:
                    ln += 2
                for _ in range(ln):
                    o.append(o[-dist])
    return bytes(o)


def sections(d):
    """Returns {magic: section_offset} for a generic Nitro file."""
    hsize = struct.unpack_from('<H', d, 12)[0]
    nsec = struct.unpack_from('<H', d, 14)[0]
    p = hsize
    out = {}
    for _ in range(nsec):
        if p + 8 > len(d):
            break
        out[d[p:p + 4]] = p
        p += struct.unpack_from('<I', d, p + 4)[0]
    return out


def bgr555(v):
    r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
    return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2)


class NCLR:
    def __init__(self, d):
        s = sections(d)[b'TTLP']
        self.depth = struct.unpack_from('<I', d, s + 8)[0]  # 3=4bpp 4=8bpp
        size, off = struct.unpack_from('<II', d, s + 0x10)
        raw = d[s + 8 + off: s + 8 + off + size]
        self.colors = [bgr555(struct.unpack_from('<H', raw, i)[0]) for i in range(0, len(raw) - 1, 2)]
        self.raw = raw


class NCGR:
    def __init__(self, d):
        s = sections(d)[b'RAHC']
        self.h, self.w = struct.unpack_from('<hh', d, s + 8)
        self.depth = struct.unpack_from('<I', d, s + 0xC)[0]
        self.mapping = struct.unpack_from('<I', d, s + 0x10)[0]
        self.linear = struct.unpack_from('<I', d, s + 0x14)[0] & 1
        size, off = struct.unpack_from('<II', d, s + 0x18)
        self.data = d[s + 8 + off: s + 8 + off + size]
        self.bpp = 4 if self.depth == 3 else 8
        self.tilesize = 8 * self.bpp
        self.ntiles = len(self.data) // self.tilesize

    def tile(self, i):
        """64 color indices of tile i (0 if it is out of range)."""
        t = self.data[i * self.tilesize:(i + 1) * self.tilesize]
        if len(t) < self.tilesize:
            return [0] * 64
        if self.bpp == 8:
            return list(t)
        out = []
        for b in t:
            out += [b & 15, b >> 4]
        return out


def tile_into(img, px, x0, y0, colors, palbase, hflip=False, vflip=False, transparent0=True):
    for y in range(8):
        for x in range(8):
            c = px[y * 8 + x]
            if c == 0 and transparent0:
                continue
            xx = 7 - x if hflip else x
            yy = 7 - y if vflip else y
            idx = palbase + c
            col = colors[idx] if idx < len(colors) else (255, 0, 255)
            img.putpixel((x0 + xx, y0 + yy), col + (255,))


def sheet(ncgr, nclr, cols=16, pal=0):
    n = max(ncgr.ntiles, 1)
    rows = (n + cols - 1) // cols
    img = Image.new('RGBA', (cols * 8, rows * 8), (0, 0, 0, 0))
    base = pal * 16 if ncgr.bpp == 4 else 0
    for i in range(n):
        tile_into(img, ncgr.tile(i), (i % cols) * 8, (i // cols) * 8, nclr.colors, base)
    return img


def screen_palettes(nscr_d):
    """Palettes used by an NSCR, from most to least frequent."""
    from collections import Counter
    s = sections(nscr_d)[b'NRCS']
    size = struct.unpack_from('<I', nscr_d, s + 0x10)[0]
    c = Counter(struct.unpack_from('<H', nscr_d, s + 0x14 + i * 2)[0] >> 12 for i in range(size // 2))
    return [p for p, _ in c.most_common()]


def screen(nscr_d, ncgr, nclr):
    s = sections(nscr_d)[b'NRCS']
    w, h = struct.unpack_from('<HH', nscr_d, s + 8)
    size = struct.unpack_from('<I', nscr_d, s + 0x10)[0]
    data = nscr_d[s + 0x14: s + 0x14 + size]
    tw = w // 8
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    bg = nclr.colors[0] + (255,)
    img.paste(bg, (0, 0, w, h))
    for i in range(len(data) // 2):
        e = struct.unpack_from('<H', data, i * 2)[0]
        t, hf, vf, pal = e & 0x3FF, (e >> 10) & 1, (e >> 11) & 1, e >> 12
        x, y = (i % tw) * 8, (i // tw) * 8
        if y >= h:
            break
        base = pal * 16 if ncgr.bpp == 4 else 0
        tile_into(img, ncgr.tile(t), x, y, nclr.colors, base, hf, vf)
    return img


SHAPES = {(0, 0): (8, 8), (0, 1): (16, 16), (0, 2): (32, 32), (0, 3): (64, 64),
          (1, 0): (16, 8), (1, 1): (32, 8), (1, 2): (32, 16), (1, 3): (64, 32),
          (2, 0): (8, 16), (2, 1): (8, 32), (2, 2): (16, 32), (2, 3): (32, 64)}


def cells(ncer_d, ncgr, nclr):
    """Renders each cell of an NCER as a cropped PNG. Returns [(img, (minx,miny))]."""
    s = sections(ncer_d)[b'KBEC']
    ncell, ctype = struct.unpack_from('<HH', ncer_d, s + 8)
    coff = struct.unpack_from('<I', ncer_d, s + 0xC)[0]
    mapping = struct.unpack_from('<I', ncer_d, s + 0x10)[0]
    base = s + 8 + coff
    esz = 16 if ctype == 1 else 8
    oam_base = base + ncell * esz
    boundary = {0: 32, 1: 64, 2: 128, 3: 256}.get(mapping, 32)  # 4 = 2D
    tstep = boundary // 32 if ncgr.bpp == 4 else boundary // 64 or 1
    out = []
    for c in range(ncell):
        noam, _attr, ooff = struct.unpack_from('<HHI', ncer_d, base + c * esz)
        objs = []
        for k in range(noam):
            a0, a1, a2 = struct.unpack_from('<HHH', ncer_d, oam_base + ooff + k * 6)
            y = a0 & 0xFF; y = y - 256 if y >= 128 else y
            x = a1 & 0x1FF; x = x - 512 if x >= 256 else x
            shape, size = a0 >> 14, a1 >> 14
            w, h = SHAPES.get((shape, size), (8, 8))
            hf, vf = (a1 >> 12) & 1, (a1 >> 13) & 1
            is8 = (a0 >> 13) & 1
            objs.append((x, y, w, h, hf, vf, a2 & 0x3FF, a2 >> 12, is8))
        if not objs:
            out.append((Image.new('RGBA', (1, 1)), (0, 0)))
            continue
        mx = min(o[0] for o in objs); my = min(o[1] for o in objs)
        Mx = max(o[0] + o[2] for o in objs); My = max(o[1] + o[3] for o in objs)
        img = Image.new('RGBA', (Mx - mx, My - my), (0, 0, 0, 0))
        for x, y, w, h, hf, vf, t, pal, is8 in reversed(objs):
            tw = w // 8
            first = t * tstep
            pbase = 0 if ncgr.bpp == 8 else pal * 16
            for ty in range(h // 8):
                for tx in range(tw):
                    ti = first + ty * tw + tx
                    dx = (tw - 1 - tx) if hf else tx
                    dy = (h // 8 - 1 - ty) if vf else ty
                    tile_into(img, ncgr.tile(ti), x - mx + dx * 8, y - my + dy * 8,
                              nclr.colors, pbase, hf, vf)
        out.append((img, (mx, my)))
    return out


def bmg(d):
    """Returns the list of strings of a BMG (UTF-16LE). Control tags are marked as {tag:...}."""
    p = 0x20
    inf = dat = None
    while p < len(d):
        mg = d[p:p + 4]; sz = struct.unpack_from('<I', d, p + 4)[0]
        if mg == b'INF1':
            inf = p
        elif mg == b'DAT1':
            dat = p
        if sz == 0:
            break
        p += sz
    n, esz = struct.unpack_from('<HH', d, inf + 8)
    enc = d[0x10] if len(d) > 0x10 else 2
    out = []
    for i in range(n):
        off = struct.unpack_from('<I', d, inf + 0x10 + i * esz)[0]
        q = dat + 8 + off
        s = ''
        while q + 1 < len(d):
            ch = struct.unpack_from('<H', d, q)[0]; q += 2
            if ch == 0:
                break
            if ch == 0x1A:
                ln = d[q]
                s += '{tag:' + d[q + 1:q - 2 + ln].hex() + '}'
                q += ln - 2
                continue
            s += chr(ch)
        out.append(s)
    return out


def blz(data):
    """'Bottom LZ' (BLZ) decompression, compresses from the end backwards."""
    hdr, inc = struct.unpack_from('<II', data, len(data) - 8)
    enc_len, hdr_len = hdr & 0xFFFFFF, hdr >> 24
    buf = bytearray(data) + bytearray(inc)
    src, src_end, dst = len(data) - hdr_len, len(data) - enc_len, len(data) + inc
    while src > src_end:
        src -= 1; flags = buf[src]
        for _ in range(8):
            if src <= src_end:
                break
            if flags & 0x80:
                src -= 2
                v = buf[src + 1] << 8 | buf[src]
                ln, pos = (v >> 12) + 3, (v & 0xFFF) + 3
                for _ in range(ln):
                    dst -= 1; buf[dst] = buf[dst + pos]
            else:
                src -= 1; dst -= 1; buf[dst] = buf[src]
            flags = (flags << 1) & 0xFF
    return bytes(buf)


def twl_font_table(d):
    """System fonts from /sys/TWLFontTable.dat -> {name: NFTR}."""
    out = {}
    for i in range(3):
        e = 0xA0 + i * 0x40
        name = d[e:e + 0x20].rstrip(b'\0').decode()
        if not name:
            break
        cs, off, ds = struct.unpack_from('<III', d, e + 0x20)
        f = blz(d[off:off + cs])
        assert len(f) == ds and f[:4] == b'RTFN', name
        out[name] = f
    return out
