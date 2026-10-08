#include <string.h>
#include "gfx.h"

Surface screens[2];
static u16 buf_top[SCR_W * SCR_H] ALIGN(32);
static u16 buf_bottom[SCR_W * SCR_H] ALIGN(32);

void gfx_init(void) {
	videoSetMode(MODE_5_2D | DISPLAY_BG3_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D | DISPLAY_SPR_1D_SIZE_128);
	videoSetModeSub(MODE_5_2D | DISPLAY_BG3_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D | DISPLAY_SPR_1D_SIZE_128);
	vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
	vramSetBankB(VRAM_B_MAIN_SPRITE_0x06400000);
	vramSetBankC(VRAM_C_SUB_BG_0x06200000);
	vramSetBankD(VRAM_D_SUB_SPRITE);

	bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	bgSetPriority(3, 3);
	bgSetPriority(7, 3);

	screens[TOP].px = buf_top;
	screens[BOTTOM].px = buf_bottom;
	gfx_clear(TOP, COL_WHITE);
	gfx_clear(BOTTOM, COL_WHITE);
}

void gfx_present(void) {
	for (int s = 0; s < 2; s++) {
		if (!screens[s].dirty) continue;
		DC_FlushRange(screens[s].px, SCR_W * SCR_H * 2);
		dmaCopyWords(3, screens[s].px, s == TOP ? BG_GFX : BG_GFX_SUB, SCR_W * SCR_H * 2);
		screens[s].dirty = false;
	}
}

u16 gfx_blend(u16 a, u16 b, int al) {
	int r = ((a & 31) * al + (b & 31) * (16 - al)) >> 4;
	int g = (((a >> 5) & 31) * al + ((b >> 5) & 31) * (16 - al)) >> 4;
	int bl = (((a >> 10) & 31) * al + ((b >> 10) & 31) * (16 - al)) >> 4;
	return r | (g << 5) | (bl << 10) | 0x8000;
}

static bool clip(int *x, int *y, int *w, int *h, int *sx, int *sy) {
	if (*x < 0) { *w += *x; if (sx) *sx -= *x; *x = 0; }
	if (*y < 0) { *h += *y; if (sy) *sy -= *y; *y = 0; }
	if (*x + *w > SCR_W) *w = SCR_W - *x;
	if (*y + *h > SCR_H) *h = SCR_H - *y;
	return *w > 0 && *h > 0;
}

void gfx_clear(int scr, u16 color) {
	u32 c = color | (color << 16);
	dmaFillWords(c, screens[scr].px, SCR_W * SCR_H * 2);
	screens[scr].dirty = true;
}

void gfx_fill(int scr, int x, int y, int w, int h, u16 color) {
	if (!clip(&x, &y, &w, &h, NULL, NULL)) return;
	for (int j = 0; j < h; j++) {
		u16 *p = screens[scr].px + (y + j) * SCR_W + x;
		for (int i = 0; i < w; i++) p[i] = color;
	}
	screens[scr].dirty = true;
}

void gfx_blend_fill(int scr, int x, int y, int w, int h, u16 color, int al) {
	if (!clip(&x, &y, &w, &h, NULL, NULL)) return;
	for (int j = 0; j < h; j++) {
		u16 *p = screens[scr].px + (y + j) * SCR_W + x;
		for (int i = 0; i < w; i++) p[i] = gfx_blend(color, p[i], al);
	}
	screens[scr].dirty = true;
}

void gfx_hgradient_rows(int scr, int y, int h, const u16 *rows) {
	for (int j = 0; j < h; j++) gfx_fill(scr, 0, y + j, SCR_W, 1, rows[j]);
}

void gfx_rect(int scr, int x, int y, int w, int h, u16 color) {
	gfx_fill(scr, x, y, w, 1, color);
	gfx_fill(scr, x, y + h - 1, w, 1, color);
	gfx_fill(scr, x, y, 1, h, color);
	gfx_fill(scr, x + w - 1, y, 1, h, color);
}

void gfx_round_rect(int scr, int x, int y, int w, int h, u16 fill, u16 border) {
	gfx_fill(scr, x + 1, y + 1, w - 2, h - 2, fill);
	gfx_fill(scr, x + 2, y, w - 4, 1, border);
	gfx_fill(scr, x + 2, y + h - 1, w - 4, 1, border);
	gfx_fill(scr, x, y + 2, 1, h - 4, border);
	gfx_fill(scr, x + w - 1, y + 2, 1, h - 4, border);
	gfx_fill(scr, x + 1, y + 1, 1, 1, border);
	gfx_fill(scr, x + w - 2, y + 1, 1, 1, border);
	gfx_fill(scr, x + 1, y + h - 2, 1, 1, border);
	gfx_fill(scr, x + w - 2, y + h - 2, 1, 1, border);
}

void gfx_image_part(int scr, const Image *img, int sx, int sy, int w, int h, int x, int y) {
	if (!clip(&x, &y, &w, &h, &sx, &sy)) return;
	for (int j = 0; j < h; j++) {
		const u16 *s = img->px + (sy + j) * img->w + sx;
		u16 *d = screens[scr].px + (y + j) * SCR_W + x;
		for (int i = 0; i < w; i++)
			if (s[i] & 0x8000) d[i] = s[i];
	}
	screens[scr].dirty = true;
}

void gfx_image(int scr, const Image *img, int x, int y) {
	gfx_image_part(scr, img, 0, 0, img->w, img->h, x, y);
}

void gfx_image_tile_x(int scr, const Image *img, int x, int y, int w) {
	for (int i = 0; i < w; i += img->w)
		gfx_image_part(scr, img, 0, 0, (w - i) < img->w ? (w - i) : img->w, img->h, x + i, y);
}

// Tiled background pack (see tools/build_assets.py: bg_pack)
typedef struct {
	u16 w, h;
	u8 bpp, pad[3];
	u32 npal, tilebytes, nmap;
} BgPack;

static void bg_pack(int scr, const void *pack, int scrollx, int scrolly, bool transparent) {
	const BgPack *b = pack;
	const u16 *pal = (const u16 *)(b + 1);
	const u8 *tiles = (const u8 *)pal + ((b->npal * 2 + 3) & ~3);
	const u16 *map = (const u16 *)(tiles + ((b->tilebytes + 3) & ~3));
	int tw = b->w / 8, th = b->h / 8;
	int tsize = b->bpp * 8;
	u16 *dst = screens[scr].px;
	for (int y = 0; y < SCR_H; y++) {
		int my = (y + scrolly) % b->h;
		if (my < 0) my += b->h;
		for (int x = 0; x < SCR_W; x++) {
			int mx = (x + scrollx) % b->w;
			if (mx < 0) mx += b->w;
			int ti = (my / 8) * tw + mx / 8;
			if (ti >= tw * th) continue;
			u16 e = map[ti];
			int px = mx & 7, py = my & 7;
			if (e & 0x400) px = 7 - px;
			if (e & 0x800) py = 7 - py;
			const u8 *t = tiles + (e & 0x3FF) * tsize;
			int c;
			if (b->bpp == 4) {
				c = t[py * 4 + px / 2];
				c = (px & 1) ? c >> 4 : c & 15;
				if (c) c += (e >> 12) * 16;
			} else {
				c = t[py * 8 + px];
			}
			if (transparent && !(c & 15)) continue;
			dst[y * SCR_W + x] = pal[c] | 0x8000;
		}
	}
	screens[scr].dirty = true;
}

void gfx_bg_pack(int scr, const void *pack, int scrollx, int scrolly) {
	bg_pack(scr, pack, scrollx, scrolly, false);
}

void gfx_bg_pack_transparent(int scr, const void *pack, int scrollx, int scrolly) {
	bg_pack(scr, pack, scrollx, scrolly, true);
}

// NDS banner icon: 4x4 tiles of 8x8 at 4bpp + 16-color palette
void gfx_icon4bpp(int scr, const u8 *tiles, const u16 *pal, int x0, int y0, int scale) {
	for (int y = 0; y < 32; y++) {
		for (int x = 0; x < 32; x++) {
			const u8 *t = tiles + ((y / 8) * 4 + x / 8) * 32;
			int c = t[(y & 7) * 4 + (x & 7) / 2];
			c = (x & 1) ? c >> 4 : c & 15;
			if (!c) continue;
			gfx_fill(scr, x0 + x * scale, y0 + y * scale, scale, scale, pal[c] | 0x8000);
		}
	}
}
