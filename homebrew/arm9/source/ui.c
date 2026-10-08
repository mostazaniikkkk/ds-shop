#include <stdio.h>
#include <stdlib.h>
#include "ui.h"
#include "snd.h"
#include "img_button_224x28_all_bin.h"
#include "img_button_224x32_all_bin.h"
#include "img_violet_128x28_all_bin.h"
#include "img_violet_128x40_all_bin.h"
#include "img_e_u_bar_bin.h"
#include "bg_dialog_bin.h"
#include "spr_progress_bin.h"
#include "spr_scroll_bin.h"
#include "spr_wait_bin.h"
#include "spr_click_bin.h"
#include "spr_tuusin_bin.h"

Input in;
SprSet spr_progress, spr_scroll, spr_wait, spr_click, spr_tuusin;
static AnimPlayer click_anim;
static int click_x, click_y;

#define IMG(x) ((const Image *)(x))

void ui_init(void) {
	spr_init();
	spr_load(&spr_tuusin, 0, spr_tuusin_bin);
	spr_load(&spr_progress, 1, spr_progress_bin);
	spr_load(&spr_scroll, 1, spr_scroll_bin);
	spr_load(&spr_wait, 1, spr_wait_bin);
	spr_load(&spr_click, 1, spr_click_bin);
}

void ui_input(void) {
	scanKeys();
	in.down = keysDown();
	in.held = keysHeld();
	in.up = keysUp();
	bool was = in.touching;
	in.touching = in.held & KEY_TOUCH;
	if (in.touching) {
		touchRead(&in.touch);
		in.last_tx = in.touch.px;
		in.last_ty = in.touch.py;
	}
	in.touch_down = in.touching && !was;
	in.touch_up = !in.touching && was;
}

void ui_click_effect(int x, int y) {
	click_x = x;
	click_y = y;
	anim_start(&click_anim, &spr_click, 0, false);
}

void ui_frame(void) {
	if (click_anim.set && !click_anim.done) {
		anim_draw(&click_anim, click_x, click_y, 0);
		anim_step(&click_anim);
	}
}

// Blue title bar, with the same geometry as #errorTitle (22 px)
void ui_header(int scr, const char *title) {
	static const u32 grad[22] = {
		0x8ADDF9, 0x7DD8F8, 0x72D4F8, 0x68D0F7, 0x5ECCF7, 0x55C8F6, 0x4DC5F6, 0x46C2F5,
		0x40BFF5, 0x3ABCF4, 0x35B9F3, 0x31B6F2, 0x2DB3F1, 0x2AB0EF, 0x27ADEE, 0x24AAEC,
		0x22A7EA, 0x20A4E8, 0x1EA1E6, 0x1C9EE4, 0x1A9BE2, 0x1784C4,
	};
	for (int y = 0; y < 22; y++) gfx_fill(scr, 0, y, SCR_W, 1, HEX(grad[y]));
	gfx_fill(scr, 0, 0, SCR_W, 1, HEX(0xC5EEFC));
	font_draw(scr, FONT_M, 6, 3, title, COL_WHITE, ALIGN_LEFT);
}

void ui_error_header(int scr, const char *title) {
	gfx_image_tile_x(scr, IMG(img_e_u_bar_bin), 0, 0, SCR_W);
	font_draw(scr, FONT_M, 6, 3, title, COL_WHITE, ALIGN_LEFT);
}

static const Image *style_img(ButtonStyle s) {
	switch (s) {
	case BTN_WIDE28: return IMG(img_button_224x28_all_bin);
	case BTN_WIDE32: return IMG(img_button_224x32_all_bin);
	case BTN_VIOLET28: return IMG(img_violet_128x28_all_bin);
	default: return IMG(img_violet_128x40_all_bin);
	}
}

int ui_button_w(ButtonStyle s) { return style_img(s)->w; }
int ui_button_h(ButtonStyle s) { return style_img(s)->h / 2; }

void ui_button_draw(const Button *b, bool pressed, bool focused) {
	const Image *img = style_img(b->style);
	int w = img->w, h = img->h / 2;
	gfx_fill(BOTTOM, b->x, b->y, w, h, COL_WHITE);
	gfx_image_part(BOTTOM, img, 0, pressed ? h : 0, w, h, b->x, b->y);
	bool violet = b->style == BTN_VIOLET28 || b->style == BTN_VIOLET40;
	u16 col = violet ? COL_WHITE : COL_TITLE;
	if (b->disabled) col = violet ? HEX(0xA0D8E8) : COL_GRAY;
	FontId f = FONT_M;
	int lines = 1;
	for (const char *p = b->label; *p; p++) lines += *p == '\n';
	int ty = b->y + (h - lines * font_line_height(f)) / 2 + 1;
	font_draw(BOTTOM, f, b->x + w / 2, ty, b->label, col, ALIGN_CENTER);
	if (focused) {
		u16 fc = HEX(0xFF9900);
		gfx_rect(BOTTOM, b->x, b->y, w, h, fc);
		gfx_rect(BOTTOM, b->x + 1, b->y + 1, w - 2, h - 2, fc);
	}
}

void ui_group_init(ButtonGroup *g, Button *b, int n) {
	g->b = b;
	g->n = n;
	g->focus = 0;
	g->pressed = -1;
	g->keys_used = false;
	g->touch_only = false;
	while (g->focus < n && b[g->focus].disabled) g->focus++;
}

void ui_group_draw(ButtonGroup *g) {
	for (int i = 0; i < g->n; i++)
		ui_button_draw(&g->b[i], i == g->pressed, g->keys_used && i == g->focus);
}

static int hit(ButtonGroup *g, int x, int y) {
	for (int i = 0; i < g->n; i++) {
		Button *b = &g->b[i];
		if (b->disabled) continue;
		if (x >= b->x && x < b->x + ui_button_w(b->style) && y >= b->y && y < b->y + ui_button_h(b->style)) return i;
	}
	return -1;
}

static int move_focus(ButtonGroup *g, int dx, int dy) {
	Button *c = &g->b[g->focus];
	int cx = c->x + ui_button_w(c->style) / 2, cy = c->y + ui_button_h(c->style) / 2;
	int best = -1, bestd = 1 << 30;
	for (int i = 0; i < g->n; i++) {
		Button *b = &g->b[i];
		if (i == g->focus || b->disabled) continue;
		int bx = b->x + ui_button_w(b->style) / 2, by = b->y + ui_button_h(b->style) / 2;
		int ddx = bx - cx, ddy = by - cy;
		if (dx && (ddx * dx <= 0)) continue;
		if (dy && (ddy * dy <= 0)) continue;
		int d = dx ? abs(ddx) + abs(ddy) * 3 : abs(ddy) + abs(ddx) * 3;
		if (d < bestd) { bestd = d; best = i; }
	}
	return best;
}

int ui_group_update(ButtonGroup *g) {
	int fired = -1;
	if (in.touch_down) {
		int i = hit(g, in.touch.px, in.touch.py);
		if (i >= 0) {
			g->pressed = i;
			g->keys_used = false;
			snd_se(SE_CMN_SE_TOUCH);
			ui_group_draw(g);
		}
	} else if (in.touching && g->pressed >= 0) {
		if (hit(g, in.touch.px, in.touch.py) != g->pressed) {
			g->pressed = -1;
			ui_group_draw(g);
		}
	} else if (in.touch_up && g->pressed >= 0) {
		fired = g->pressed;
		g->pressed = -1;
		ui_click_effect(in.last_tx, in.last_ty);
	}

	if (fired < 0 && g->n && g->touch_only) {
		if (in.down & KEY_B) {
			for (int i = 0; i < g->n; i++)
				if (g->b[i].back && !g->b[i].disabled) { fired = i; break; }
		}
	} else if (fired < 0 && g->n) {
		int dir = -1;
		if (in.down & KEY_UP) dir = move_focus(g, 0, -1);
		else if (in.down & KEY_DOWN) dir = move_focus(g, 0, 1);
		else if (in.down & KEY_LEFT) dir = move_focus(g, -1, 0);
		else if (in.down & KEY_RIGHT) dir = move_focus(g, 1, 0);
		if (in.down & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
			if (!g->keys_used) { g->keys_used = true; dir = -1; }
			if (dir >= 0) g->focus = dir;
			snd_se(SE_KBD_SE_CURSOR);
			ui_group_draw(g);
		} else if ((in.down & KEY_A) && g->focus < g->n && !g->b[g->focus].disabled) {
			fired = g->focus;
			Button *b = &g->b[fired];
			ui_click_effect(b->x + ui_button_w(b->style) / 2, b->y + ui_button_h(b->style) / 2);
		} else if (in.down & KEY_B) {
			for (int i = 0; i < g->n; i++)
				if (g->b[i].back && !g->b[i].disabled) { fired = i; break; }
		}
	}
	if (fired >= 0) {
		snd_se(g->b[fired].back ? SE_CMN_SE_BACK : SE_CMN_SE_DECIDE);
		ui_group_draw(g);
		return g->b[fired].id;
	}
	return -1;
}

// Original dialog box (ued_dialog_BG), a bit higher up to leave
// room for the buttons below, as in the error pages.
void ui_dialog(const char *text) {
	gfx_blend_fill(BOTTOM, 0, 0, SCR_W, SCR_H, RGB(0, 0, 0), 7);
	gfx_bg_pack_transparent(BOTTOM, bg_dialog_bin, 0, 10);
	font_draw_box(BOTTOM, FONT_M, 24, 10, 208, 140, text, COL_BLUE, ALIGN_CENTER);
}

void ui_format_size(u32 bytes, char *buf, int len) {
	if (bytes >= 1024 * 1024) snprintf(buf, len, "%lu.%lu MB", bytes / (1024 * 1024), (bytes % (1024 * 1024)) * 10 / (1024 * 1024));
	else snprintf(buf, len, "%lu KB", (bytes + 1023) / 1024);
}
