# DS Shop — a DSi Shop–style store for the Nintendo DS

A homebrew app for the **original Nintendo DS and DS Lite** (DSPico, other
flashcarts, or melonDS) that recreates the Nintendo DSi Shop using its
original graphics, animations, sounds and fonts. Content is published by the
store server, which lives in its own repository:
[ds-shop-server](https://github.com/mostazaniikkkk/ds-shop-server).
The client can use several stores (servers) as sources.

Features:

- Browse and download titles from one or more store servers, merged into a
  single catalog (or one store at a time).
- Resumable downloads with automatic retries.
- Add, edit and delete stores right on the console, with the system keyboard.
- **Experimental BitTorrent client running on the DS itself** (HTTP and UDP
  trackers).
- **QR code scanning on DSi** (outer camera) for store and `.torrent` links.
- Spanish and English UI (follows the console language).

```
dsinand.bin          your DSi NAND dump (the assets come from here; never distributed)
tools/               Python scripts: decrypt the NAND, extract and convert assets
homebrew/            the DS client (C, devkitPro/libnds)
PROTOCOL.md          the API between the client and store servers
```

> **No Nintendo assets are included in this repository.** You build them from
> a NAND dump of your own DSi (steps 0 and 1).

## 0. Dump your DSi NAND

1. Your DSi must be able to run homebrew (Unlaunch, hiyaCFW or TWiLight Menu++;
   see https://dsi.cfw.guide).
2. Run **dumpTool** or **GodMode9i** and dump the NAND to the SD card.
3. Copy the file into the root of this repository as `dsinand.bin`.

The dump must end with the `DSi eMMC CID/CPU` footer (no$gba format). It holds
your console's CID and console ID, which `tools/dsicrypt.py` needs to decrypt
the NAND. dumpTool and GodMode9i both add it; if it is missing the scripts stop
right away with an error.

## 1. Extract the assets from the NAND (once)

Everything the client shows — graphics, sounds, texts and fonts — comes from
the DSi Shop and the system fonts on **your** NAND. Two scripts do the work;
you only need to run them once (and again if you switch to a different NAND).

**Requirements:** Python 3.8 or newer and two libraries:

```
python -m pip install pycryptodome Pillow
```

(On Linux/macOS the command may be `python3`.) If a library is missing, the
scripts tell you and print the command to install it.

**Steps**, from the repository root (the folder containing `tools/` and
`homebrew/`), with the dump copied as `dsinand.bin`:

```
python -I tools/extract_assets.py dsinand.bin
python -I tools/build_assets.py dsinand.bin
```

Order matters: `build_assets.py` uses the output of `extract_assets.py`. If
your dump has a different name or location, pass its path instead of
`dsinand.bin` (with no argument, `dsinand.bin` in the root is used).

1. **`extract_assets.py`** decrypts the NAND using the CID/CPU footer, finds the
   DSi Shop under `/title/00030015/484E46xx` (any region works) and unpacks its
   file system. It takes a few seconds and ends with `OK -> ...`. Output:
   - `assets_src/`: the shop's original files (decompressed) and its texts in
     `message/*.json`.
   - `assets_png/`: PNG previews, just for looking at.
2. **`build_assets.py`** converts what the client needs into formats the DS
   can draw directly, and extracts the system fonts from
   `/sys/TWLFontTable.dat`. It ends with `OK`. Output:
   - `homebrew/arm9/data/*.bin`: backgrounds, buttons, sprites, fonts,
     keyboard and sound (the original `sound_data.sdat`).
   - `homebrew/arm9/include/assets_gen.h` and `kbd_gen.h`: asset indices and
     key positions.
   - `homebrew/arm9/source/bmg_text.c`: the original texts in every language.
   - `homebrew/icon.bmp`: the banner icon (the DSi Shop's).

That's all you need to build (step 2).

**If something fails**, the scripts stop with a message:

| Message | What it means |
|---|---|
| `Cannot find ...` | the path to the dump is wrong |
| `does not end with the "DSi eMMC CID/CPU" footer` | the dump has no footer; dump it again with dumpTool or GodMode9i |
| `is too small to be a DSi NAND` | the file is truncated (a NAND dump is about 240 MB) |
| `the NAND does not decrypt correctly` | the dump is damaged, or the CID/CPU footer belongs to another console |
| `This NAND does not contain the DSi Shop` | the shop was deleted from that console; there is nothing to extract |
| `Missing .../assets_src/...: run tools/extract_assets.py first` | `build_assets.py` was run first |

Everything generated is Nintendo material taken from your console and is
listed in `.gitignore`: do not upload it, or the compiled `.nds`, anywhere
public.

## 2. Build the DS client

You only need **Docker**. The build runs inside the official devkitPro image,
so nothing is installed on your system:

```
sh homebrew/build.sh        # -> homebrew/DSShop.nds
sh homebrew/build.sh r4     # also DSShop_R4.nds, patched with the original R4 DLDI driver
sh homebrew/build.sh clean
```

The image is pinned (`devkitpro/devkitarm:20240918`: devkitARM r65, libnds
1.8.3, dswifi 0.4.2) because newer images ship libnds 2.x, which this code
does not target yet. On Windows, run it from Git Bash with Docker Desktop
started.

By default titles from all stores are shown **together**: categories with the
same name are merged, each title shows which store it comes from, and a game
found in several stores appears only once. If a store does not respond, a
notice appears at the top and the others are still shown. Settings →
"Shops: Separately" switches back to picking one store at a time.

Stores are managed on the DS itself ("Choose a Shop" screen: "+ Add Shop", and
"Edit" or the X button to change or delete one), using the original system
keyboard. On the keyboard: B deletes, START accepts, SELECT cancels and L/R
switch between letters, accents, symbols and pictograms.

If a download drops, it is retried automatically up to 5 times. If it still
fails, the partial data is kept (`<game>.nds.part` + `.part.info`) and the
title page shows "Resume Download", which continues where it left off (HTTP
`Range` request).

### Torrents (experimental)

Off by default. Turning on Settings → "Torrents (experimental)" adds a
"Download from Torrent" button to the menu: you enter the full link to a
`.torrent` (`http://...`), the DS downloads it and then fetches the content
**by itself** from other peers (the BitTorrent client runs on the console,
`homebrew/arm9/source/torrent.c`; the server is not involved). If the torrent
has several files you pick one (the first `.nds` is preselected), and it is
saved next to `DSShop.nds` like any other download.

- `http://` and `udp://` trackers. No HTTPS (the DS has no TLS), no DHT and no
  magnet links: the torrent needs at least one tracker of those two kinds.
- Outgoing connections only, and it never uploads (it never unchokes other
  peers), so reachable seeders are required.
- Every piece is verified against its SHA‑1 before it is accepted.
- Cancelling keeps the partial data (`.part` + `.part.info`); the same
  `.torrent` and file resume from there. The last link is remembered.
- Up to 512 MB per file.

### QR scanning (DSi only)

On a DSi (in DSi mode), the store editor and the torrent screen have a
"Scan QR Code" button that uses the outer camera. The QR code can contain
`http://server:port`, `Name|http://server:port` or a `.torrent` link. The DS
and DS Lite have no camera, so the button is not shown there. Camera control is
adapted from the BlocksDS libnds (Zlib) and QR decoding uses
[quirc](https://github.com/dlbeer/quirc) (ISC). **Not yet tested on real DSi
hardware.**

On the SD card:

```
sd:/data/dsstore/fuentes.txt       stores (written by the DS; format  Name|http://192.168.1.10:8080)
sd:/data/dsstore/config.ini        settings (music, merged/separate stores, torrents, last .torrent)
sd:/data/dsstore/log.txt           network log, useful when something won't connect
```

Downloads are saved in the same folder as `DSShop.nds` (the launcher provides
the path; if it doesn't, as with old R4 kernels, they go to the SD root).
Settings from older versions (`sd:/dsishop/`) are moved to `sd:/data/dsstore/`
automatically.

**Wi‑Fi:** the client uses the connection saved in the firmware (the one you
set up from a game with Nintendo WFC). The original DS can only join **open or
WEP** networks: with a modern router, the easiest option is an open guest
network or a phone hotspot without a password. In melonDS it works with no
setup: use your PC's LAN IP address (not `localhost`).

## License

This project is dedicated to the public domain under
[CC0 1.0](LICENSE): use it however you like, no attribution required.

Exceptions (third-party code, under its own license, whose notices must be kept):

- `homebrew/arm9/source/quirc/`: [quirc](https://github.com/dlbeer/quirc), ISC license (`quirc/LICENSE`).
- `homebrew/arm7/source/camera.c` and the camera part of `homebrew/arm9/source/qr.c`: adapted from
  [BlocksDS libnds](https://github.com/blocksds/libnds), Zlib license (notice in the file).
- `homebrew/dldi/r4tf.dldi`: the original R4 DLDI driver, from https://www.chishm.com/DLDI/.

The Nintendo assets the tools extract from your NAND are not covered by this
license and are not part of the repository.
