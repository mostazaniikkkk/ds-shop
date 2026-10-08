// 16-bit surfaces (one per screen) and drawing primitives.
#pragma once
#include <nds.h>

#define SCR_W 256
#define SCR_H 192

typedef struct {
	u16 *px;	// 256x192, ARGB1555 (bit 15 = opaque)
	bool dirty;
} Surface;

enum { TOP = 0, BOTTOM = 1 };
extern Surface screens[2];

#define RGB(r, g, b) ((u16)(((r) >> 3) | (((g) >> 3) << 5) | (((b) >> 3) << 10) | 0x8000))
#define HEX(c) RGB(((c) >> 16) & 255, ((c) >> 8) & 255, (c) & 255)

// Colors from the original stylesheet (error.css)
#define COL_WHITE   HEX(0xFFFFFF)
#define COL_TEXT    HEX(0x555555)
#define COL_TITLE   HEX(0x333333)
#define COL_BLUE    HEX(0x34BEED)
#define COL_RED     HEX(0xFF0000)
#define COL_IMPORT  HEX(0xFF2244)
#define COL_GRAY    HEX(0x888888)

typedef struct {
	u16 w, h;
	u16 px[];
} Image;

void gfx_init(void);
void gfx_present(void);	// copies the dirty surfaces to VRAM (call after VBlank)

void gfx_clear(int scr, u16 color);
void gfx_fill(int scr, int x, int y, int w, int h, u16 color);
void gfx_blend_fill(int scr, int x, int y, int w, int h, u16 color, int alpha16);
void gfx_hgradient_rows(int scr, int y, int h, const u16 *rows);
void gfx_rect(int scr, int x, int y, int w, int h, u16 color);
void gfx_round_rect(int scr, int x, int y, int w, int h, u16 fill, u16 border);
void gfx_image(int scr, const Image *img, int x, int y);
void gfx_image_part(int scr, const Image *img, int sx, int sy, int w, int h, int x, int y);
void gfx_image_tile_x(int scr, const Image *img, int x, int y, int w);
void gfx_bg_pack(int scr, const void *pack, int scrollx, int scrolly);
void gfx_bg_pack_transparent(int scr, const void *pack, int scrollx, int scrolly);	// without color 0
void gfx_icon4bpp(int scr, const u8 *tiles, const u16 *pal, int x, int y, int scale);

u16 gfx_blend(u16 a, u16 b, int alpha16);	// alpha16 0..16 over a
