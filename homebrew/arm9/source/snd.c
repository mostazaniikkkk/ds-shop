#include <nds.h>
#include "snd.h"
#include "sndcmd.h"
#include "sound_data_bin.h"

// Players from the original SDAT
#define PLAYER_BGM 15
#define SEQ_SHOP_BGM 0
#define SEQARC_SE 0

static bool music_on = true, bgm_playing = false;

// Player for each SE per the SDAT (TWL_SHP_* go in their own players so
// the download sound does not cut off the button sounds).
static int se_player(int se) {
	switch (se) {
	case SE_SHP_SE_DL_TAMA:
	case SE_SHP_SE_DL_DATA: return 5;
	case SE_SHP_SE_SCROLL:
	case SE_SHP_SE_SCROLL_INVALID: return 6;
	case SE_CMN_SE_PROCESSING:
	case SE_CMN_SE_CONNECTING: return 2;
	case SE_SHP_SE_COMPLETED:
	case SE_SHP_SE_LOADED:
	case SE_SHP_SE_WARNING_PAGE: return 3;
	default: return 0;
	}
}

void snd_init(void) {
	DC_FlushRange(sound_data_bin, sound_data_bin_size);
	fifoSendAddress(FIFO_SND, (void *)sound_data_bin);
}

void snd_bgm(bool on) {
	bgm_playing = on;
	if (on && music_on) fifoSendValue32(FIFO_SND, SND_MSG(SND_CMD_PLAY_SEQ, PLAYER_BGM, SEQ_SHOP_BGM));
	else fifoSendValue32(FIFO_SND, SND_MSG(SND_CMD_STOP, PLAYER_BGM, 0));
}

void snd_se(int se) {
	fifoSendValue32(FIFO_SND, SND_MSG(SND_CMD_PLAY_SE, se_player(se), (SEQARC_SE << 16) | se));
}

void snd_set_music_enabled(bool on) {
	bool was = bgm_playing;
	music_on = on;
	snd_bgm(was);
	bgm_playing = was;
}

bool snd_music_enabled(void) { return music_on; }
