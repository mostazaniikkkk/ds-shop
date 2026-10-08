"""Converts the assets extracted from the DSi Shop into the format the homebrew uses.

Usage:
    python -I tools/build_assets.py [dsinand.bin]

Requires tools/extract_assets.py to have been run first (it uses assets_src/).
The NAND is only used to read the system fonts (/sys/TWLFontTable.dat).

Generates:
    homebrew/arm9/data/*.bin           blobs embedded with bin2o
    homebrew/arm9/include/assets_gen.h cell, SE and text indices
    homebrew/icon.bmp                  banner icon (the DSi Shop one)
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deps  # noqa: E402
deps.check()
from PIL import Image  # noqa: E402
import nitro  # noqa: E402
import sdat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'assets_src')
LAY = os.path.join(SRC, 'layout', 'cmn')
OUT = os.path.join(ROOT, 'homebrew', 'arm9', 'data')
INC = os.path.join(ROOT, 'homebrew', 'arm9', 'include')

header_lines = []


def rd(path):
    with open(path, 'rb') as f:
        return f.read()


def find(folder, ext):
    return sorted(os.path.join(folder, f) for f in os.listdir(folder) if f.lower().endswith(ext))


def emit(name, data):
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name + '.bin'), 'wb') as f:
        f.write(data)


def pad4(b):
    return b + b'\0' * (-len(b) % 4)


def rgb555(c, a=True):
    r, g, b = c[:3]
    return (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10 | (0x8000 if a else 0)


# --------------------------------------------------------------------------
# Tiled backgrounds: header + palette + tiles + map (native DS format)
#   u16 w, h (px); u8 bpp; u8 pad[3]; u32 npal; u32 tilebytes; u32 nmap
# --------------------------------------------------------------------------
def bg_pack(name, ncgr, nclr, nscr):
    g = nitro.NCGR(rd(ncgr))
    p = nitro.NCLR(rd(nclr))
    s_d = rd(nscr)
    s = nitro.sections(s_d)[b'NRCS']
    w, h = struct.unpack_from('<HH', s_d, s + 8)
    size = struct.unpack_from('<I', s_d, s + 0x10)[0]
    mp = s_d[s + 0x14:s + 0x14 + size]
    pal = p.raw[:512]
    blob = struct.pack('<HHB3xIII', w, h, g.bpp, len(pal) // 2, len(g.data), len(mp) // 2)
    blob += pad4(pal) + pad4(g.data) + pad4(mp)
    emit(name, blob)


# --------------------------------------------------------------------------
# Sprites (cells from an NCER + animations from an NANR), 4bpp, 1D 128K mapping.
#   u32 tilebytes; u16 npalbanks; u16 ncells; u16 noams; u16 nanims; u16 nframes; u16 pad
#   palette (npalbanks*16 u16) | tiles | cells[ncells] {u16 firstoam, noam; s16 x0,y0,x1,y1}
#   oams[noams] {u16 a0, a1, a2, pad} (a2: tile in 128-byte units, remapped pal)
#   anims[nanims] {u16 firstframe, nframes} | frames[nframes] {u16 cell, dur}
# --------------------------------------------------------------------------
SHAPES = nitro.SHAPES


def sprite_pack(name, folder, stem):
    ncgr = [f for f in find(folder, '.ncgr') if os.path.basename(f)[:-5].lower() == stem.lower()][0]
    ncer = [f for f in find(folder, '.ncer') if os.path.basename(f)[:-5].lower() == stem.lower()][0]
    nclr = find(folder, '.nclr')[0]
    nanr = [f for f in find(folder, '.nanr') if os.path.basename(f)[:-5].lower() == stem.lower()]
    g = nitro.NCGR(rd(ncgr))
    p = nitro.NCLR(rd(nclr))
    assert g.bpp == 4, name
    cd = rd(ncer)
    s = nitro.sections(cd)[b'KBEC']
    ncell, ctype = struct.unpack_from('<HH', cd, s + 8)
    coff = struct.unpack_from('<I', cd, s + 0xC)[0]
    mapping = struct.unpack_from('<I', cd, s + 0x10)[0]
    boundary = {0: 32, 1: 64, 2: 128, 3: 256}[mapping]
    base = s + 8 + coff
    esz = 16 if ctype == 1 else 8
    oam_base = base + ncell * esz

    tiles = bytearray()
    placed = {}     # (start_byte, nbytes) -> new index in 128-byte units
    banks = []      # palette banks used, in order of appearance
    cells, oams = [], []
    for c in range(ncell):
        noam, _, ooff = struct.unpack_from('<HHI', cd, base + c * esz)
        first = len(oams)
        x0 = y0 = 9999; x1 = y1 = -9999
        for k in range(noam):
            a0, a1, a2 = struct.unpack_from('<HHH', cd, oam_base + ooff + k * 6)
            w, h = SHAPES[(a0 >> 14, a1 >> 14)]
            start = (a2 & 0x3FF) * boundary
            nbytes = w * h // 2
            key = (start, nbytes)
            if key not in placed:
                placed[key] = len(tiles) // 128
                tiles += g.data[start:start + nbytes]
                tiles += b'\0' * (-len(tiles) % 128)
            bank = a2 >> 12
            if bank not in banks:
                banks.append(bank)
            a2n = placed[key] | (banks.index(bank) << 12) | (a2 & 0x0C00)
            y = a0 & 0xFF; y = y - 256 if y >= 128 else y
            x = a1 & 0x1FF; x = x - 512 if x >= 256 else x
            x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x + w), max(y1, y + h)
            oams.append(struct.pack('<HHHH', a0, a1, a2n, 0))
        if noam == 0:
            x0 = y0 = x1 = y1 = 0
        cells.append(struct.pack('<HHhhhh', first, noam, x0, y0, x1, y1))

    anims, frames = [], []
    if nanr:
        ad = rd(nanr[0])
        k = nitro.sections(ad)[b'KNBA']
        na, _, ao, fo, do = struct.unpack_from('<HHIII', ad, k + 8)
        kb = k + 8
        for a in range(na):
            nfr, _, _, _, _, foff = struct.unpack_from('<IHHHHI', ad, kb + ao + a * 16)
            anims.append(struct.pack('<HH', len(frames), nfr))
            for i in range(nfr):
                doff, dur, _ = struct.unpack_from('<IHH', ad, kb + fo + foff + i * 8)
                frames.append(struct.pack('<HH', struct.unpack_from('<H', ad, kb + do + doff)[0], dur))

    pal = b''.join(p.raw[b * 32:(b + 1) * 32] for b in banks)
    blob = struct.pack('<IHHHHHH', len(tiles), len(banks), len(cells), len(oams), len(anims), len(frames), 0)
    blob += pad4(pal) + bytes(tiles) + b''.join(cells) + b''.join(oams) + b''.join(anims) + b''.join(frames)
    emit(name, blob)
    header_lines.append('// %s: %d cells, %d anims, %d palette banks, %d tile bytes'
                        % (name, len(cells), len(anims), len(banks), len(tiles)))


# --------------------------------------------------------------------------
# Images (GIF/PNG) -> ARGB1555 bitmap: u16 w, h + pixels
# --------------------------------------------------------------------------
def image_pack(name, path):
    image_pack_img(name, Image.open(path))


def image_pack_img(name, im):
    im = im.convert('RGBA')
    px = im.load()
    data = bytearray(struct.pack('<HH', im.width, im.height))
    for y in range(im.height):
        for x in range(im.width):
            c = px[x, y]
            data += struct.pack('<H', rgb555(c, c[3] >= 128) if c[3] >= 128 else 0)
    emit(name, pad4(bytes(data)))


# --------------------------------------------------------------------------
# System NFTR fonts (TBF1) -> subset
#   u8 cellw, cellh, ascent, linefeed; u16 nglyphs; u8 cellbytes; u8 pad
#   glyphs[n] {u16 code; s8 left; u8 width; u8 advance; u8 pad; u16 pad}
#   2bpp bitmaps (NFTR format, MSB first)
# --------------------------------------------------------------------------
EXTRA = [0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AC, 0x2122, 0x2190, 0x2191,
         0x2192, 0x2193, 0x2605, 0x2606, 0x25CB, 0x25CF, 0x266A, 0x00D7]
KBD_CHARS = set()  # keyboard characters: added to the fonts


# --------------------------------------------------------------------------
# System keyboard (keyboard.szs): key image + positions + maps
# --------------------------------------------------------------------------
KBD_SPECIALS = {'ascii': ['BACKSPACE', 'ENTER', 'SPACE', 'SHIFT', 'CAPS'],
                'euro': ['BACKSPACE', 'ENTER', 'SPACE'], 'picto': ['BACKSPACE', 'ENTER', 'SPACE']}


def kbd_image(cnt, ncl):
    cols = [nitro.bgr555(struct.unpack_from('<H', ncl, i * 2)[0]) for i in range(16)]
    im = Image.new('RGBA', (256, 96), (0, 0, 0, 0))
    for t in range(32 * 12):
        for y in range(8):
            for x in range(8):
                v = cnt[t * 32 + y * 4 + x // 2]
                v = (v >> 4) if x & 1 else v & 15
                if v:
                    im.putpixel(((t % 32) * 8 + x, (t // 32) * 8 + y), cols[v] + (255,))
    return im


def keyboard_pack(lines):
    kb = os.path.join(LAY, 'keyboard')
    ncl = rd(os.path.join(kb, 'fs_keyboardColor.ncl.cnt'))
    images = {'ascii': 'fs_chat_keyboard', 'euro': 'fs_chat_keyboard_euro', 'picto': 'fs_chat_keyboard_picto'}
    for k, f in images.items():
        image_pack_img('img_kbd_' + k, kbd_image(rd(os.path.join(kb, f + '.ncg.cnt')), ncl))
    lines += ['// Generated by tools/build_assets.py - do not edit', '#pragma once', '',
              'typedef struct { u8 x, y, w, h; } KbdRect;', '']
    maps = {'ascii': ['ascii_normal', 'ascii_shift', 'ascii_caps'], 'euro': ['euro_normal'], 'picto': ['sign', 'picto']}
    for k, layout in (('ascii', 'ascii.kbd'), ('euro', 'euro.kbd'), ('picto', 'picto.kbd')):
        d = rd(os.path.join(kb, layout))
        keys = [struct.unpack_from('<BBBBHH', d, i) for i in range(0, len(d), 8)]
        keys.sort(key=lambda e: e[5])
        nspec = len(KBD_SPECIALS[k])
        lines.append('// %s: %d special keys (%s) and %d character keys'
                     % (k, nspec, ', '.join(KBD_SPECIALS[k]), len(keys) - nspec))
        lines.append('static const KbdRect kbd_%s_keys[%d] = {' % (k, len(keys)))
        lines.append('  ' + ', '.join('{%d,%d,%d,%d}' % e[:4] for e in keys))
        lines.append('};')
        for m in maps[k]:
            chars = rd(os.path.join(kb, m + '.kbdmap'))[2:].decode('utf-16le')[:len(keys) - nspec]
            KBD_CHARS.update(ord(c) for c in chars)
            lines.append('static const u16 kbd_map_%s[%d] = {%s};'
                         % (m, len(chars), ','.join('0x%x' % ord(c) for c in chars)))
        lines.append('')

    # shop keyboard pieces (sm_keyboard): tabs and bottom bar
    sk = os.path.join(LAY, 'shop_keyboard')
    g = nitro.NCGR(rd(os.path.join(sk, 'sm_keyboard.ncgr')))
    p = nitro.NCLR(rd(os.path.join(sk, 'sm_keyboard_D.NCLR')))
    cells = nitro.cells(rd(os.path.join(sk, 'sm_keyboard.ncer')), g, p)
    for i, nm in ((28, 'tab_a'), (29, 'tab_euro'), (30, 'tab_sign'), (31, 'tab_picto'),
                  (36, 'tab_a_on'), (37, 'tab_euro_on'), (38, 'tab_sign_on'), (39, 'tab_picto_on'),
                  (0, 'bar_l'), (1, 'bar_r'), (2, 'bar_l_on'), (3, 'bar_r_on')):
        image_pack_img('img_kb_' + nm, cells[i][0])
    bg_pack('bg_keyboard', os.path.join(sk, 'sm_keyboard_D_BG.NCGR'), os.path.join(sk, 'sm_keyboard_D_BG.NCLR'),
            os.path.join(sk, 'sm_keyboard_D_BG01.NSCR'))


def nftr_glyphs(d):
    s = nitro.sections(d)
    finf = s[b'FNIF']
    _, lf, alt, _, _, _, _, cg, cw, cm = struct.unpack_from('<BBHBBBBIII', d, finf + 8)
    ascent = d[finf + 0x1E] if struct.unpack_from('<I', d, finf + 4)[0] >= 0x20 else lf
    cellw, cellh, csz, _, _, bpp, _ = struct.unpack_from('<BBHbBBB', d, cg)
    gbase = cg + 8
    widths = {}
    off = cw
    while off:
        first, last, nxt = struct.unpack_from('<HHI', d, off)
        for i in range(first, last + 1):
            widths[i] = struct.unpack_from('<bBB', d, off + 8 + (i - first) * 3)
        off = nxt
    cmap = {}
    off = cm
    while off:
        b, e, t, _, nxt = struct.unpack_from('<HHHHI', d, off)
        q = off + 12
        if t == 0:
            first = struct.unpack_from('<H', d, q)[0]
            for c in range(b, e + 1):
                cmap[c] = first + c - b
        elif t == 1:
            for c in range(b, e + 1):
                v = struct.unpack_from('<H', d, q + (c - b) * 2)[0]
                if v != 0xFFFF:
                    cmap[c] = v
        else:
            n = struct.unpack_from('<H', d, q)[0]
            for i in range(n):
                c, v = struct.unpack_from('<HH', d, q + 2 + i * 4)
                cmap[c] = v
        off = nxt
    return dict(cellw=cellw, cellh=cellh, csz=csz, bpp=bpp, gbase=gbase, ascent=ascent, lf=lf,
                widths=widths, cmap=cmap, data=d)


def font_pack(name, nftr):
    f = nftr_glyphs(nftr)
    assert f['bpp'] == 2
    want = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + list(range(0xE000, 0xE070)) + EXTRA + sorted(KBD_CHARS)
    codes = sorted(c for c in set(want) if c in f['cmap'])
    table, bitmaps = b'', b''
    for c in codes:
        gi = f['cmap'][c]
        left, gw, adv = f['widths'].get(gi, (0, f['cellw'], f['cellw']))
        table += struct.pack('<HbBBBH', c, left, gw, adv, 0, 0)
        bitmaps += f['data'][f['gbase'] + gi * f['csz']: f['gbase'] + (gi + 1) * f['csz']]
    blob = struct.pack('<BBBBHBB', f['cellw'], f['cellh'], f['ascent'], f['lf'], len(codes), f['csz'], 0)
    emit(name, pad4(blob + table + bitmaps))
    header_lines.append('// %s: %d glyphs %dx%d' % (name, len(codes), f['cellw'], f['cellh']))
    return f


def font_preview(f, path):
    """PNG with the private-use icons (U+E000..) to see what is there."""
    codes = [c for c in range(0xE000, 0xE070) if c in f['cmap']]
    im = Image.new('RGB', (16 * (f['cellw'] + 2), ((len(codes) + 15) // 16) * (f['cellh'] + 2)), (255, 255, 255))
    for n, c in enumerate(codes):
        gi = f['cmap'][c]
        g = f['data'][f['gbase'] + gi * f['csz']:]
        for y in range(f['cellh']):
            for x in range(f['cellw']):
                bit = (y * f['cellw'] + x) * 2
                v = (g[bit >> 3] >> (6 - (bit & 7))) & 3
                if v:
                    im.putpixel(((n % 16) * (f['cellw'] + 2) + x, (n // 16) * (f['cellh'] + 2) + y),
                                (255 - v * 85,) * 3)
    im.resize((im.width * 3, im.height * 3), 0).save(path)


# --------------------------------------------------------------------------
def c_utf8(s):
    out = ''
    for ch in s:
        o = ord(ch)
        if ch == '"':
            out += '\\"'
        elif ch == '\\':
            out += '\\\\'
        elif ch == '\n':
            out += '\\n'
        elif 0x20 <= o < 0x7F:
            out += ch
        else:
            out += ''.join('\\%03o' % b for b in ch.encode('utf-8'))
    return '"' + out + '"'


LANGS = [('jp_jpn', 'LANG_JP'), ('us_eng', 'LANG_EN'), ('us_fra', 'LANG_FR'), ('eu_ger', 'LANG_DE'),
         ('eu_ita', 'LANG_IT'), ('us_spa', 'LANG_ES')]


def strings_and_sounds(h, c):
    msgs = {}
    for folder, _ in LANGS:
        msgs[folder] = json.load(open(os.path.join(SRC, 'message', folder, 'shop.json'), encoding='utf-8'))
    n = max(len(v) for v in msgs.values())
    h.append('\n// Original DSi Shop texts (shop.bmg), by firmware language')
    h.append('#define BMG_COUNT %d' % n)
    h.append('extern const char *const bmg_text[6][BMG_COUNT];')
    c.append('#include "assets_gen.h"')
    c.append('const char *const bmg_text[6][BMG_COUNT] = {')
    for folder, _ in LANGS:
        row = []
        for i in range(n):
            s = msgs[folder][i] if i < len(msgs[folder]) else ''
            s = s.replace('<BR>\n', '\n').replace('<BR>', '\n')
            row.append(c_utf8(s))
        c.append('  { // ' + folder)
        for i in range(0, n, 4):
            c.append('    ' + ', '.join(row[i:i + 4]) + ',')
        c.append('  },')
    c.append('};')

    s = sdat.SDAT(rd(os.path.join(SRC, 'sound', 'sound_data.sdat')))
    h.append('\n// Sound effects (indices within SAR_SE)')
    h.append('enum {')
    for i, nm in enumerate(s.seqarc_names[0]):
        if nm:
            h.append('  %s = %d,' % (nm.replace('TWL_', 'SE_', 1), i))
    h.append('};')


def banner_icon(app, path):
    bo = struct.unpack_from('<I', app, 0x68)[0]
    b = app[bo:]
    bmp, pal = b[0x20:0x220], b[0x220:0x240]
    colors = [nitro.bgr555(struct.unpack_from('<H', pal, i * 2)[0]) for i in range(16)]
    im = Image.new('P', (32, 32))
    im.putpalette([v for c in colors for v in c])
    for t in range(16):
        for y in range(8):
            for x in range(8):
                v = bmp[t * 32 + y * 4 + x // 2]
                v = (v >> 4) if x & 1 else (v & 15)
                im.putpixel(((t % 4) * 8 + x, (t // 4) * 8 + y), v)
    im.save(path)


def main():
    nand = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'dsinand.bin')
    if not os.path.isdir(LAY):
        sys.exit('Missing %s: run tools/extract_assets.py first' % LAY)
    os.makedirs(INC, exist_ok=True)

    # Backgrounds
    su = os.path.join(LAY, 'shop_start_up')
    bg_pack('bg_startup_top', *(os.path.join(su, 'ued_shop_u.' + e) for e in ('NCGR', 'NCLR', 'NSCR')))
    bg_pack('bg_startup_bottom', *(os.path.join(su, 'ued_shop.' + e) for e in ('NCGR', 'NCLR', 'NSCR')))
    dl = os.path.join(LAY, 'shop_dialog')
    bg_pack('bg_dialog', os.path.join(dl, 'ued_dialog_BG.NCGR'), os.path.join(dl, 'ued_dialog_BG.NCLR'),
            os.path.join(dl, 'ued_dialog_BG00.NSCR'))

    # Sprites
    sprite_pack('spr_progress', os.path.join(LAY, 'shop_progressbar'), 'ued_progress_bar')
    sprite_pack('spr_scroll', os.path.join(LAY, 'shop_scrollbutton'), 'ued_scroll_bar')
    sprite_pack('spr_wait', os.path.join(LAY, 'wait_icon'), 'ued_wait_icon')
    sprite_pack('spr_click', os.path.join(LAY, 'shop_click_effect'), 'ued_kettei')
    sprite_pack('spr_tuusin', os.path.join(LAY, 'shop_tuusin_icon'), 'ued_tuusin_icon')

    # Error page images / buttons
    er = os.path.join(SRC, 'htmls', 'error')
    for f in ('button_224x28_all', 'button_224x32_all', 'violet_128x28_all', 'violet_128x40_all',
              'E_u_bar', 'E_bg_u', 'E_bg_d', 'Wi-Fi_on'):
        image_pack('img_' + f.replace('-', '_').lower(), os.path.join(er, f + '.gif'))
    sk = os.path.join(SRC, 'skin', 'std_skin')
    for f in ('dialog_images/info.png', 'dialog_images/question.png', 'dialog_images/warning.png',
              'dialog_images/error.png'):
        image_pack('img_dlg_' + os.path.basename(f)[:-4], os.path.join(sk, f))

    # Keyboard (before the fonts: it adds its characters)
    kbd = []
    keyboard_pack(kbd)
    with open(os.path.join(INC, 'kbd_gen.h'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(kbd) + '\n')

    # DSi system fonts (from the NAND)
    from extract_assets import open_nand
    fs = open_nand(nand)
    table = [n for n, d, c, s in fs.ls('/sys') if n.upper().startswith('TWLFON')][0]
    fonts = nitro.twl_font_table(fs.get('/sys/' + table))
    for k, nm in (('TBF1_s.NFTR', 'font_s'), ('TBF1_m.NFTR', 'font_m'), ('TBF1_l.NFTR', 'font_l')):
        f = font_pack(nm, fonts[k])
    font_preview(f, os.path.join(ROOT, 'assets_png', 'font_icons.png'))

    # Sound: the SDAT as is (the ARM7 interprets it)
    emit('sound_data', rd(os.path.join(SRC, 'sound', 'sound_data.sdat')))

    # Banner icon
    from extract_assets import load_app
    banner_icon(load_app(nand), os.path.join(ROOT, 'homebrew', 'icon.bmp'))

    h = ['// Generated by tools/build_assets.py - do not edit', '#pragma once', '']
    h += header_lines
    c = ['// Generated by tools/build_assets.py - do not edit']
    strings_and_sounds(h, c)
    with open(os.path.join(INC, 'assets_gen.h'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(h) + '\n')
    with open(os.path.join(ROOT, 'homebrew', 'arm9', 'source', 'bmg_text.c'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(c) + '\n')
    print('OK')


if __name__ == '__main__':
    main()
