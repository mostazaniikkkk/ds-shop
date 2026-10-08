// ARM9 -> ARM7 protocol for the DSi cameras (DSi mode only).
// The ARM7 talks to the cameras over I2C; the ARM9 receives the image via NDMA.
#pragma once

#define FIFO_CAM FIFO_USER_02

// 32-bit messages: cmd (8 bits) << 24 | arg. The ARM7 replies with a value.
enum {
	CAM_CMD_INIT   = 1,	// turns on and configures both cameras; replies with the chip version
	CAM_CMD_SELECT = 2,	// arg = 0 inner, 1 outer, 2 none; replies 1
	CAM_CMD_SEQ    = 3,	// arg = sequencer command (1 preview); replies 1
	CAM_CMD_DEINIT = 4,	// turns off the active camera; replies 1
};

#define CAM_MSG(cmd, arg) (((u32)(cmd) << 24) | ((u32)(arg) & 0xFFFFFF))
#define CAM_CHIP_MT9V113 0x2280
