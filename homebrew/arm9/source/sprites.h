// Original cells and animations (NCER/NANR) drawn with hardware sprites.
#pragma once
#include <nds.h>

typedef struct {
	u16 first, count;
	s16 x0, y0, x1, y1;
} SprCell;

typedef struct {
	u16 cell, duration;
} SprFrame;

typedef struct {
	u16 first, count;
} SprAnim;

typedef struct {
	int engine;	// 0 = top screen, 1 = bottom
	int tilebase, palbase;
	int ncells, nanims;
	const SprCell *cells;
	const u16 *oams;	// 4 u16 per object
	const SprAnim *anims;
	const SprFrame *frames;
} SprSet;

typedef struct {
	const SprSet *set;
	int anim, frame, timer;
	bool loop, done;
} AnimPlayer;

void spr_init(void);
void spr_reset(int engine);	// frees a screen's sprite VRAM
bool spr_load(SprSet *s, int engine, const void *pack);
void spr_begin(void);	// clears the frame's object list
void spr_cell(const SprSet *s, int cell, int x, int y, int prio);
void spr_cell_flip(const SprSet *s, int cell, int x, int y, int prio, bool hflip);
void spr_end(void);	// copies the list to OAM (call after VBlank)

void anim_start(AnimPlayer *a, const SprSet *s, int anim, bool loop);
void anim_step(AnimPlayer *a);	// advances 1 video frame (60 Hz)
int anim_cell(const AnimPlayer *a);
void anim_draw(const AnimPlayer *a, int x, int y, int prio);
