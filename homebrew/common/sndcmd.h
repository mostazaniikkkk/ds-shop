// ARM9 -> ARM7 protocol for the SSEQ player.
#pragma once

#define FIFO_SND FIFO_USER_01

// 32-bit messages: cmd (4 bits) | player (4 bits) | arg (24 bits).
// The SDAT address is sent separately with fifoSendAddress.
enum {
	SND_CMD_PLAY_SEQ   = 1,	// arg = SEQ index
	SND_CMD_PLAY_SE    = 2,	// arg = (seqarc << 16) | index within the SSAR
	SND_CMD_STOP       = 3,	// player; arg = 1 to stop without release
	SND_CMD_STOP_ALL   = 4,
	SND_CMD_VOLUME     = 5,	// player; arg = volume 0..127
};

#define SND_MSG(cmd, player, arg) (((u32)(cmd) << 28) | ((u32)((player) & 15) << 24) | ((u32)(arg) & 0xFFFFFF))
