// QR code reader using the DSi outer camera (DSi mode only;
// DS and DS Lite have no camera and the option is not shown).
#pragma once
#include <nds.h>

bool qr_available(void);	// there is a camera (DSi mode)
bool qr_start(void);	// turns on the outer camera; false if it does not respond
// Every frame: draws the image on the top screen and looks for a QR.
// Returns 1 if one was read (text in out), 0 if not yet, -1 if the camera fails.
int qr_step(char *out, int max);
void qr_stop(void);
