#include <string.h>
#include "font.h"
#include "gfx.h"
#include "font_s_bin.h"
#include "font_m_bin.h"
#include "font_l_bin.h"

typedef struct {
	u8 cellw, cellh, ascent, linefeed;
	u16 nglyphs;
	u8 cellbytes, pad;
} FontHdr;

typedef struct {
	u16 code;
	s8 left;
	u8 width, advance, pad;
	u16 pad2;
} Glyph;

typedef struct {
	const FontHdr *h;
	const Glyph *g;
	const u8 *bits;
} Font;

static Font fonts[3];

static void load(Font *f, const u8 *data) {
	f->h = (const FontHdr *)data;
	f->g = (const Glyph *)(data + sizeof(FontHdr));
	f->bits = (const u8 *)(f->g + f->h->nglyphs);
}

void font_init(void) {
	load(&fonts[FONT_S], font_s_bin);
	load(&fonts[FONT_M], font_m_bin);
	load(&fonts[FONT_L], font_l_bin);
}

int font_line_height(FontId f) { return fonts[f].h->linefeed; }

static u32 utf8_next(const char **s) {
	const u8 *p = (const u8 *)*s;
	u32 c = *p++;
	if (c >= 0xF0) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
	else if (c >= 0xE0) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
	else if (c >= 0xC0) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
	*s = (const char *)p;
	return c;
}

static const Glyph *find(const Font *f, u32 code) {
	int lo = 0, hi = f->h->nglyphs - 1;
	while (lo <= hi) {
		int m = (lo + hi) / 2;
		if (f->g[m].code == code) return &f->g[m];
		if (f->g[m].code < code) lo = m + 1; else hi = m - 1;
	}
	return code != '?' ? find(f, '?') : NULL;
}

static int span_width(const Font *f, const char *s, const char *end) {
	int w = 0;
	while (s < end && *s && *s != '\n') {
		const Glyph *g = find(f, utf8_next(&s));
		if (g) w += g->advance;
	}
	return w;
}

int font_text_width(FontId f, const char *s) {
	return span_width(&fonts[f], s, s + strlen(s));
}

static const u8 alpha_of[4] = { 0, 6, 11, 16 };

static int draw_span(int scr, const Font *f, int x, int y, const char *s, const char *end, u16 color) {
	u16 *dst = screens[scr].px;
	while (s < end && *s && *s != '\n') {
		const Glyph *g = find(f, utf8_next(&s));
		if (!g) continue;
		const u8 *bits = f->bits + (g - f->g) * f->h->cellbytes;
		int gx = x + g->left;
		for (int j = 0; j < f->h->cellh; j++) {
			int py = y + j;
			if (py < 0 || py >= SCR_H) continue;
			for (int i = 0; i < f->h->cellw; i++) {
				int px = gx + i;
				if (px < 0 || px >= SCR_W) continue;
				int bit = (j * f->h->cellw + i) * 2;
				int v = (bits[bit >> 3] >> (6 - (bit & 7))) & 3;
				if (!v) continue;
				u16 *d = &dst[py * SCR_W + px];
				*d = v == 3 ? color : gfx_blend(color, *d, alpha_of[v]);
			}
		}
		x += g->advance;
	}
	screens[scr].dirty = true;
	return x;
}

int font_draw(int scr, FontId fid, int x, int y, const char *s, u16 color, int align) {
	const Font *f = &fonts[fid];
	for (;;) {
		const char *e = strchr(s, '\n');
		if (!e) e = s + strlen(s);
		int w = span_width(f, s, e);
		int lx = align == ALIGN_CENTER ? x - w / 2 : align == ALIGN_RIGHT ? x - w : x;
		draw_span(scr, f, lx, y, s, e, color);
		y += f->h->linefeed;
		if (!*e) break;
		s = e + 1;
	}
	return y;
}

// Splits the text into lines that fit in w. Returns the number of lines.
#define MAXLINES 24
static int wrap(const Font *f, int w, const char *s, const char **ls, const char **le) {
	int n = 0;
	while (*s && n < MAXLINES) {
		const char *line = s, *brk = NULL, *p = s;
		while (*p && *p != '\n') {
			const char *q = p;
			if (*q == ' ') brk = q;
			utf8_next(&q);
			if (span_width(f, line, q) > w && p > line) {
				if (brk && brk > line) { p = brk; }
				break;
			}
			p = q;
		}
		ls[n] = line; le[n] = p; n++;
		if (*p == '\n' || *p == ' ') p++;
		s = p;
		if (!*p && p > line && p[-1] == '\n') { ls[n] = le[n] = p; n++; }
	}
	return n;
}

int font_measure_lines(FontId fid, int w, const char *s) {
	const char *ls[MAXLINES], *le[MAXLINES];
	return wrap(&fonts[fid], w, s, ls, le);
}

int font_draw_box(int scr, FontId fid, int x, int y, int w, int h, const char *s, u16 color, int align) {
	const Font *f = &fonts[fid];
	const char *ls[MAXLINES], *le[MAXLINES];
	int n = wrap(f, w, s, ls, le);
	int lh = f->h->linefeed;
	if (h > 0) y += (h - n * lh) / 2;
	for (int i = 0; i < n; i++) {
		int lw = span_width(f, ls[i], le[i]);
		int lx = align == ALIGN_CENTER ? x + (w - lw) / 2 : align == ALIGN_RIGHT ? x + w - lw : x;
		draw_span(scr, f, lx, y, ls[i], le[i], color);
		y += lh;
	}
	return y;
}
