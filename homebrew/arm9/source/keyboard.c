#include <stdio.h>
#include <string.h>
#include "keyboard.h"
#include "gfx.h"
#include "font.h"
#include "snd.h"
#include "text.h"
#include "ui.h"
#include "kbd_gen.h"
#include "bg_keyboard_bin.h"
#include "img_kbd_ascii_bin.h"
#include "img_kbd_euro_bin.h"
#include "img_kbd_picto_bin.h"
#include "img_kb_tab_a_bin.h"
#include "img_kb_tab_a_on_bin.h"
#include "img_kb_tab_euro_bin.h"
#include "img_kb_tab_euro_on_bin.h"
#include "img_kb_tab_sign_bin.h"
#include "img_kb_tab_sign_on_bin.h"
#include "img_kb_tab_picto_bin.h"
#include "img_kb_tab_picto_on_bin.h"
#include "img_kb_bar_l_bin.h"
#include "img_kb_bar_l_on_bin.h"
#include "img_kb_bar_r_bin.h"
#include "img_kb_bar_r_on_bin.h"

#define IMG(x) ((const Image *)(x))

// Bottom screen layout
#define FIELD_Y 8
#define FIELD_H 26
#define TAB_Y 40
#define KB_Y 60
#define BAR_Y 160

enum { MODE_ASCII, MODE_EURO, MODE_SIGN, MODE_PICTO, NMODES };
enum { K_BACKSPACE, K_ENTER, K_SPACE, K_SHIFT, K_CAPS };

typedef struct {
	const KbdRect *keys;
	int nkeys, nspecial;
	const u8 *img;
	const u8 *tab, *tab_on;
} Layout;

static const Layout layouts[NMODES] = {
	{ kbd_ascii_keys, 50, 5, img_kbd_ascii_bin, img_kb_tab_a_bin, img_kb_tab_a_on_bin },
	{ kbd_euro_keys, 55, 3, img_kbd_euro_bin, img_kb_tab_euro_bin, img_kb_tab_euro_on_bin },
	{ kbd_picto_keys, 58, 3, img_kbd_picto_bin, img_kb_tab_sign_bin, img_kb_tab_sign_on_bin },
	{ kbd_picto_keys, 58, 3, img_kbd_picto_bin, img_kb_tab_picto_bin, img_kb_tab_picto_on_bin },
};

static const char *k_title, *k_help;
static char *k_buf;
static int k_max, k_mode, k_pressed = -1, k_bar = -1, k_blink;
static bool k_shift, k_caps;

void kbd_setup(const char *title, const char *help, char *buf, int maxlen) {
	k_title = title;
	k_help = help;
	k_buf = buf;
	k_max = maxlen;
	k_mode = MODE_ASCII;
	k_shift = k_caps = false;
	k_pressed = k_bar = -1;
}

static const u16 *char_map(void) {
	switch (k_mode) {
	case MODE_ASCII: return k_shift ? kbd_map_ascii_shift : k_caps ? kbd_map_ascii_caps : kbd_map_ascii_normal;
	case MODE_EURO: return kbd_map_euro_normal;
	case MODE_SIGN: return kbd_map_sign;
	default: return kbd_map_picto;
	}
}

static int map_len(void) {
	switch (k_mode) {
	case MODE_ASCII: return 45;
	case MODE_EURO: return sizeof(kbd_map_euro_normal) / 2;
	case MODE_SIGN: return sizeof(kbd_map_sign) / 2;
	default: return sizeof(kbd_map_picto) / 2;
	}
}

static int utf8_encode(u16 c, char *out) {
	if (c < 0x80) { out[0] = c; return 1; }
	if (c < 0x800) { out[0] = 0xC0 | (c >> 6); out[1] = 0x80 | (c & 63); return 2; }
	out[0] = 0xE0 | (c >> 12); out[1] = 0x80 | ((c >> 6) & 63); out[2] = 0x80 | (c & 63);
	return 3;
}

// ------------------------------------------------------------ drawing
static void draw_top(void) {
	gfx_clear(TOP, COL_WHITE);
	ui_header(TOP, k_title);
	if (k_help) font_draw_box(TOP, FONT_S, 8, 30, 240, 0, k_help, COL_GRAY, ALIGN_LEFT);
	gfx_round_rect(TOP, 6, 70, 244, 112, HEX(0xF4FBFE), HEX(0xB8E6F7));
	font_draw_box(TOP, FONT_M, 12, 76, 232, 0, k_buf, COL_TITLE, ALIGN_LEFT);
}

static void draw_field(void) {
	gfx_round_rect(BOTTOM, 4, FIELD_Y, 248, FIELD_H, COL_WHITE, COL_BLUE);
	// if it does not fit, the end of the text is shown
	const char *s = k_buf;
	while (*s && font_text_width(FONT_M, s) > 232) {
		s++;
		while ((*s & 0xC0) == 0x80) s++;
	}
	font_draw(BOTTOM, FONT_M, 10, FIELD_Y + 5, s, COL_TITLE, ALIGN_LEFT);
	int cx = 10 + font_text_width(FONT_M, s);
	if (k_blink < 30) gfx_fill(BOTTOM, cx, FIELD_Y + 5, 2, 16, COL_BLUE);
}

static void draw_tabs(void) {
	for (int m = 0; m < NMODES; m++)
		gfx_image(BOTTOM, IMG(m == k_mode ? layouts[m].tab_on : layouts[m].tab), m * 64, TAB_Y);
}

static void key_label(const Layout *l, int i, char *out) {
	out[0] = 0;
	if (i < l->nspecial) return;	// the special keys already have their own graphic
	int ci = i - l->nspecial;
	if (ci >= map_len()) return;
	u16 c = char_map()[ci];
	if (c == ' ') return;
	out[utf8_encode(c, out)] = 0;
}

static void draw_keys(void) {
	const Layout *l = &layouts[k_mode];
	gfx_fill(BOTTOM, 0, KB_Y, SCR_W, 96, HEX(0xE8E8E8));
	gfx_image(BOTTOM, IMG(l->img), 0, KB_Y);
	for (int i = 0; i < l->nkeys; i++) {
		const KbdRect *r = &l->keys[i];
		if (i == K_ENTER) continue;	// the Enter key is not drawn: "Aceptar" is used
		bool on = i == k_pressed || (k_mode == MODE_ASCII && ((i == K_SHIFT && k_shift) || (i == K_CAPS && k_caps)));
		if (on) gfx_blend_fill(BOTTOM, r->x + 1, KB_Y + r->y + 1, r->w - 2, r->h - 2, HEX(0x0068F8), 7);
		char lbl[4];
		key_label(l, i, lbl);
		if (lbl[0]) font_draw(BOTTOM, FONT_M, r->x + r->w / 2, KB_Y + r->y + 1, lbl, on ? COL_WHITE : COL_TITLE, ALIGN_CENTER);
	}
}

static void draw_bar(void) {
	gfx_image(BOTTOM, IMG(k_bar == 0 ? img_kb_bar_l_on_bin : img_kb_bar_l_bin), 0, BAR_Y);
	gfx_image(BOTTOM, IMG(k_bar == 1 ? img_kb_bar_r_on_bin : img_kb_bar_r_bin), 128, BAR_Y);
	font_draw(BOTTOM, FONT_M, 64, BAR_Y + 8, txt(T_CANCEL), COL_WHITE, ALIGN_CENTER);
	font_draw(BOTTOM, FONT_M, 192, BAR_Y + 8, bmg(B_OK), COL_WHITE, ALIGN_CENTER);
}

void kbd_enter(void) {
	k_blink = 0;
	draw_top();
	gfx_bg_pack(BOTTOM, bg_keyboard_bin, 0, 0);
	draw_field();
	draw_tabs();
	draw_keys();
	draw_bar();
}

// ------------------------------------------------------------ input
static void type_char(u16 c) {
	char enc[4];
	int n = utf8_encode(c, enc), len = strlen(k_buf);
	if (len + n >= k_max) { snd_se(SE_KBD_SE_ERROR); return; }
	memcpy(k_buf + len, enc, n);
	k_buf[len + n] = 0;
	snd_se(SE_KBD_SE_KEY_INPUT);
}

static void backspace(void) {
	int len = strlen(k_buf);
	if (!len) { snd_se(SE_KBD_SE_NO_DELETE); return; }
	do len--; while (len > 0 && (k_buf[len] & 0xC0) == 0x80);
	k_buf[len] = 0;
	snd_se(SE_KBD_SE_KEY_INPUT);
}

static void press_key(int i) {
	const Layout *l = &layouts[k_mode];
	if (i < l->nspecial) {
		switch (i) {
		case K_BACKSPACE: backspace(); break;
		case K_SPACE: type_char(' '); break;
		case K_SHIFT: k_shift = !k_shift; snd_se(SE_KBD_SE_MOJI_KIRIKAE); break;
		case K_CAPS: k_caps = !k_caps; k_shift = false; snd_se(SE_KBD_SE_MOJI_KIRIKAE); break;
		}
		return;
	}
	int ci = i - l->nspecial;
	if (ci >= map_len() || char_map()[ci] == ' ') return;
	type_char(char_map()[ci]);
	if (k_shift) k_shift = false;
}

static int key_at(int x, int y) {
	const Layout *l = &layouts[k_mode];
	for (int i = 0; i < l->nkeys; i++) {
		const KbdRect *r = &l->keys[i];
		if (i == K_ENTER) continue;
		if (x >= r->x && x < r->x + r->w && y >= KB_Y + r->y && y < KB_Y + r->y + r->h) return i;
	}
	return -1;
}

int kbd_update(void) {
	int result = KBD_RUNNING;
	bool text_changed = false, keys_changed = false;

	if (in.touch_down) {
		int x = in.touch.px, y = in.touch.py;
		if (y >= TAB_Y && y < TAB_Y + 16) {
			int m = x / 64;
			if (m != k_mode && m < NMODES) {
				k_mode = m;
				k_shift = false;
				snd_se(SE_KBD_SE_MOJI_KIRIKAE);
				draw_tabs();
				keys_changed = true;
			}
		} else if (y >= BAR_Y) {
			k_bar = x < 128 ? 0 : 1;
			snd_se(SE_CMN_SE_TOUCH);
			draw_bar();
		} else {
			k_pressed = key_at(x, y);
			if (k_pressed >= 0) keys_changed = true;
		}
	} else if (in.touch_up) {
		if (k_pressed >= 0) {
			press_key(k_pressed);
			k_pressed = -1;
			keys_changed = text_changed = true;
		}
		if (k_bar >= 0) {
			result = k_bar ? KBD_ACCEPTED : KBD_CANCELLED;
			ui_click_effect(in.last_tx, in.last_ty);
			k_bar = -1;
			draw_bar();
		}
	} else if (in.touching && k_pressed >= 0 && key_at(in.touch.px, in.touch.py) != k_pressed) {
		k_pressed = -1;	// the stylus left the key: nothing is typed
		keys_changed = true;
	}

	// buttons: B deletes, START accepts, SELECT cancels, L/R switch keyboards
	if (in.down & KEY_B) { backspace(); text_changed = true; }
	if (in.down & KEY_START) result = KBD_ACCEPTED;
	if (in.down & KEY_SELECT) result = KBD_CANCELLED;
	if (in.down & (KEY_L | KEY_R)) {
		k_mode = (k_mode + ((in.down & KEY_R) ? 1 : NMODES - 1)) % NMODES;
		k_shift = false;
		snd_se(SE_KBD_SE_MOJI_KIRIKAE);
		draw_tabs();
		keys_changed = true;
	}

	if (result == KBD_ACCEPTED) snd_se(SE_KBD_SE_OK);
	else if (result == KBD_CANCELLED) snd_se(SE_CMN_SE_BACK);

	if (keys_changed) draw_keys();
	if (++k_blink >= 60) k_blink = 0;
	if (text_changed || k_blink == 0 || k_blink == 30) {
		draw_field();
		if (text_changed) draw_top();
	}
	return result;
}
