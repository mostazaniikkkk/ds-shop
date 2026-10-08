// Original music and effects (SDAT) played by the ARM7.
#pragma once
#include "assets_gen.h"

void snd_init(void);
void snd_bgm(bool on);	// TWL_SHOP_BGM
void snd_se(int se);	// SE_* from assets_gen.h
void snd_set_music_enabled(bool on);
bool snd_music_enabled(void);
