#include <string.h>
#include "sprites.h"

typedef struct {
	u32 tilebytes;
	u16 npalbanks, ncells, noams, nanims, nframes, pad;
} SprPackHdr;

static const u8 obj_w[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const u8 obj_h[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

static u16 shadow[2][128 * 4] ALIGN(4);
static int nobj[2];
static int next_tile[2], next_pal[2];

void spr_init(void) {
	spr_reset(0);
	spr_reset(1);
	spr_begin();
	spr_end();
}

void spr_reset(int engine) {
	next_tile[engine] = 0;
	next_pal[engine] = 0;
}

bool spr_load(SprSet *s, int engine, const void *pack) {
	const SprPackHdr *h = pack;
	const u16 *pal = (const u16 *)(h + 1);
	const u8 *tiles = (const u8 *)(pal + h->npalbanks * 16);
	const u8 *p = tiles + h->tilebytes;
	if (next_pal[engine] + h->npalbanks > 16) return false;
	if ((next_tile[engine] * 128 + h->tilebytes) > 128 * 1024) return false;

	s->engine = engine;
	s->tilebase = next_tile[engine];
	s->palbase = next_pal[engine];
	s->ncells = h->ncells;
	s->nanims = h->nanims;
	s->cells = (const SprCell *)p;   p += h->ncells * sizeof(SprCell);
	s->oams = (const u16 *)p;        p += h->noams * 8;
	s->anims = (const SprAnim *)p;   p += h->nanims * sizeof(SprAnim);
	s->frames = (const SprFrame *)p;

	u16 *vram = engine ? SPRITE_GFX_SUB : SPRITE_GFX;
	u16 *pram = engine ? SPRITE_PALETTE_SUB : SPRITE_PALETTE;
	DC_FlushRange(tiles, h->tilebytes);
	dmaCopy(tiles, (u8 *)vram + s->tilebase * 128, h->tilebytes);
	dmaCopy(pal, pram + s->palbase * 16, h->npalbanks * 32);

	next_tile[engine] += h->tilebytes / 128;
	next_pal[engine] += h->npalbanks;
	return true;
}

void spr_begin(void) {
	nobj[0] = nobj[1] = 0;
}

void spr_cell(const SprSet *s, int cell, int x, int y, int prio) {
	spr_cell_flip(s, cell, x, y, prio, false);
}

void spr_cell_flip(const SprSet *s, int cell, int x, int y, int prio, bool hflip) {
	if (cell < 0 || cell >= s->ncells) return;
	const SprCell *c = &s->cells[cell];
	int e = s->engine;
	for (int i = 0; i < c->count && nobj[e] < 128; i++) {
		const u16 *o = &s->oams[(c->first + i) * 4];
		int oy = (s8)(o[0] & 0xFF), ox = o[1] & 0x1FF;
		if (ox >= 256) ox -= 512;
		int shape = o[0] >> 14, size = o[1] >> 14;
		if (shape > 2) continue;
		int w = obj_w[shape][size], h = obj_h[shape][size];
		if (hflip) ox = -(ox + w);
		int px = x + ox, py = y + oy;
		if (px >= 256 || py >= 192 || px + w <= 0 || py + h <= 0) continue;
		u16 *d = &shadow[e][nobj[e]++ * 4];
		d[0] = (o[0] & 0xFF00) | (py & 0xFF);
		d[1] = ((o[1] ^ (hflip ? BIT(12) : 0)) & 0xFE00) | (px & 0x1FF);
		d[2] = ((o[2] & 0x3FF) + s->tilebase) | ((((o[2] >> 12) + s->palbase) & 15) << 12) | ((prio & 3) << 10);
		d[3] = 0;
	}
}

void spr_end(void) {
	for (int e = 0; e < 2; e++) {
		for (int i = nobj[e]; i < 128; i++) {
			shadow[e][i * 4 + 0] = ATTR0_DISABLED;
			shadow[e][i * 4 + 1] = 0;
			shadow[e][i * 4 + 2] = 0;
		}
		DC_FlushRange(shadow[e], sizeof(shadow[e]));
		dmaCopy(shadow[e], e ? OAM_SUB : OAM, sizeof(shadow[e]));
	}
}

void anim_start(AnimPlayer *a, const SprSet *s, int anim, bool loop) {
	a->set = s;
	a->anim = anim;
	a->frame = 0;
	a->timer = 0;
	a->loop = loop;
	a->done = false;
}

void anim_step(AnimPlayer *a) {
	if (!a->set || a->done) return;
	const SprAnim *an = &a->set->anims[a->anim];
	const SprFrame *f = &a->set->frames[an->first + a->frame];
	if (++a->timer < f->duration) return;
	a->timer = 0;
	if (++a->frame >= an->count) {
		if (a->loop) a->frame = 0;
		else { a->frame = an->count - 1; a->done = true; }
	}
}

int anim_cell(const AnimPlayer *a) {
	if (!a->set) return -1;
	const SprAnim *an = &a->set->anims[a->anim];
	return a->set->frames[an->first + a->frame].cell;
}

void anim_draw(const AnimPlayer *a, int x, int y, int prio) {
	if (a->set) spr_cell(a->set, anim_cell(a), x, y, prio);
}
