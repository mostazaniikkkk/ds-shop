// Text with the DSi system fonts (TBF1 at 10, 12 and 16 px), UTF-8.
#pragma once
#include <nds.h>

typedef enum { FONT_S, FONT_M, FONT_L } FontId;
enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

// Icons in the font's private use area (U+E000..)
#define ICON_A     "\xee\x80\x80"
#define ICON_B     "\xee\x80\x81"
#define ICON_X     "\xee\x80\x82"
#define ICON_Y     "\xee\x80\x83"
#define ICON_L     "\xee\x80\x84"
#define ICON_R     "\xee\x80\x85"
#define ICON_DPAD  "\xee\x80\x86"

void font_init(void);
int font_line_height(FontId f);
int font_text_width(FontId f, const char *s);	// width of the first line
// Draws text; '\n' starts a new line. Returns the y following the last line.
int font_draw(int scr, FontId f, int x, int y, const char *s, u16 color, int align);
// Word-wraps to width w and centers vertically in h (if h > 0).
int font_draw_box(int scr, FontId f, int x, int y, int w, int h, const char *s, u16 color, int align);
// Counts the lines the text would take wrapped to w
int font_measure_lines(FontId f, int w, const char *s);
