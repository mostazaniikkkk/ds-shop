// DSi camera (ARM9: CAM and NDMA registers) + quirc to read QR codes.
// The camera part follows BlocksDS libnds (arm9/peripherals/
// camera.twl.c, Zlib license, Copyright (C) 2023 Adrian "asie" Siekierka
// and others); see the full license notice in arm7/source/camera.c.
#include <nds.h>
#include <string.h>
#include "camcmd.h"
#include "gfx.h"
#include "net.h"
#include "qr.h"
#include "quirc/quirc.h"

#define REG_CAM_MCNT (*(vu16 *)0x4004200)
#define REG_CAM_CNT (*(vu16 *)0x4004202)
#define REG_CAM_DATA (*(vu32 *)0x4004204)
#define CAM_MCNT_RESET_DISABLE BIT(1)
#define CAM_MCNT_PWR_18V_IO BIT(5)
#define CAM_CNT_SCANLINES(n) ((n) - 1)
#define CAM_CNT_TRANSFER_FLUSH BIT(5)
#define CAM_CNT_IRQ BIT(11)
#define CAM_CNT_FORMAT_RGB BIT(13)
#define CAM_CNT_TRANSFER_ENABLE BIT(15)
#define SCFG_CLK_CAMERA_IF BIT(2)
#define SCFG_CLK_CAMERA_EXT BIT(8)

#define NDMA_CH 1
#define REG_NDMA_SRC(n) (*(vu32 *)(0x04004104 + (n) * 0x1C))
#define REG_NDMA_DEST(n) (*(vu32 *)(0x04004108 + (n) * 0x1C))
#define REG_NDMA_LENGTH(n) (*(vu32 *)(0x0400410C + (n) * 0x1C))
#define REG_NDMA_BLENGTH(n) (*(vu32 *)(0x04004110 + (n) * 0x1C))
#define REG_NDMA_BDELAY(n) (*(vu32 *)(0x04004114 + (n) * 0x1C))
#define REG_NDMA_CR(n) (*(vu32 *)(0x0400411C + (n) * 0x1C))
#define NDMA_ENABLE BIT(31)
#define NDMA_BLOCK_SCALER(n) ((n) << 16)
#define NDMA_SRC_FIX (2 << 13)
#define NDMA_START_CAMERA (11 << 24)

static u16 frame_buf[SCR_W * SCR_H] ALIGN(32);
static struct quirc *qr;
static bool running;
static int nframes;
static struct quirc_code code;
static struct quirc_data data;

bool qr_available(void) { return isDSiMode(); }

// Command to the ARM7 and its reply (with a timeout)
static bool cam_cmd(u32 cmd, u32 arg, u32 *reply) {
	fifoSendValue32(FIFO_CAM, CAM_MSG(cmd, arg));
	for (int i = 0; i < 60 * 5; i++) {
		if (fifoCheckValue32(FIFO_CAM)) {
			u32 v = fifoGetValue32(FIFO_CAM);
			if (reply) *reply = v;
			return true;
		}
		swiWaitForVBlank();
	}
	return false;
}

static void hw_off(void) {
	REG_CAM_CNT &= ~0x8F00;
	REG_CAM_CNT |= CAM_CNT_TRANSFER_FLUSH;
	REG_SCFG_CLK &= ~SCFG_CLK_CAMERA_EXT;
	swiDelay(30);
	REG_CAM_MCNT = 0;
	REG_SCFG_CLK &= ~SCFG_CLK_CAMERA_IF;
	swiDelay(30);
}

static void start_dma(void) {
	REG_CAM_CNT &= ~0x200F;
	REG_CAM_CNT |= CAM_CNT_FORMAT_RGB | CAM_CNT_SCANLINES(4);
	REG_CAM_CNT |= CAM_CNT_TRANSFER_FLUSH;
	REG_CAM_CNT |= CAM_CNT_TRANSFER_ENABLE;
	REG_NDMA_SRC(NDMA_CH) = (u32)&REG_CAM_DATA;
	REG_NDMA_DEST(NDMA_CH) = (u32)frame_buf;
	REG_NDMA_LENGTH(NDMA_CH) = (SCR_W * SCR_H) >> 1;	// words
	REG_NDMA_BLENGTH(NDMA_CH) = 512;	// 4 lines
	REG_NDMA_BDELAY(NDMA_CH) = 2;
	REG_NDMA_CR(NDMA_CH) = NDMA_SRC_FIX | NDMA_BLOCK_SCALER(4) | NDMA_START_CAMERA | NDMA_ENABLE;
}

bool qr_start(void) {
	if (!qr_available()) return false;
	if (!qr) {
		qr = quirc_new();
		if (!qr || quirc_resize(qr, SCR_W, SCR_H) < 0) { net_log("qr: sin memoria"); return false; }
	}
	if (REG_CAM_MCNT || (REG_SCFG_CLK & (SCFG_CLK_CAMERA_IF | SCFG_CLK_CAMERA_EXT))) hw_off();
	REG_SCFG_CLK |= SCFG_CLK_CAMERA_IF;
	REG_CAM_MCNT = 0;
	swiDelay(30);
	REG_SCFG_CLK |= SCFG_CLK_CAMERA_EXT;
	swiDelay(30);
	REG_CAM_MCNT |= CAM_MCNT_RESET_DISABLE | CAM_MCNT_PWR_18V_IO;
	swiDelay(8200);
	REG_SCFG_CLK &= ~SCFG_CLK_CAMERA_EXT;
	REG_CAM_CNT &= ~CAM_CNT_TRANSFER_ENABLE;
	REG_CAM_CNT |= CAM_CNT_TRANSFER_FLUSH;
	REG_CAM_CNT = (REG_CAM_CNT & ~0x0300) | 0x0200;
	REG_CAM_CNT |= 0x0400;
	REG_CAM_CNT |= CAM_CNT_IRQ;
	REG_SCFG_CLK |= SCFG_CLK_CAMERA_EXT;
	swiDelay(20);

	u32 chip = 0;
	if (!cam_cmd(CAM_CMD_INIT, 0, &chip) || chip != CAM_CHIP_MT9V113) {
		net_log("qr: la camara no responde (chip %04lx)", chip);
		hw_off();
		return false;
	}
	REG_SCFG_CLK &= ~SCFG_CLK_CAMERA_EXT;
	REG_SCFG_CLK |= SCFG_CLK_CAMERA_EXT;
	swiDelay(20);
	if (!cam_cmd(CAM_CMD_SELECT, 1, NULL) || !cam_cmd(CAM_CMD_SEQ, 1, NULL)) {	// outer, preview
		qr_stop();
		return false;
	}
	start_dma();
	running = true;
	nframes = 0;
	return true;
}

void qr_stop(void) {
	REG_CAM_CNT &= ~CAM_CNT_TRANSFER_ENABLE;
	REG_NDMA_CR(NDMA_CH) = 0;
	if (running || REG_CAM_MCNT) cam_cmd(CAM_CMD_DEINIT, 0, NULL);
	hw_off();
	running = false;
}

// Looks for a QR in the image (grayscale). Slow: done every few frames.
static bool decode(char *out, int max) {
	int w, h;
	u8 *img = quirc_begin(qr, &w, &h);
	for (int i = 0; i < w * h; i++) {
		u16 c = frame_buf[i];
		img[i] = (((c & 31) + ((c >> 5) & 31) * 2 + ((c >> 10) & 31)) * 255) / 124;
	}
	quirc_end(qr);
	for (int i = 0; i < quirc_count(qr); i++) {
		quirc_extract(qr, i, &code);
		quirc_decode_error_t err = quirc_decode(&code, &data);
		if (err == QUIRC_ERROR_DATA_ECC) {	// mirrored image
			quirc_flip(&code);
			err = quirc_decode(&code, &data);
		}
		if (err) continue;
		int n = data.payload_len < max - 1 ? data.payload_len : max - 1;
		memcpy(out, data.payload, n);
		out[n] = 0;
		net_log("qr: %s", out);
		return true;
	}
	return false;
}

int qr_step(char *out, int max) {
	if (!running) return -1;
	if (REG_NDMA_CR(NDMA_CH) & NDMA_ENABLE) {
		if (++nframes > 60 * 3) { net_log("qr: no llegan imagenes"); return -1; }
		return 0;
	}
	nframes = 0;
	DC_InvalidateRange(frame_buf, sizeof(frame_buf));
	u16 *dst = screens[TOP].px;
	for (int i = 0; i < SCR_W * SCR_H; i++) dst[i] = frame_buf[i] | 0x8000;
	screens[TOP].dirty = true;
	static int skip;
	bool found = ++skip >= 4 && (skip = 0, decode(out, max));
	start_dma();
	return found ? 1 : 0;
}
