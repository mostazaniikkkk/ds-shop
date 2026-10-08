// On-screen keyboard with the graphics of the system keyboard (keyboard.szs)
// and the pieces of the shop keyboard (sm_keyboard).
#pragma once
#include <nds.h>

enum { KBD_RUNNING = 0, KBD_ACCEPTED = 1, KBD_CANCELLED = -1 };

// Prepares the keyboard to edit buf (UTF-8, maxlen bytes including the 0).
void kbd_setup(const char *title, const char *help, char *buf, int maxlen);
void kbd_enter(void);	// draws both screens
int kbd_update(void);	// KBD_RUNNING while typing
