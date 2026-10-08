"""Extracts the DSi Shop assets from a NAND dump (or from an already extracted .app).

Usage:
    python -I tools/extract_assets.py dsinand.bin      # from the NAND
    python -I tools/extract_assets.py shop.app         # from the .app

Output:
    assets_src/   original NitroFS tree, with the .szs files decompressed and unpacked
    assets_png/   PNG previews (screens, tile sheets and cells)
    assets_src/message/*.json   BMG texts
"""
import json
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deps  # noqa: E402
deps.check()
import nitro  # noqa: E402
from narc import unpack as narc_unpack  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'assets_src')
PNG = os.path.join(ROOT, 'assets_png')
SYSTITLES = '/title/00030015'
SHOP_TID = '484E46'	# HNF?: the DSi Shop (the last letter is the region: E, P, J, U, K, C)


def open_nand(path):
    """The main NAND partition (FAT16), already decrypted."""
    if not os.path.isfile(path):
        sys.exit('Cannot find %s. Copy your NAND dump there (see README, step 0)' % path)
    from dsicrypt import Nand
    from fat import Fat
    try:
        return Fat(Nand(path), 0x10EE00)
    except ValueError as e:
        sys.exit('Error: %s' % e)


def load_app(path):
    """The DSi Shop .app: from the NAND, or the file as is if it already is the .app."""
    if os.path.isfile(path):
        with open(path, 'rb') as f:
            head = f.read(16)
        if head[12:15] == b'HNF':
            return open(path, 'rb').read()
    fs = open_nand(path)
    shops = [n for n, d, c, s in fs.ls(SYSTITLES) if d and n.upper().startswith(SHOP_TID)]
    for tid in shops:
        content = SYSTITLES + '/' + tid + '/content'
        apps = [n for n, d, c, s in fs.ls(content) if n.upper().endswith('.APP')]
        if apps:
            print('DSi Shop: %s/%s' % (content, apps[0]))
            return fs.get(content + '/' + apps[0])
    sys.exit('This NAND does not contain the DSi Shop (%s/%s?). It ships preinstalled on every DSi;'
             ' if it was deleted, there is nothing to extract.' % (SYSTITLES, SHOP_TID))


def nitrofs(d):
    fnto, _, fato, fats = struct.unpack_from('<4I', d, 0x40)
    fat = [struct.unpack_from('<II', d, fato + i * 8) for i in range(fats // 8)]

    def walk(did, path):
        off, first, _ = struct.unpack_from('<IHH', d, fnto + (did & 0xFFF) * 8)
        p = fnto + off; fid = first
        while True:
            ln = d[p]; p += 1
            if ln == 0:
                break
            nm = d[p:p + (ln & 0x7F)].decode(); p += ln & 0x7F
            if ln & 0x80:
                sub = struct.unpack_from('<H', d, p)[0]; p += 2
                yield from walk(sub, path + nm + '/')
            else:
                s, e = fat[fid]; fid += 1
                yield path + nm, d[s:e]
    yield from walk(0xF000, '')


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def best_match(stem, candidates):
    """The candidate whose name shares the longest prefix with stem."""
    def score(c):
        n = 0
        for a, b in zip(stem.lower(), c.lower()):
            if a != b:
                break
            n += 1
        return n
    return max(candidates, key=score) if candidates else None


def render_layout(folder, outdir):
    files = {f: open(os.path.join(folder, f), 'rb').read() for f in os.listdir(folder)}
    by_ext = lambda e: sorted(f for f in files if f.lower().endswith(e))  # noqa: E731
    os.makedirs(outdir, exist_ok=True)
    for g in by_ext('.ncgr'):
        stem = g[:-5]
        pal = best_match(stem, by_ext('.nclr'))
        if not pal:
            continue
        ncgr, nclr = nitro.NCGR(files[g]), nitro.NCLR(files[pal])
        scrs = [s for s in by_ext('.nscr') if s[:-5].rstrip('0123456789').lower() == stem.lower()]
        palno = 0
        for s in scrs:
            nitro.screen(files[s], ncgr, nclr).save(os.path.join(outdir, s[:-5] + '_screen.png'))
            palno = nitro.screen_palettes(files[s])[0]
        nitro.sheet(ncgr, nclr, pal=palno).save(os.path.join(outdir, stem + '_tiles.png'))
        for cer in by_ext('.ncer'):
            if cer[:-5].lower() != stem.lower():
                continue
            for i, (img, _) in enumerate(nitro.cells(files[cer], ncgr, nclr)):
                img.save(os.path.join(outdir, '%s_cell%02d.png' % (cer[:-5], i)))


def main():
    d = load_app(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'dsinand.bin'))
    for name, data in nitrofs(d):
        out = os.path.join(SRC, name)
        if name.endswith('.szs'):
            arc = out[:-4] + '.narc'
            write(arc, nitro.yaz0(data))
            narc_unpack(arc, out[:-4])
            render_layout(out[:-4], os.path.join(PNG, 'layout', os.path.basename(out[:-4])))
        else:
            write(out, data)
        if name.endswith('.zip'):	# e.g. skin/std_skin.zip (dialog icons)
            with zipfile.ZipFile(out) as z:
                z.extractall(out[:-4])
        if name.endswith('.bmg'):
            with open(out[:-4] + '.json', 'w', encoding='utf-8') as f:
                json.dump(nitro.bmg(data), f, ensure_ascii=False, indent=1)
    print('OK ->', SRC, PNG)


if __name__ == '__main__':
    main()
