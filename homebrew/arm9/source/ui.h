// UI elements with the DSi Shop look.
#pragma once
#include "gfx.h"
#include "font.h"
#include "sprites.h"

typedef enum { BTN_WIDE28, BTN_WIDE32, BTN_VIOLET28, BTN_VIOLET40 } ButtonStyle;

typedef struct {
	int id;
	ButtonStyle style;
	int x, y;
	const char *label;
	bool back;	// the B button also triggers it
	bool disabled;
} Button;

typedef struct {
	u32 down, held, up;
	touchPosition touch;
	bool touching, touch_down, touch_up;
	int last_tx, last_ty;	// last valid stylus position
} Input;

extern Input in;
extern SprSet spr_progress, spr_scroll, spr_wait, spr_click, spr_tuusin;

void ui_init(void);
void ui_input(void);	// reads buttons and touch screen
void ui_frame(void);	// draws the common sprites (touch effect)

void ui_header(int scr, const char *title);
void ui_error_header(int scr, const char *title);
int ui_button_w(ButtonStyle s);
int ui_button_h(ButtonStyle s);
void ui_button_draw(const Button *b, bool pressed, bool focused);

// A page's buttons: drawing, touch, D-pad and A/B.
typedef struct {
	Button *b;
	int n, focus, pressed;
	bool keys_used;
	bool touch_only;	// the D-pad is used by something else (lists): touch and B only
} ButtonGroup;

void ui_group_init(ButtonGroup *g, Button *b, int n);
void ui_group_draw(ButtonGroup *g);
int ui_group_update(ButtonGroup *g);	// id of the activated button or -1

void ui_click_effect(int x, int y);
void ui_dialog(const char *text);	// dims the bottom screen and draws the dialog box
void ui_format_size(u32 bytes, char *buf, int len);
