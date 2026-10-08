// Tienda DS: homebrew for Nintendo DS (and DS Lite) built with the DSi Shop assets.
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "keyboard.h"
#include "net.h"
#include "font.h"
#include "sprites.h"
#include "snd.h"
#include "store.h"
#include "text.h"
#include "torrent.h"
#include "qr.h"
#include <sys/stat.h>
#include "ui.h"
#include "bg_startup_top_bin.h"
#include "bg_startup_bottom_bin.h"
#include "img_e_bg_u_bin.h"
#include "img_e_bg_d_bin.h"

typedef enum {
	SC_BOOT, SC_MENU, SC_CATEGORIES, SC_LIST, SC_TITLE, SC_CONFIRM, SC_DOWNLOAD,
	SC_SETTINGS, SC_ERROR, SC_EXIT, SC_MESSAGE, SC_SOURCES, SC_CONNECT,
	SC_SOURCE_EDIT, SC_SOURCE_DELETE, SC_KEYBOARD, SC_TORRENT, SC_TOR_FETCH, SC_TOR_FILES, SC_QR,
} Scene;

static Scene scene = SC_BOOT, next_scene = SC_BOOT;
static bool entering = true;
static int frame;
static bool sd_ok;
static const char *rom_path;	// argv[0]: the path of this .nds (if the launcher provides it)

static Button buttons[8];
static ButtonGroup group;

// Current list (indices into titles[]); cat = -1 all, -2 recent
static int view[MAX_TITLES], nview, list_sel, list_top, list_cat;
static Scene list_from = SC_MENU;
static int cat_sel, cat_top;
static Title *cur;
static bool dl_torrent;	// the current download is from a torrent (otherwise it is cur)
static char msg_text[256];
static Scene msg_return;

#define ROW_H 44
#define ROWS 3
#define LIST_Y 24
#define LIST_W 224

static void go(Scene s) {
	next_scene = s;
}

static void qr_open(Scene ret);

static void set_buttons(int n) {
	ui_group_init(&group, buttons, n);
	ui_group_draw(&group);
}

static Button btn(int id, ButtonStyle st, int x, int y, const char *label, bool back) {
	Button b = { id, st, x, y, label, back, false };
	return b;
}

static const char *bmg_fmt(int id, const char *arg, char *buf, int len) {
	const char *s = bmg(id), *p = strstr(s, "{0}");
	if (!p) { snprintf(buf, len, "%s", s); return buf; }
	snprintf(buf, len, "%.*s%s%s", (int)(p - s), s, arg, p + 3);
	return buf;
}

static void blocks_text(u32 bytes, char *buf, int len) {
	u32 b = store_blocks(bytes);
	snprintf(buf, len, "%lu %s", b, bmg(b == 1 ? B_BLOCK : B_BLOCKS));
}

// ------------------------------------------------------------ top screen
static const char *shop_title(void) {
	return !store_merge && store_name[0] ? store_name : txt(T_SHOP_NAME);
}

// Truncates the text with "..." so it does not exceed maxw pixels
static void ellipsize(char *s, FontId f, int maxw) {
	if (font_text_width(f, s) <= maxw) return;
	int n = strlen(s);
	while (n > 0) {
		do n--; while (n > 0 && (s[n] & 0xC0) == 0x80);
		strcpy(s + n, "\xe2\x80\xa6");
		if (font_text_width(f, s) <= maxw) return;
	}
}

// With the stores merged, the store each title comes from is shown
static bool show_source(void) { return store_merge && nsources > 1; }

static char failed_names[160];	// stores that did not respond on the last load

static void top_home(void) {
	gfx_bg_pack(TOP, bg_startup_top_bin, 0, 0);
	font_draw(TOP, FONT_L, 128, 44, shop_title(), COL_BLUE, ALIGN_CENTER);
	if (failed_names[0]) {
		char buf[200];
		snprintf(buf, sizeof(buf), "%s %s", txt(T_OFFLINE), failed_names);
		font_draw(TOP, FONT_S, 128, 64, buf, COL_IMPORT, ALIGN_CENTER);
	} else if (nsources > 1) {
		char buf[64];
		if (store_merge) snprintf(buf, sizeof(buf), txt(T_N_STORES), nsources);
		else snprintf(buf, sizeof(buf), "%s", sources[cur_source].name);
		font_draw(TOP, FONT_S, 128, 64, buf, COL_GRAY, ALIGN_CENTER);
	}
	if (!ntitles) {
		font_draw_box(TOP, FONT_M, 8, 124, 240, 0, txt(T_EMPTY_STORE), COL_TEXT, ALIGN_CENTER);
		return;
	}
	const char *msg = !store_merge && store_message[0] ? store_message : txt(T_WELCOME);
	font_draw_box(TOP, FONT_M, 8, 120, 240, 40, msg, COL_TEXT, ALIGN_CENTER);
	char buf[64], sz[24];
	snprintf(buf, sizeof(buf), txt(T_N_TITLES), ntitles);
	font_draw(TOP, FONT_S, 128, 162, buf, COL_GRAY, ALIGN_CENTER);
	u64 fb = store_free_bytes();
	blocks_text(fb > 0xFFFFFFFFull ? 0xFFFFFFFF : (u32)fb, sz, sizeof(sz));
	snprintf(buf, sizeof(buf), "%s: %s", txt(T_FREE_SPACE), sz);
	font_draw(TOP, FONT_S, 128, 175, buf, COL_GRAY, ALIGN_CENTER);
}

static void top_title(const Title *t, bool allow_network) {
	gfx_clear(TOP, COL_WHITE);
	ui_header(TOP, txt(T_HELP_TITLE));
	if (!t) return;
	gfx_round_rect(TOP, 8, 30, 72, 72, HEX(0xF4FBFE), HEX(0xB8E6F7));
	if (t->has_icon) gfx_icon4bpp(TOP, t->icon, t->pal, 12, 34, 2);
	int y = 32;
	for (int i = 0; i < 3; i++) {
		if (!t->title[i][0]) continue;
		font_draw(TOP, i ? FONT_S : FONT_M, 88, y, t->title[i], i ? COL_TEXT : COL_TITLE, ALIGN_LEFT);
		y += i ? 13 : 17;
	}
	char buf[96], sz[24], bl[24];
	ui_format_size(t->size, sz, sizeof(sz));
	blocks_text(t->size, bl, sizeof(bl));
	snprintf(buf, sizeof(buf), "%s: %s (%s)", txt(T_SIZE), bl, sz);
	int sy = y > 80 ? y + 2 : 84;
	font_draw(TOP, FONT_S, 88, sy, buf, COL_BLUE, ALIGN_LEFT);
	if (show_source()) {
		snprintf(buf, sizeof(buf), "%s: %s", txt(T_SOURCE), sources[t->source].name);
		font_draw(TOP, FONT_S, 88, sy + 13, buf, COL_GRAY, ALIGN_LEFT);
	}
	static char desc[512];
	gfx_fill(TOP, 8, 108, 240, 1, HEX(0xDDDDDD));
	if (store_read_desc(t, desc, sizeof(desc), allow_network))
		font_draw_box(TOP, FONT_S, 10, 113, 236, 0, desc, COL_TEXT, ALIGN_LEFT);
	else {
		snprintf(buf, sizeof(buf), "%s: %s", txt(T_FILE), t->file);
		font_draw_box(TOP, FONT_S, 10, 113, 236, 0, buf, COL_GRAY, ALIGN_LEFT);
		bool code_ok = t->gamecode[0] != 0;
		for (const char *c = t->gamecode; *c; c++)
			if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'))) code_ok = false;
		if (code_ok) font_draw(TOP, FONT_S, 10, 126, t->gamecode, COL_GRAY, ALIGN_LEFT);
	}
}

// ------------------------------------------------------------ errors
enum { ERR_SD, ERR_NET };
static int err_kind;
static TextId err_code;
static char err_msg[400];

static void error_set(TextId code, const char *msg, int kind) {
	err_code = code;
	err_kind = kind;
	snprintf(err_msg, sizeof(err_msg), "%s", msg);
}

static void start_loading(void);

// ------------------------------------------------------------ startup
static AnimPlayer wait_anim;

static void boot_enter(void) {
	gfx_bg_pack(TOP, bg_startup_top_bin, 0, 0);
	font_draw(TOP, FONT_L, 128, 44, txt(T_SHOP_NAME), COL_BLUE, ALIGN_CENTER);
	gfx_bg_pack(BOTTOM, bg_startup_bottom_bin, 127, 0);
	font_draw_box(BOTTOM, FONT_M, 8, 16, 240, 48, txt(T_LOADING), COL_BLUE, ALIGN_CENTER);
	anim_start(&wait_anim, &spr_wait, 0, true);
	snd_bgm(true);
	frame = 0;
}

static void boot_update(void) {
	anim_draw(&wait_anim, 128, 180, 0);
	anim_step(&wait_anim);
	if (frame == 3 && sd_ok) sources_load();
	if (frame >= 100) {
		if (!sd_ok) { error_set(T_CODE_SD, txt(T_NO_SD), ERR_SD); go(SC_ERROR); return; }
		// no stores, or separate with several: one must be chosen/added
		if (!nsources || (!store_merge && nsources > 1)) { go(SC_SOURCES); return; }
		start_loading();
	}
}

// ------------------------------------------------------------ main menu
enum { ID_SHOP = 1, ID_NEWEST, ID_SETTINGS, ID_QUIT, ID_BACK, ID_DOWNLOAD, ID_YES, ID_NO,
	ID_MUSIC, ID_RETRY, ID_OK, ID_CANCEL, ID_STORES, ID_EDIT, ID_EDIT_NAME, ID_EDIT_URL,
	ID_SAVE, ID_DELETE, ID_MERGE, ID_TORRENTS, ID_TORRENT, ID_TOR_URL, ID_QR };

static void menu_enter(void) {
	top_home();
	gfx_clear(BOTTOM, COL_WHITE);
	ui_header(BOTTOM, bmg(B_MAIN_MENU));
	// with torrents enabled there is one more button and they are packed a bit closer
	int y0 = store_torrents ? 26 : 36, dy = store_torrents ? 34 : 40;
	buttons[0] = btn(ID_SHOP, BTN_WIDE32, 16, y0, txt(T_START_SHOPPING), false);
	buttons[1] = btn(ID_NEWEST, BTN_WIDE32, 16, y0 + dy, txt(T_NEWEST), false);
	buttons[2] = btn(ID_SETTINGS, BTN_WIDE32, 16, y0 + dy * (store_torrents ? 3 : 2), txt(T_SETTINGS), false);
	buttons[3] = btn(ID_QUIT, BTN_VIOLET28, 0, 164, bmg(B_QUIT), true);
	buttons[4] = btn(ID_STORES, BTN_VIOLET28, 128, 164, txt(T_CHANGE_STORE), false);
	buttons[5] = btn(ID_TORRENT, BTN_WIDE32, 16, y0 + dy * 2, txt(T_TOR_DOWNLOAD), false);
	buttons[0].disabled = buttons[1].disabled = !ntitles;
	set_buttons(store_torrents ? 6 : 5);
}

static int cmp_title(const void *a, const void *b) {
	return strcasecmp(titles[*(const int *)a].title[0], titles[*(const int *)b].title[0]);
}

static int cmp_newest(const void *a, const void *b) {
	time_t ta = titles[*(const int *)a].mtime, tb = titles[*(const int *)b].mtime;
	return ta < tb ? 1 : ta > tb ? -1 : cmp_title(a, b);
}

static void open_list(int cat) {
	list_cat = cat;
	nview = 0;
	for (int i = 0; i < ntitles; i++)
		if (cat < 0 || titles[i].category == cat) view[nview++] = i;
	qsort(view, nview, sizeof(int), cat == -2 ? cmp_newest : cmp_title);
	list_sel = list_top = 0;
	list_from = scene;
	go(SC_LIST);
}

static void menu_update(void) {
	switch (ui_group_update(&group)) {
	case ID_SHOP:
		if (ncategories > 1) { cat_sel = cat_top = 0; go(SC_CATEGORIES); }
		else open_list(-1);
		break;
	case ID_NEWEST: open_list(-2); break;
	case ID_SETTINGS: go(SC_SETTINGS); break;
	case ID_QUIT: go(SC_EXIT); break;
	case ID_STORES: go(SC_SOURCES); break;
	case ID_TORRENT: go(SC_TORRENT); break;
	}
}

// ------------------------------------------------------------ lists
// Generic list with 44 px rows, the original scroll arrows and
// selection by touch or D-pad. Returns the opened row or -1; -2 = back.
typedef void (*RowDraw)(int idx, int y, bool selected);

static int list_update(int n, int *sel, int *top, RowDraw draw, void (*on_select)(int)) {
	int old_sel = *sel, old_top = *top;
	int opened = -1;
	bool scrolled = false, invalid = false;

	if (in.down & KEY_UP) { if (*sel > 0) (*sel)--; else invalid = true; }
	if (in.down & KEY_DOWN) { if (*sel < n - 1) (*sel)++; else invalid = true; }
	if (in.down & (KEY_L | KEY_LEFT)) { if (*top > 0) { *top -= ROWS; *sel -= ROWS; scrolled = true; } else invalid = true; }
	if (in.down & (KEY_R | KEY_RIGHT)) { if (*top + ROWS < n) { *top += ROWS; *sel += ROWS; scrolled = true; } else invalid = true; }
	if (in.down & KEY_A && n) opened = *sel;

	if (in.touch_down) {
		int tx = in.touch.px, ty = in.touch.py;
		if (tx >= LIST_W && ty >= LIST_Y && ty < LIST_Y + 60) {
			if (*top > 0) { *top -= ROWS; *sel -= ROWS; scrolled = true; } else invalid = true;
		} else if (tx >= LIST_W && ty >= LIST_Y + 72 && ty < LIST_Y + ROWS * ROW_H) {
			if (*top + ROWS < n) { *top += ROWS; *sel += ROWS; scrolled = true; } else invalid = true;
		} else if (tx < LIST_W && ty >= LIST_Y && ty < LIST_Y + ROWS * ROW_H) {
			int r = *top + (ty - LIST_Y) / ROW_H;
			if (r < n) {
				*sel = r;
				opened = r;
				ui_click_effect(tx, ty);
			}
		}
	}
	if (*top < 0) *top = 0;
	if (*sel < 0) *sel = 0;
	if (*sel > n - 1) *sel = n - 1;
	if (*sel < *top) *top = *sel;
	if (*sel >= *top + ROWS) *top = *sel - ROWS + 1;

	if (opened >= 0) snd_se(SE_CMN_SE_DECIDE);
	else if (scrolled || *top != old_top) snd_se(SE_SHP_SE_SCROLL);
	else if (*sel != old_sel) snd_se(SE_KBD_SE_CURSOR);
	else if (invalid) snd_se(SE_SHP_SE_SCROLL_INVALID);

	if (*sel != old_sel || *top != old_top || entering) {
		gfx_fill(BOTTOM, 0, LIST_Y, SCR_W, ROWS * ROW_H, COL_WHITE);
		for (int r = 0; r < ROWS && *top + r < n; r++) draw(*top + r, LIST_Y + r * ROW_H, *top + r == *sel);
		// page number between the arrows
		char buf[16];
		snprintf(buf, sizeof(buf), "%d/%d", *top / ROWS + 1, n ? (n + ROWS - 1) / ROWS : 1);
		font_draw(BOTTOM, FONT_S, LIST_W + 16, LIST_Y + 60, buf, COL_GRAY, ALIGN_CENTER);
		if (on_select && n) on_select(*sel);
	}

	// arrows: 0/3 enabled, 2/5 disabled
	spr_cell(&spr_scroll, *top > 0 ? 0 : 2, LIST_W + 16, LIST_Y + 30, 0);
	spr_cell(&spr_scroll, *top + ROWS < n ? 3 : 5, LIST_W + 16, LIST_Y + 102, 0);

	return opened;
}

static void row_frame(int y, bool sel) {
	if (sel) gfx_round_rect(BOTTOM, 2, y + 1, LIST_W - 4, ROW_H - 2, HEX(0xDDF4FD), COL_BLUE);
	else gfx_fill(BOTTOM, 8, y + ROW_H - 1, LIST_W - 16, 1, HEX(0xE4E4E4));
}

static void title_row(int idx, int y, bool sel) {
	Title *t = &titles[view[idx]];
	row_frame(y, sel);
	if (t->has_icon) gfx_icon4bpp(BOTTOM, t->icon, t->pal, 8, y + 6, 1);
	else gfx_round_rect(BOTTOM, 8, y + 6, 32, 32, HEX(0xEEEEEE), HEX(0xCCCCCC));
	font_draw(BOTTOM, FONT_M, 48, y + 6, t->title[0], COL_TITLE, ALIGN_LEFT);
	char bl[24], line[128];
	blocks_text(t->size, bl, sizeof(bl));
	if (show_source()) snprintf(line, sizeof(line), "%s \302\267 %s", bl, sources[t->source].name);
	else snprintf(line, sizeof(line), "%s", bl);
	ellipsize(line, FONT_S, LIST_W - 10 - font_text_width(FONT_S, txt(T_FREE)) - 6 - 48);
	font_draw(BOTTOM, FONT_S, 48, y + 26, line, COL_GRAY, ALIGN_LEFT);
	font_draw(BOTTOM, FONT_S, LIST_W - 10, y + 26, txt(T_FREE), COL_BLUE, ALIGN_RIGHT);
}

static void title_selected(int idx) {
	top_title(&titles[view[idx]], false);
}

static void list_enter(void) {
	gfx_clear(BOTTOM, COL_WHITE);
	const char *h = list_cat == -2 ? txt(T_NEWEST) : list_cat <= 0 ? txt(T_ALL) : categories[list_cat].name;
	if (list_cat == 0 && ncategories > 1) h = txt(T_ALL);
	ui_header(BOTTOM, h);
	buttons[0] = btn(ID_BACK, BTN_VIOLET28, 0, 164, bmg(B_BACK), true);
	set_buttons(1);
	group.touch_only = true;
	if (!nview) top_home();
}

static void list_update_scene(void) {
	int r = list_update(nview, &list_sel, &list_top, title_row, title_selected);
	int b = ui_group_update(&group);
	if (r >= 0) { cur = &titles[view[r]]; go(SC_TITLE); }
	else if (b == ID_BACK || r == -2) {
		if (r == -2) snd_se(SE_CMN_SE_BACK);
		go(list_from);
	}
}

// Categories: "Todos los titulos" + each subfolder (+ "sin categoria" if any)
static int cat_ids[MAX_CATEGORIES + 1], ncat_ids;

static void cat_row(int idx, int y, bool sel) {
	row_frame(y, sel);
	int c = cat_ids[idx];
	const char *name = c == -1 ? txt(T_ALL) : c == 0 ? txt(T_OTHER) : categories[c].name;
	int count = c == -1 ? ntitles : categories[c].count;
	font_draw(BOTTOM, FONT_M, 14, y + 8, name, COL_TITLE, ALIGN_LEFT);
	char buf[48];
	snprintf(buf, sizeof(buf), txt(T_N_TITLES), count);
	font_draw(BOTTOM, FONT_S, 14, y + 26, buf, COL_GRAY, ALIGN_LEFT);
}

static void cat_enter(void) {
	top_home();
	ncat_ids = 0;
	cat_ids[ncat_ids++] = -1;
	for (int c = 1; c < ncategories; c++)
		if (categories[c].count) cat_ids[ncat_ids++] = c;
	if (categories[0].count) cat_ids[ncat_ids++] = 0;
	gfx_clear(BOTTOM, COL_WHITE);
	ui_header(BOTTOM, txt(T_CATEGORIES));
	buttons[0] = btn(ID_BACK, BTN_VIOLET28, 0, 164, bmg(B_BACK), true);
	set_buttons(1);
	group.touch_only = true;
}

static void cat_update(void) {
	int r = list_update(ncat_ids, &cat_sel, &cat_top, cat_row, NULL);
	int b = ui_group_update(&group);
	if (r >= 0) open_list(cat_ids[r]);
	else if (b == ID_BACK || r == -2) {
		if (r == -2) snd_se(SE_CMN_SE_BACK);
		go(SC_MENU);
	}
}

// ------------------------------------------------------------ title page
static void title_enter(void) {
	top_title(cur, true);
	gfx_clear(BOTTOM, COL_WHITE);
	ui_header(BOTTOM, cur->title[0]);
	gfx_round_rect(BOTTOM, 16, 32, 40, 40, HEX(0xF4FBFE), HEX(0xB8E6F7));
	if (cur->has_icon) gfx_icon4bpp(BOTTOM, cur->icon, cur->pal, 20, 36, 1);
	font_draw_box(BOTTOM, FONT_M, 64, 32, 180, 0, cur->title[0], COL_TITLE, ALIGN_LEFT);
	if (cur->title[1][0]) font_draw_box(BOTTOM, FONT_S, 64, 50, 180, 0, cur->title[1], COL_TEXT, ALIGN_LEFT);
	char bl[24];
	blocks_text(cur->size, bl, sizeof(bl));
	font_draw(BOTTOM, FONT_S, 64, 66, bl, COL_GRAY, ALIGN_LEFT);
	font_draw(BOTTOM, FONT_L, 240, 86, txt(T_FREE), COL_BLUE, ALIGN_RIGHT);
	bool inst = store_installed(cur);
	TextId lbl = store_partial(cur) ? T_RESUME_DL : inst ? T_REDOWNLOAD : T_DOWNLOAD;
	buttons[0] = btn(ID_DOWNLOAD, BTN_WIDE32, 16, 116, txt(lbl), false);
	buttons[1] = btn(ID_BACK, BTN_VIOLET28, 0, 164, bmg(B_BACK), true);
	set_buttons(2);
}

static void title_update(void) {
	switch (ui_group_update(&group)) {
	case ID_DOWNLOAD: dl_torrent = false; go(store_partial(cur) ? SC_DOWNLOAD : SC_CONFIRM); break;
	case ID_BACK: go(SC_LIST); break;
	}
}

static void confirm_enter(void) {
	ui_dialog(txt(store_installed(cur) ? T_ALREADY : T_CONFIRM_DL));
	buttons[0] = btn(ID_YES, BTN_VIOLET28, 0, 164, bmg(B_YES), false);
	buttons[1] = btn(ID_NO, BTN_VIOLET28, 128, 164, bmg(B_NO), true);
	set_buttons(2);
}

static void confirm_update(void) {
	switch (ui_group_update(&group)) {
	case ID_YES: go(SC_DOWNLOAD); break;
	case ID_NO: go(SC_TITLE); break;
	}
}

// Message with an "Aceptar" button that returns to the given scene
static void message(const char *text, Scene ret) {
	snprintf(msg_text, sizeof(msg_text), "%s", text);
	msg_return = ret;
	go(SC_MESSAGE);
}

static void message_enter(void) {
	ui_dialog(msg_text);
	buttons[0] = btn(ID_OK, BTN_VIOLET28, 64, 164, bmg(B_OK), true);
	set_buttons(1);
}

static void message_update(void) {
	if (ui_group_update(&group) == ID_OK) go(msg_return);
}

// ------------------------------------------------------------ download
typedef struct {
	int walk[4], thr[3], back[4];
	int x0, x1, thr_x[3], thr_y[3];
} Runner;

// Cells and positions taken from ued_progress_bar1.bncl
static const Runner runners[4] = {
	{ { 21, 22, 23, 24 }, { 25, 26, 27 }, { 28, 29, 30, 31 }, 9, 96, { 101, 106, 111 }, { 88, 85, 89 } },	// Mario
	{ { 32, 33, 34, 35 }, { 36, 37, 38 }, { 39, 40, 41, 42 }, 247, 160, { 154, 149, 144 }, { 88, 85, 89 } },	// Luigi
	{ { 43, 44, 45, 46 }, { 47, 48, 49 }, { 50, 51, 52, 53 }, 8, 96, { 101, 108, 111 }, { 87, 85, 89 } },	// Peach
	{ { 54, 55, 56, 57 }, { 58, 59, 60 }, { 61, 62, 63, 64 }, 247, 160, { 155, 150, 145 }, { 88, 86, 89 } },	// Toad
};

#define WALK_T 44
#define THROW_T 18
#define ROUND_T (WALK_T * 2 + THROW_T)

static int dl_progress, dl_round, dl_t, dl_phase, dl_done_t;
static AnimPlayer box_anim;
static bool box_intro;
static int last_pct;
static bool dl_retrying;


static TorMeta tor_meta;
static int tor_file;
static char tor_url[256], tor_dest[256];

static void draw_runner(const Runner *r, int t, int delay) {
	t -= delay;
	if (t < 0) return;
	bool left = r->x0 < 128;
	if (t < WALK_T) {
		int x = r->x0 + (r->x1 - r->x0) * t / WALK_T;
		spr_cell(&spr_progress, r->walk[(t / 6) & 3], x, 91, 1);
	} else if (t < WALK_T + THROW_T) {
		int k = (t - WALK_T) / 6;
		if (t == WALK_T) snd_se(SE_SHP_SE_DL_TAMA);
		if (t == WALK_T + THROW_T - 1) snd_se(SE_SHP_SE_DL_DATA);
		spr_cell(&spr_progress, r->thr[k], r->thr_x[k], r->thr_y[k], 1);
	} else if (t < ROUND_T) {
		int tt = t - WALK_T - THROW_T;
		int x = r->x1 + (r->x0 - r->x1) * tt / WALK_T;
		spr_cell_flip(&spr_progress, r->back[(tt / 6) & 3], x, 94, 1, left);
	}
}

static void dl_percent(int pct) {
	char buf[16];
	gfx_fill(BOTTOM, 96, 132, 64, 18, COL_WHITE);
	snprintf(buf, sizeof(buf), txt(T_DL_REMAINING), pct);
	font_draw(BOTTOM, FONT_L, 128, 132, buf, COL_BLUE, ALIGN_CENTER);
}

static void top_download(void) {
	gfx_clear(TOP, COL_WHITE);
	ui_header(TOP, txt(T_SHOP_NAME));
	if (dl_torrent) {
		const char *name = tor_meta.files[tor_file].path;
		const char *base = strrchr(name, '/');
		font_draw_box(TOP, FONT_M, 8, 32, 240, 0, base ? base + 1 : name, COL_TITLE, ALIGN_LEFT);
		if (tor_meta.nfiles > 1) font_draw_box(TOP, FONT_S, 8, 52, 240, 0, tor_meta.name, COL_TEXT, ALIGN_LEFT);
		font_draw_box(TOP, FONT_M, 8, 100, 240, 60, txt(T_DONT_TURN_OFF), COL_IMPORT, ALIGN_CENTER);
		return;
	}
	gfx_round_rect(TOP, 8, 34, 40, 40, HEX(0xF4FBFE), HEX(0xB8E6F7));
	if (cur->has_icon) gfx_icon4bpp(TOP, cur->icon, cur->pal, 12, 38, 1);
	font_draw_box(TOP, FONT_M, 56, 36, 192, 0, cur->title[0], COL_TITLE, ALIGN_LEFT);
	if (cur->title[1][0]) font_draw_box(TOP, FONT_S, 56, 54, 192, 0, cur->title[1], COL_TEXT, ALIGN_LEFT);
	font_draw_box(TOP, FONT_M, 8, 100, 240, 60, txt(T_DONT_TURN_OFF), COL_IMPORT, ALIGN_CENTER);
}

// Torrent status on the top screen (every half second)
static void tor_status(void) {
	TorStats st;
	tor_stats(&st);
	char buf[96], a[24], b[24], r[24];
	gfx_fill(TOP, 0, 150, SCR_W, 42, COL_WHITE);
	ui_format_size(st.done > 0xFFFFFFFFull ? 0xFFFFFFFF : (u32)st.done, a, sizeof(a));
	ui_format_size(st.size > 0xFFFFFFFFull ? 0xFFFFFFFF : (u32)st.size, b, sizeof(b));
	snprintf(buf, sizeof(buf), "%s / %s", a, b);
	font_draw(TOP, FONT_S, 128, 152, buf, COL_BLUE, ALIGN_CENTER);
	if (!strcmp(st.status, "prepare")) snprintf(buf, sizeof(buf), "%s", txt(T_TOR_PREPARE));
	else if (st.peers) {
		ui_format_size(st.rate, r, sizeof(r));
		snprintf(buf, sizeof(buf), txt(T_TOR_PEERS), st.peers, st.known, r);
	} else snprintf(buf, sizeof(buf), "%s", txt(!strcmp(st.status, "tracker") ? T_TOR_TRACKER : T_TOR_SEARCH));
	font_draw(TOP, FONT_S, 128, 166, buf, COL_GRAY, ALIGN_CENTER);
}

static void download_enter(void) {
	Scene back = dl_torrent ? SC_MENU : SC_TITLE;
	if (dl_torrent) {
		if (!tor_start(&tor_meta, tor_file, tor_dest)) {
			snd_se(SE_SHP_SE_WARNING_PAGE);
			char buf[256];
			snprintf(buf, sizeof(buf), "%s\n\n%s", txt(T_DL_FAILED), tor_error());
			message(buf, back);
			return;
		}
	} else if (store_free_bytes() < (u64)cur->size + 256 * 1024 && !store_installed(cur)) {
		snd_se(SE_SHP_SE_WARNING_PAGE);
		message(txt(T_NO_SPACE), SC_TITLE);
		return;
	}
	if (!dl_torrent && !store_copy_begin(cur)) {
		snd_se(SE_SHP_SE_WARNING_PAGE);
		char buf[64], code[32];
		snprintf(buf, sizeof(buf), "%s\n%s", txt(T_DL_FAILED), bmg_fmt(B_ERROR_CODE, txt(T_CODE_COPY), code, sizeof(code)));
		message(buf, SC_TITLE);
		return;
	}
	top_download();
	gfx_clear(BOTTOM, COL_WHITE);
	ui_header(BOTTOM, txt(dl_torrent ? T_DOWNLOADING : store_copy_resumed() ? T_RESUMING : T_DOWNLOADING));
	buttons[0] = btn(ID_CANCEL, BTN_VIOLET28, 0, 164, txt(T_CANCEL), true);
	set_buttons(1);
	dl_progress = 0;
	dl_round = 0;
	dl_t = 0;
	dl_phase = 0;
	last_pct = -1;
	dl_retrying = false;
	anim_start(&box_anim, &spr_progress, 0, false);
	box_intro = true;
	snd_se(SE_CMN_SE_PROCESSING);
}

static void download_update(void) {
	spr_cell(&spr_tuusin, (frame / 30) & 3, 240, 11, 0);

	if (dl_phase == 0) {
		int p = dl_torrent ? tor_step() : store_copy_step();
		if (p < 0 && dl_torrent) {
			snd_se(SE_SHP_SE_WARNING_PAGE);
			char buf[256];
			snprintf(buf, sizeof(buf), "%s\n\n%s", txt(T_DL_FAILED), tor_error());
			message(buf, SC_MENU);
			return;
		}
		if (p < 0) {
			snd_se(SE_SHP_SE_WARNING_PAGE);
			char buf[192], code[32];
			snprintf(buf, sizeof(buf), "%s\n%s%s%s", txt(T_DL_FAILED),
				bmg_fmt(B_ERROR_CODE, txt(T_CODE_COPY), code, sizeof(code)),
				store_copy_kept_partial ? "\n\n" : "", store_copy_kept_partial ? txt(T_DL_CAN_RESUME) : "");
			message(buf, SC_TITLE);
			return;
		}
		dl_progress = p;
		int pct = p / 10;
		if (pct != last_pct) { dl_percent(pct); last_pct = pct; }
		if (dl_torrent && (frame % 30) == 0) tor_status();
		bool retrying = !dl_torrent && store_copy_retrying();
		if (retrying != dl_retrying) {
			dl_retrying = retrying;
			gfx_fill(BOTTOM, 0, 150, SCR_W, 13, COL_WHITE);
			if (retrying) font_draw(BOTTOM, FONT_S, 128, 150, txt(T_RECONNECTING), COL_GRAY, ALIGN_CENTER);
		}
		if (p >= 1000) {
			dl_phase = 1;
			dl_done_t = 0;
			anim_start(&box_anim, &spr_progress, 1, false);
		}
		if (ui_group_update(&group) == ID_CANCEL) {
			if (dl_torrent) {
				// what was downloaded is kept: the same .torrent resumes it
				tor_stop(true);
				char buf[160];
				snprintf(buf, sizeof(buf), "%s\n\n%s", txt(T_DL_CANCELLED), txt(T_DL_CAN_RESUME));
				message(buf, SC_MENU);
				return;
			}
			store_copy_abort(false);
			message(txt(T_DL_CANCELLED), SC_TITLE);
			return;
		}
	}

	// box: appears, fills with the progress, closes and gets wrapped
	if (box_intro) {
		anim_draw(&box_anim, 128, 96, 2);
		anim_step(&box_anim);
		if (box_anim.done) box_intro = false;
	} else if (dl_phase == 0) {
		spr_cell(&spr_progress, dl_progress / 100, 128, 96, 2);
	} else {
		dl_done_t++;
		if (!box_anim.done) {
			anim_draw(&box_anim, 128, 96, 2);
			anim_step(&box_anim);
		} else {
			int k = (dl_done_t - 30) / 6;
			spr_cell(&spr_progress, k < 0 ? 73 : k > 3 ? 19 : 16 + k, 128, 96, 2);
		}
		if (dl_done_t == 30) snd_se(SE_SHP_SE_COMPLETED);
		if (dl_done_t == 60) {
			gfx_fill(BOTTOM, 0, 0, SCR_W, 22, COL_WHITE);
			ui_header(BOTTOM, dl_torrent ? tor_meta.name : cur->title[0]);
			gfx_fill(BOTTOM, 0, 124, SCR_W, 40, COL_WHITE);
			font_draw(BOTTOM, FONT_M, 128, 124, txt(T_DL_DONE), COL_BLUE, ALIGN_CENTER);
			char buf[128];
			snprintf(buf, sizeof(buf), "%s %s", txt(T_DL_SAVED), dl_torrent ? tor_dest : store_copy_dest_path());
			font_draw_box(BOTTOM, FONT_S, 8, 142, 240, 0, buf, COL_TEXT, ALIGN_CENTER);
			gfx_fill(BOTTOM, 0, 164, SCR_W, 28, COL_WHITE);
			buttons[0] = btn(ID_OK, BTN_VIOLET28, 64, 164, bmg(B_OK), true);
			set_buttons(1);
			gfx_fill(TOP, 0, 96, SCR_W, 96, COL_WHITE);
			font_draw_box(TOP, FONT_M, 8, 100, 240, 60, txt(T_DL_DONE), COL_BLUE, ALIGN_CENTER);
		}
		if (dl_done_t > 60 && ui_group_update(&group) == ID_OK) go(dl_torrent ? SC_MENU : SC_TITLE);
	}

	// characters carrying the data to the box (Mario and Luigi, then Peach and Toad)
	if (dl_phase == 0 && !box_intro) {
		int pair = (dl_round & 1) * 2;
		draw_runner(&runners[pair], dl_t, 0);
		draw_runner(&runners[pair + 1], dl_t, 10);
		if (++dl_t >= ROUND_T + 10) { dl_t = 0; dl_round++; }
	}
}

// ------------------------------------------------------------ settings
static char lbl_music[64], lbl_merge[96], lbl_torrents[96];
static bool merge_before;

static void settings_labels(void) {
	snprintf(lbl_merge, sizeof(lbl_merge), "%s: %s", txt(T_STORES), txt(store_merge ? T_MERGED : T_SEPARATE));
	snprintf(lbl_music, sizeof(lbl_music), "%s: %s", txt(T_MUSIC), txt(snd_music_enabled() ? T_ON : T_OFF));
	snprintf(lbl_torrents, sizeof(lbl_torrents), "%s: %s", txt(T_TORRENTS), txt(store_torrents ? T_ON : T_OFF));
}

static void settings_enter(void) {
	top_home();
	gfx_clear(BOTTOM, COL_WHITE);
	ui_header(BOTTOM, txt(T_SETTINGS));
	settings_labels();
	buttons[0] = btn(ID_MUSIC, BTN_WIDE28, 16, 26, lbl_music, false);
	buttons[1] = btn(ID_MERGE, BTN_WIDE28, 16, 58, lbl_merge, false);
	buttons[2] = btn(ID_TORRENTS, BTN_WIDE28, 16, 90, lbl_torrents, false);
	buttons[3] = btn(ID_BACK, BTN_VIOLET28, 0, 164, bmg(B_BACK), true);
	set_buttons(4);
	// downloads always go next to the store's .nds
	int y = font_draw_box(BOTTOM, FONT_S, 16, 122, 224, 0, txt(T_DOWNLOADS_TO), COL_GRAY, ALIGN_LEFT);
	font_draw_box(BOTTOM, FONT_S, 16, y, 224, 0, store_dest(), COL_BLUE, ALIGN_LEFT);
	if (entering) merge_before = store_merge;
}

static void settings_update(void) {
	switch (ui_group_update(&group)) {
	case ID_MUSIC:
		snd_set_music_enabled(!snd_music_enabled());
		settings_labels();
		ui_group_draw(&group);
		break;
	case ID_MERGE:
		store_merge = !store_merge;
		settings_labels();
		ui_group_draw(&group);
		break;
	case ID_TORRENTS:
		store_torrents = !store_torrents;
		settings_labels();
		ui_group_draw(&group);
		break;
	case ID_BACK:
		store_save_config();
		if (store_merge == merge_before) go(SC_MENU);
		else if (store_merge || nsources == 1) start_loading();
		else go(SC_SOURCES);
		break;
	}
}

// ------------------------------------------------------------ error (original E_01 page)
static void error_enter(void) {
	gfx_clear(TOP, COL_WHITE);
	ui_error_header(TOP, txt(T_SHOP_NAME));
	char code[32];
	font_draw(TOP, FONT_M, 8, 30, bmg_fmt(B_ERROR_CODE, txt(err_code), code, sizeof(code)), COL_RED, ALIGN_LEFT);
	font_draw_box(TOP, FONT_M, 8, 52, 240, 0, err_msg, COL_RED, ALIGN_LEFT);

	gfx_clear(BOTTOM, COL_WHITE);
	gfx_image_tile_x(BOTTOM, (const Image *)img_e_bg_u_bin, 0, 30, SCR_W);
	gfx_image_tile_x(BOTTOM, (const Image *)img_e_bg_d_bin, 0, 105, SCR_W);
	font_draw_box(BOTTOM, FONT_M, 4, 40, 248, 64, bmg(B_ERR_SEE_TOP), COL_RED, ALIGN_CENTER);
	buttons[0] = btn(ID_RETRY, BTN_VIOLET28, 0, 164, bmg(B_RETRY), false);
	if (err_kind == ERR_NET)
		buttons[1] = btn(ID_STORES, BTN_VIOLET28, 128, 164, txt(T_CHANGE_STORE), true);
	else
		buttons[1] = btn(ID_QUIT, BTN_VIOLET28, 128, 164, bmg(B_QUIT), true);
	set_buttons(2);
	snd_se(SE_SHP_SE_WARNING_PAGE);
}

static void error_update(void) {
	switch (ui_group_update(&group)) {
	case ID_RETRY:
		if (err_kind == ERR_NET) { start_loading(); break; }
		sd_ok = store_init(rom_path);
		go(SC_BOOT);
		break;
	case ID_STORES: go(SC_SOURCES); break;
	case ID_QUIT: go(SC_EXIT); break;
	}
}

// ------------------------------------------------------------ choose store
static int src_sel, src_top;
static bool sources_dirty;	// one was added, changed or deleted: a reload is needed

static void source_row(int idx, int y, bool sel) {
	row_frame(y, sel);
	if (idx == nsources) {	// last row: add
		font_draw(BOTTOM, FONT_L, 14, y + 12, "+", COL_BLUE, ALIGN_LEFT);
		font_draw(BOTTOM, FONT_M, 32, y + 14, txt(T_ADD_STORE), COL_BLUE, ALIGN_LEFT);
		return;
	}
	font_draw(BOTTOM, FONT_M, 14, y + 8, sources[idx].name, COL_TITLE, ALIGN_LEFT);
	font_draw(BOTTOM, FONT_S, 14, y + 26, sources[idx].url, COL_GRAY, ALIGN_LEFT);
}

static void sources_enter(void) {
	gfx_bg_pack(TOP, bg_startup_top_bin, 0, 0);
	font_draw(TOP, FONT_L, 128, 44, txt(T_SHOP_NAME), COL_BLUE, ALIGN_CENTER);
	TextId help = !nsources ? T_NO_STORES : store_merge ? T_SOURCES_HELP_MERGED : T_SOURCES_HELP;
	font_draw_box(TOP, FONT_M, 8, 124, 240, 0, txt(help), COL_TEXT, ALIGN_CENTER);
	gfx_clear(BOTTOM, COL_WHITE);
	ui_header(BOTTOM, txt(store_merge || !nsources ? T_STORES : T_CHOOSE_STORE));
	if (store_merge && nsources) buttons[0] = btn(ID_BACK, BTN_VIOLET28, 0, 164, bmg(B_BACK), true);
	else buttons[0] = btn(ID_QUIT, BTN_VIOLET28, 0, 164, bmg(B_QUIT), true);
	buttons[1] = btn(ID_EDIT, BTN_VIOLET28, 128, 164, txt(T_EDIT), false);
	if (src_sel < 0 || src_sel > nsources) src_sel = cur_source;
	buttons[1].disabled = src_sel >= nsources;
	set_buttons(2);
	group.touch_only = true;
	src_top = (src_sel / ROWS) * ROWS;
}

static void source_selected(int idx) {
	bool can = idx < nsources;
	if (buttons[1].disabled == can) {
		buttons[1].disabled = !can;
		ui_group_draw(&group);
	}
}

// Editing a source (edit_idx = -1: new)
static int edit_idx;
static char edit_name[48], edit_url[128], kbd_backup[256];
static char *kbd_target;
static Scene kbd_return;

static void open_editor(int idx) {
	edit_idx = idx;
	if (idx >= 0) {
		snprintf(edit_name, sizeof(edit_name), "%s", sources[idx].name);
		snprintf(edit_url, sizeof(edit_url), "%s", sources[idx].url);
	} else {
		edit_name[0] = 0;
		snprintf(edit_url, sizeof(edit_url), "http://");
	}
	go(SC_SOURCE_EDIT);
}

static void sources_update(void) {
	int r = list_update(nsources + 1, &src_sel, &src_top, source_row, source_selected);
	int b = ui_group_update(&group);
	if ((in.down & KEY_X) && src_sel < nsources) { snd_se(SE_CMN_SE_DECIDE); b = ID_EDIT; }
	if (b == ID_EDIT) { open_editor(src_sel); return; }
	if (r == nsources) {
		if (nsources >= MAX_SOURCES) { message(txt(T_TOO_MANY_STORES), SC_SOURCES); return; }
		open_editor(-1);
		return;
	}
	if (r >= 0 && store_merge) {
		// all merged: the list is only for managing them
		open_editor(r);
	} else if (r >= 0) {
		cur_source = r;
		start_loading();
	} else if (b == ID_BACK) {
		if (sources_dirty || !ntitles) start_loading();
		else go(SC_MENU);
	} else if (b == ID_QUIT || r == -2) {
		if (r == -2) snd_se(SE_CMN_SE_BACK);
		go(SC_EXIT);
	}
}

// "Prefix: value" label truncated with "..." to fit in a button
static void fit_label(char *out, int len, const char *prefix, const char *value, int maxw) {
	snprintf(out, len, "%s: %s", prefix, value[0] ? value : "-");
	if (font_text_width(FONT_M, out) <= maxw) return;
	int n = strlen(out);
	while (n > 0) {
		do n--; while (n > 0 && (out[n] & 0xC0) == 0x80);
		snprintf(out + n, len - n, "\xe2\x80\xa6");
		if (font_text_width(FONT_M, out) <= maxw) return;
	}
}

static char lbl_name[96], lbl_url[160];

static void source_edit_enter(void) {
	gfx_clear(TOP, COL_WHITE);
	ui_header(TOP, txt(edit_idx >= 0 ? T_EDIT_STORE : T_ADD_STORE));
	font_draw_box(TOP, FONT_S, 8, 30, 240, 0, txt(T_STORE_HELP), COL_TEXT, ALIGN_LEFT);
	gfx_round_rect(TOP, 6, 120, 244, 64, HEX(0xF4FBFE), HEX(0xB8E6F7));
	font_draw(TOP, FONT_M, 12, 126, edit_name[0] ? edit_name : "-", COL_TITLE, ALIGN_LEFT);
	font_draw_box(TOP, FONT_S, 12, 146, 232, 0, edit_url, COL_BLUE, ALIGN_LEFT);

	gfx_clear(BOTTOM, COL_WHITE);
	ui_header(BOTTOM, txt(edit_idx >= 0 ? T_EDIT_STORE : T_ADD_STORE));
	fit_label(lbl_name, sizeof(lbl_name), txt(T_STORE_NAME), edit_name, 210);
	fit_label(lbl_url, sizeof(lbl_url), "URL", edit_url, 210);
	// on DSi there is one more button to read the address from a QR
	bool qr = qr_available();
	buttons[0] = btn(ID_EDIT_NAME, BTN_WIDE32, 16, qr ? 28 : 32, lbl_name, false);
	buttons[1] = btn(ID_EDIT_URL, BTN_WIDE32, 16, qr ? 62 : 72, lbl_url, false);
	buttons[2] = btn(ID_BACK, BTN_VIOLET28, 0, 164, bmg(B_BACK), true);
	buttons[3] = btn(ID_SAVE, BTN_VIOLET28, 128, 164, txt(T_SAVE), false);
	int n = 4;
	if (qr) buttons[n++] = btn(ID_QR, BTN_WIDE32, 16, 96, txt(T_SCAN_QR), false);
	if (edit_idx >= 0) buttons[n++] = btn(ID_DELETE, BTN_WIDE28, 16, qr ? 132 : 120, txt(T_DELETE_STORE), false);
	set_buttons(n);
}

static void open_keyboard(char *target, int len, TextId title) {
	kbd_target = target;
	snprintf(kbd_backup, sizeof(kbd_backup), "%s", target);
	kbd_setup(txt(title), txt(title == T_STORE_URL ? T_URL_HELP : title == T_TOR_URL ? T_TOR_URL_HELP : T_NAME_HELP), target, len);
	kbd_return = scene;
	go(SC_KEYBOARD);
}

static bool host_of(const char *url, char *out, int len) {
	const char *h = url + 7;
	int n = strcspn(h, ":/");
	if (n <= 0 || n >= len) return false;
	memcpy(out, h, n);
	out[n] = 0;
	return true;
}

static void source_edit_update(void) {
	switch (ui_group_update(&group)) {
	case ID_EDIT_NAME: open_keyboard(edit_name, sizeof(edit_name), T_STORE_NAME); break;
	case ID_EDIT_URL: open_keyboard(edit_url, sizeof(edit_url), T_STORE_URL); break;
	case ID_BACK: go(SC_SOURCES); break;
	case ID_DELETE: go(SC_SOURCE_DELETE); break;
	case ID_QR: qr_open(SC_SOURCE_EDIT); break;
	case ID_SAVE: {
		// without a scheme http:// is assumed; https is not supported
		char url[128], host[96];
		if (!strncasecmp(edit_url, "https://", 8)) { message(txt(T_NO_HTTPS), SC_SOURCE_EDIT); break; }
		if (strncasecmp(edit_url, "http://", 7)) snprintf(url, sizeof(url), "http://%s", edit_url);
		else snprintf(url, sizeof(url), "%s", edit_url);
		if (!host_of(url, host, sizeof(host))) { message(txt(T_BAD_URL), SC_SOURCE_EDIT); break; }
		int i = edit_idx >= 0 ? edit_idx : nsources++;
		snprintf(sources[i].url, sizeof(sources[i].url), "%s", url);
		snprintf(sources[i].name, sizeof(sources[i].name), "%s", edit_name[0] ? edit_name : host);
		src_sel = i;
		sources_dirty = true;
		if (!sources_save()) { message(txt(T_SAVE_FAILED), SC_SOURCES); break; }
		go(SC_SOURCES);
		break;
	}
	}
}

static void source_delete_enter(void) {
	char buf[160];
	snprintf(buf, sizeof(buf), "%s\n\n%s", txt(T_CONFIRM_DELETE), sources[edit_idx].name);
	ui_dialog(buf);
	buttons[0] = btn(ID_YES, BTN_VIOLET28, 0, 164, bmg(B_YES), false);
	buttons[1] = btn(ID_NO, BTN_VIOLET28, 128, 164, bmg(B_NO), true);
	set_buttons(2);
}

static void source_delete_update(void) {
	switch (ui_group_update(&group)) {
	case ID_YES:
		for (int i = edit_idx; i < nsources - 1; i++) sources[i] = sources[i + 1];
		nsources--;
		if (cur_source >= edit_idx && cur_source > 0) cur_source--;
		src_sel = edit_idx < nsources ? edit_idx : nsources;
		sources_dirty = true;
		sources_save();
		go(SC_SOURCES);
		break;
	case ID_NO: go(SC_SOURCE_EDIT); break;
	}
}

static void keyboard_update(void) {
	int r = kbd_update();
	if (r == KBD_CANCELLED) {
		strcpy(kbd_target, kbd_backup);
		go(kbd_return);
	} else if (r == KBD_ACCEPTED) {
		go(kbd_return);
	}
}

// Catalog loading: the SD card and, with Wi-Fi, each remote store.
// With the stores merged all are loaded and the failing ones are skipped;
// separately only the chosen one, and a failure leads to the error page.
static int load_list[MAX_SOURCES], nload, load_pos, conn_state;
static bool wifi_failed;
static int nfailed, last_fail_src;
static TextId last_fail_code;
static int last_fail_msg;

static void start_loading(void) {
	catalog_clear();
	nload = 0;
	if (store_merge) for (int i = 0; i < nsources; i++) load_list[nload++] = i;
	else load_list[nload++] = cur_source;
	sources_dirty = false;
	go(SC_CONNECT);
}

static void connect_status(void) {
	int i = load_list[load_pos < nload ? load_pos : nload - 1];
	gfx_bg_pack(TOP, bg_startup_top_bin, 0, 0);
	font_draw(TOP, FONT_L, 128, 44, txt(T_SHOP_NAME), COL_BLUE, ALIGN_CENTER);
	char buf[64];
	if (nload > 1) {
		snprintf(buf, sizeof(buf), "%d / %d", load_pos + 1, nload);
		font_draw(TOP, FONT_S, 128, 120, buf, COL_GRAY, ALIGN_CENTER);
	}
	font_draw(TOP, FONT_M, 128, 134, sources[i].name, COL_TEXT, ALIGN_CENTER);
	font_draw(TOP, FONT_S, 128, 152, sources[i].url, COL_GRAY, ALIGN_CENTER);
}

static void connect_enter(void) {
	load_pos = 0;
	conn_state = 0;
	wifi_failed = false;
	nfailed = 0;
	failed_names[0] = 0;
	connect_status();
	gfx_bg_pack(BOTTOM, bg_startup_bottom_bin, 127, 0);
	font_draw_box(BOTTOM, FONT_M, 8, 16, 240, 48, bmg(B_CONNECTING), COL_BLUE, ALIGN_CENTER);
	anim_start(&wait_anim, &spr_wait, 0, true);
	snd_se(SE_CMN_SE_CONNECTING);
	frame = 0;
}

static void source_failed(TextId code, int msg) {
	int i = load_list[load_pos];
	net_log("tienda %s: error %s (%s)", sources[i].name, txt(code), net_error);
	nfailed++;
	last_fail_src = i;
	last_fail_code = code;
	last_fail_msg = msg;
	int n = strlen(failed_names);
	snprintf(failed_names + n, sizeof(failed_names) - n, "%s%s", n ? ", " : "", sources[i].name);
	load_pos++;
	conn_state = 0;
}

static void loading_done(void) {
	bool all_failed = nfailed && !ntitles;
	if (nfailed && (!store_merge || all_failed)) {
		char buf[400];
		if (store_merge && nfailed > 1)
			snprintf(buf, sizeof(buf), "%s\n\n%s", bmg(last_fail_msg), failed_names);
		else
			snprintf(buf, sizeof(buf), "%s\n\n%s (%s)", bmg(last_fail_msg), sources[last_fail_src].url, net_error);
		error_set(last_fail_code, buf, ERR_NET);
		go(SC_ERROR);
		return;
	}
	snd_se(SE_SHP_SE_LOADED);
	go(SC_MENU);
}

static void connect_update(void) {
	anim_draw(&wait_anim, 128, 180, 0);
	anim_step(&wait_anim);
	if (frame < 2) return;	// let the screen show before blocking
	if (load_pos >= nload) { loading_done(); return; }
	int i = load_list[load_pos];
	spr_cell(&spr_tuusin, (frame / 30) & 3, 240, 11, 0);
	switch (conn_state) {
	case 0:
		connect_status();
		if (wifi_failed) { source_failed(T_CODE_WIFI, B_ERR_NO_AP); break; }
		if (!net_wifi_ready()) { net_wifi_start(); conn_state = 1; break; }
		if (!catalog_fetch_begin(i)) source_failed(T_CODE_SERVER, B_ERR_SERVER);
		else conn_state = 2;
		break;
	case 1:
		switch (net_wifi_poll()) {
		case WIFI_CONNECTING: break;
		case WIFI_FAILED: wifi_failed = true; source_failed(T_CODE_WIFI, B_ERR_NO_AP); break;
		case WIFI_OK: conn_state = 0; break;
		}
		break;
	case 2: {
		int r = catalog_fetch_poll();
		if (r == CAT_OK) { load_pos++; conn_state = 0; }
		else if (r == CAT_ERR_DATA) source_failed(T_CODE_DATA, B_ERR_GENERIC);
		else if (r < 0) source_failed(T_CODE_SERVER, B_ERR_SERVER);
		break;
	}
	}
}

// ------------------------------------------------------------ download from torrent (experimental)
// The .torrent link is typed by hand (or via a QR on DSi); the console
// downloads it over HTTP and then fetches the content from peers with torrent.c.
static char lbl_tor[160];
static int tor_sel, tor_top, tf_state;
static Http tor_http;

static void torrent_enter(void) {
	if (!tor_url[0]) snprintf(tor_url, sizeof(tor_url), "%s", store_torrent_url[0] ? store_torrent_url : "http://");
	gfx_clear(TOP, COL_WHITE);
	ui_header(TOP, txt(T_TOR_DOWNLOAD));
	int y = font_draw_box(TOP, FONT_S, 8, 28, 240, 0, txt(T_TOR_HELP), COL_TEXT, ALIGN_LEFT);
	if (y < 130) y = 130;
	gfx_round_rect(TOP, 6, y, 244, 186 - y, HEX(0xF4FBFE), HEX(0xB8E6F7));
	font_draw_box(TOP, FONT_S, 12, y + 4, 232, 0, tor_url, COL_BLUE, ALIGN_LEFT);

	gfx_clear(BOTTOM, COL_WHITE);
	ui_header(BOTTOM, txt(T_TOR_DOWNLOAD));
	fit_label(lbl_tor, sizeof(lbl_tor), "URL", tor_url, 210);
	buttons[0] = btn(ID_TOR_URL, BTN_WIDE32, 16, 36, lbl_tor, false);
	buttons[1] = btn(ID_BACK, BTN_VIOLET28, 0, 164, bmg(B_BACK), true);
	buttons[2] = btn(ID_DOWNLOAD, BTN_VIOLET28, 128, 164, txt(T_DOWNLOAD), false);
	buttons[2].disabled = strlen(tor_url) <= 8;
	// the camera only exists on DSi: not shown on DS and DS Lite
	buttons[3] = btn(ID_QR, BTN_WIDE32, 16, 80, txt(T_SCAN_QR), false);
	set_buttons(qr_available() ? 4 : 3);
}

static void torrent_update(void) {
	switch (ui_group_update(&group)) {
	case ID_TOR_URL: open_keyboard(tor_url, sizeof(tor_url), T_TOR_URL); break;
	case ID_QR: qr_open(SC_TORRENT); break;
	case ID_BACK: go(SC_MENU); break;
	case ID_DOWNLOAD: {
		char host[96];
		if (!strncasecmp(tor_url, "https://", 8)) { message(txt(T_NO_HTTPS), SC_TORRENT); break; }
		if (strncasecmp(tor_url, "http://", 7)) {
			char tmp[sizeof(tor_url)];
			snprintf(tmp, sizeof(tmp), "http://%s", tor_url);
			strcpy(tor_url, tmp);
		}
		if (!host_of(tor_url, host, sizeof(host))) { message(txt(T_BAD_URL), SC_TORRENT); break; }
		snprintf(store_torrent_url, sizeof(store_torrent_url), "%s", tor_url);
		store_save_config();
		go(SC_TOR_FETCH);
		break;
	}
	}
}

static void tor_fetch_enter(void) {
	gfx_bg_pack(TOP, bg_startup_top_bin, 0, 0);
	font_draw(TOP, FONT_L, 128, 44, txt(T_SHOP_NAME), COL_BLUE, ALIGN_CENTER);
	font_draw_box(TOP, FONT_S, 8, 130, 240, 0, tor_url, COL_GRAY, ALIGN_CENTER);
	gfx_bg_pack(BOTTOM, bg_startup_bottom_bin, 127, 0);
	font_draw_box(BOTTOM, FONT_M, 8, 16, 240, 48, txt(T_TOR_FETCHING), COL_BLUE, ALIGN_CENTER);
	anim_start(&wait_anim, &spr_wait, 0, true);
	snd_se(SE_CMN_SE_CONNECTING);
	tf_state = 0;
	frame = 0;
}

// Name a torrent file is saved under: no folders or characters
// FAT does not allow, in the store's folder.
static void tor_dest_path(int i) {
	const char *name = tor_meta.files[i].path;
	const char *base = strrchr(name, '/');
	char clean[96];
	snprintf(clean, sizeof(clean), "%s", base ? base + 1 : name);
	for (char *c = clean; *c; c++)
		if (strchr("\\:*?\"<>|", *c) || (u8)*c < 32) *c = '_';
	if (!clean[0]) snprintf(clean, sizeof(clean), "torrent.bin");
	snprintf(tor_dest, sizeof(tor_dest), "%s%s", store_dest(), clean);
}

static void tor_choose(int i) {
	tor_dest_path(i);
	char part[260];
	struct stat st;
	snprintf(part, sizeof(part), "%s.part", tor_dest);
	u64 have = stat(part, &st) ? 0 : (u64)st.st_size, size = tor_meta.files[i].size;
	if (store_free_bytes() + have < size + 256 * 1024) {
		snd_se(SE_SHP_SE_WARNING_PAGE);
		message(txt(T_NO_SPACE), SC_TORRENT);
		return;
	}
	tor_file = i;
	dl_torrent = true;
	go(SC_DOWNLOAD);
}

static void tor_fetch_failed(const char *why) {
	snd_se(SE_SHP_SE_WARNING_PAGE);
	char buf[256];
	snprintf(buf, sizeof(buf), "%s\n\n%s", why, net_error);
	message(buf, SC_TORRENT);
}

static void tor_fetch_update(void) {
	anim_draw(&wait_anim, 128, 180, 0);
	anim_step(&wait_anim);
	spr_cell(&spr_tuusin, (frame / 30) & 3, 240, 11, 0);
	if (frame < 2) return;	// let the screen show before blocking
	switch (tf_state) {
	case 0:
		if (net_wifi_ready()) { tf_state = 2; break; }
		net_wifi_start();
		tf_state = 1;
		break;
	case 1:
		switch (net_wifi_poll()) {
		case WIFI_CONNECTING: break;
		case WIFI_FAILED: tor_fetch_failed(bmg(B_ERR_NO_AP)); break;
		case WIFI_OK: tf_state = 2; break;
		}
		break;
	case 2: {
		// "http://host:port/path?x" -> base "http://host:port" and path "/path?x"
		char base[128];
		const char *path = strchr(tor_url + 7, '/');
		int n = path ? path - tor_url : (int)strlen(tor_url);
		if (n >= (int)sizeof(base)) { message(txt(T_BAD_URL), SC_TORRENT); break; }
		memcpy(base, tor_url, n);
		base[n] = 0;
		const int max = 512 * 1024;
		u8 *buf = malloc(max);
		if (!buf) { tor_fetch_failed(txt(T_TOR_FETCH_FAILED)); break; }
		memset(&tor_http, 0, sizeof(tor_http));
		int len = http_fetch(&tor_http, base, path ? path : "/", buf, max, 60 * 20);
		http_close(&tor_http);
		if (len <= 0) { free(buf); tor_fetch_failed(txt(T_TOR_FETCH_FAILED)); break; }
		tor_free(&tor_meta);
		const char *err = tor_load(buf, len, &tor_meta);
		free(buf);
		if (err) {
			snd_se(SE_SHP_SE_WARNING_PAGE);
			char msg[200];
			snprintf(msg, sizeof(msg), "%s\n%s", txt(T_TOR_BAD), err);
			message(msg, SC_TORRENT);
			break;
		}
		net_log("torrent %s: %d archivos, %d trackers utiles", tor_meta.name, tor_meta.nfiles, tor_meta.ntrackers);
		if (tor_meta.nfiles == 1) { tor_choose(0); break; }
		// the first .nds is preselected
		tor_sel = 0;
		for (int i = 0; i < tor_meta.nfiles; i++) {
			const char *dot = strrchr(tor_meta.files[i].path, '.');
			if (dot && !strcasecmp(dot, ".nds")) { tor_sel = i; break; }
		}
		tor_top = (tor_sel / ROWS) * ROWS;
		go(SC_TOR_FILES);
		break;
	}
	}
}

static void tor_file_row(int idx, int y, bool sel) {
	TorFile *f = &tor_meta.files[idx];
	row_frame(y, sel);
	const char *base = strrchr(f->path, '/');
	char name[128], line[160], sz[24];
	snprintf(name, sizeof(name), "%s", base ? base + 1 : f->path);
	ellipsize(name, FONT_M, LIST_W - 20);
	font_draw(BOTTOM, FONT_M, 10, y + 6, name, COL_TITLE, ALIGN_LEFT);
	ui_format_size(f->size > 0xFFFFFFFFull ? 0xFFFFFFFF : (u32)f->size, sz, sizeof(sz));
	if (base) snprintf(line, sizeof(line), "%s \302\267 %.*s", sz, (int)(base - f->path), f->path);
	else snprintf(line, sizeof(line), "%s", sz);
	ellipsize(line, FONT_S, LIST_W - 20);
	font_draw(BOTTOM, FONT_S, 10, y + 26, line, COL_GRAY, ALIGN_LEFT);
}

static void tor_files_enter(void) {
	gfx_clear(TOP, COL_WHITE);
	ui_header(TOP, txt(T_TOR_DOWNLOAD));
	font_draw_box(TOP, FONT_M, 8, 32, 240, 0, tor_meta.name, COL_TITLE, ALIGN_LEFT);
	char buf[96], sz[24];
	ui_format_size(tor_meta.total > 0xFFFFFFFFull ? 0xFFFFFFFF : (u32)tor_meta.total, sz, sizeof(sz));
	snprintf(buf, sizeof(buf), "%d \302\267 %s", tor_meta.nfiles, sz);
	font_draw(TOP, FONT_S, 8, 70, buf, COL_GRAY, ALIGN_LEFT);
	gfx_clear(BOTTOM, COL_WHITE);
	ui_header(BOTTOM, txt(T_TOR_FILES));
	buttons[0] = btn(ID_BACK, BTN_VIOLET28, 0, 164, bmg(B_BACK), true);
	set_buttons(1);
	group.touch_only = true;
}

static void tor_files_update(void) {
	int r = list_update(tor_meta.nfiles, &tor_sel, &tor_top, tor_file_row, NULL);
	int b = ui_group_update(&group);
	if (r >= 0) tor_choose(r);
	else if (b == ID_BACK || r == -2) {
		if (r == -2) snd_se(SE_CMN_SE_BACK);
		go(SC_TORRENT);
	}
}

// ------------------------------------------------------------ read a QR (DSi only)
// Used for a store address ("URL" or "Name|URL") and for a
// .torrent link.
static Scene qr_return;
static bool qr_ok;

static void qr_open(Scene ret) {
	qr_return = ret;
	go(SC_QR);
}

static void qr_enter(void) {
	gfx_clear(TOP, HEX(0x000000));
	gfx_clear(BOTTOM, COL_WHITE);
	ui_header(BOTTOM, txt(T_SCAN_QR));
	font_draw_box(BOTTOM, FONT_M, 8, 40, 240, 0, txt(T_QR_HELP), COL_TEXT, ALIGN_CENTER);
	buttons[0] = btn(ID_BACK, BTN_VIOLET28, 0, 164, bmg(B_BACK), true);
	set_buttons(1);
	gfx_present();
	qr_ok = qr_start();
	if (!qr_ok) {
		snd_se(SE_SHP_SE_WARNING_PAGE);
		message(txt(T_QR_FAILED), qr_return);
	}
}

static void qr_update(void) {
	static char text[300];
	int r = qr_step(text, sizeof(text));
	if (ui_group_update(&group) == ID_BACK) {
		qr_stop();
		go(qr_return);
		return;
	}
	if (r < 0) {
		qr_stop();
		snd_se(SE_SHP_SE_WARNING_PAGE);
		message(txt(T_QR_FAILED), qr_return);
		return;
	}
	if (r == 0) return;
	// "Name|URL" or just "URL", with no surrounding spaces
	char *name = NULL, *url = text;
	char *bar = strchr(text, '|');
	if (bar) { *bar = 0; name = text; url = bar + 1; }
	while (*url == ' ' || *url == '\n' || *url == '\r') url++;
	for (int n = strlen(url); n && (url[n - 1] == ' ' || url[n - 1] == '\n' || url[n - 1] == '\r'); n--) url[n - 1] = 0;
	if (!strncasecmp(url, "https://", 8)) {
		qr_stop();
		snd_se(SE_SHP_SE_WARNING_PAGE);
		message(txt(T_NO_HTTPS), qr_return);
		return;
	}
	if (strncasecmp(url, "http://", 7)) {	// a different QR: warn and keep looking
		gfx_fill(BOTTOM, 0, 112, SCR_W, 40, COL_WHITE);
		font_draw_box(BOTTOM, FONT_S, 8, 116, 240, 0, txt(T_QR_UNKNOWN), COL_IMPORT, ALIGN_CENTER);
		return;
	}
	qr_stop();
	snd_se(SE_CMN_SE_DECIDE);
	if (qr_return == SC_TORRENT) {
		snprintf(tor_url, sizeof(tor_url), "%s", url);
	} else {
		snprintf(edit_url, sizeof(edit_url), "%s", url);
		if (name && *name) snprintf(edit_name, sizeof(edit_name), "%s", name);
	}
	go(qr_return);
}

// ------------------------------------------------------------ exit
static Scene exit_return = SC_MENU;

static void exit_enter(void) {
	ui_dialog(txt(T_EXIT_CONFIRM));
	buttons[0] = btn(ID_YES, BTN_VIOLET28, 0, 164, bmg(B_YES), false);
	buttons[1] = btn(ID_NO, BTN_VIOLET28, 128, 164, bmg(B_NO), true);
	set_buttons(2);
}

static void exit_update(void) {
	switch (ui_group_update(&group)) {
	case ID_YES:
		snd_bgm(false);
		for (int i = 0; i < 20; i++) swiWaitForVBlank();
		exit(0);
		break;
	case ID_NO: go(exit_return); break;
	}
}

// ------------------------------------------------------------
int main(int argc, char **argv) {
	rom_path = argc > 0 ? argv[0] : NULL;
	defaultExceptionHandler();
	gfx_init();
	font_init();
	text_init();
	ui_init();
	snd_init();
	sd_ok = store_init(rom_path);

	while (1) {
		swiWaitForVBlank();
		gfx_present();
		spr_end();
		net_tick();
		ui_input();
		spr_begin();

		if (next_scene != scene || entering) {
			if (next_scene == SC_EXIT) exit_return = scene;
			scene = next_scene;
			entering = true;
			switch (scene) {
			case SC_BOOT: boot_enter(); break;
			case SC_MENU: menu_enter(); break;
			case SC_CATEGORIES: cat_enter(); break;
			case SC_LIST: list_enter(); break;
			case SC_TITLE: title_enter(); break;
			case SC_CONFIRM: confirm_enter(); break;
			case SC_DOWNLOAD: download_enter(); break;
			case SC_SETTINGS: settings_enter(); break;
			case SC_ERROR: error_enter(); break;
			case SC_EXIT: exit_enter(); break;
			case SC_MESSAGE: message_enter(); break;
			case SC_SOURCES: sources_enter(); break;
			case SC_CONNECT: connect_enter(); break;
			case SC_SOURCE_EDIT: source_edit_enter(); break;
			case SC_SOURCE_DELETE: source_delete_enter(); break;
			case SC_KEYBOARD: kbd_enter(); break;
			case SC_TORRENT: torrent_enter(); break;
			case SC_TOR_FETCH: tor_fetch_enter(); break;
			case SC_TOR_FILES: tor_files_enter(); break;
			case SC_QR: qr_enter(); break;
			}
			// download_enter may redirect to a message
			if (next_scene != scene) continue;
		}

		switch (scene) {
		case SC_BOOT: boot_update(); break;
		case SC_MENU: menu_update(); break;
		case SC_CATEGORIES: cat_update(); break;
		case SC_LIST: list_update_scene(); break;
		case SC_TITLE: title_update(); break;
		case SC_CONFIRM: confirm_update(); break;
		case SC_DOWNLOAD: download_update(); break;
		case SC_SETTINGS: settings_update(); break;
		case SC_ERROR: error_update(); break;
		case SC_EXIT: exit_update(); break;
		case SC_MESSAGE: message_update(); break;
		case SC_SOURCES: sources_update(); break;
		case SC_CONNECT: connect_update(); break;
		case SC_SOURCE_EDIT: source_edit_update(); break;
		case SC_SOURCE_DELETE: source_delete_update(); break;
		case SC_KEYBOARD: keyboard_update(); break;
		case SC_TORRENT: torrent_update(); break;
		case SC_TOR_FETCH: tor_fetch_update(); break;
		case SC_TOR_FILES: tor_files_update(); break;
		case SC_QR: qr_update(); break;
		}
		entering = false;
		ui_frame();
		frame++;
	}
}
