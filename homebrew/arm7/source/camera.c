// DSi cameras (Aptina MT9V113) for the ARM7: I2C and startup sequence.
//
// Adapted from BlocksDS libnds (source/arm7/camera.twl.c,
// camerai2c.twl.c and i2c.twl.c; Zlib license):
//   Copyright (C) 2011 Dave Murphy (WinterMute)
//   Copyright (C) 2023 Adrian "asie" Siekierka
//   Copyright (C) 2023 Epicpkmn11
//
//   This software is provided 'as-is', without any express or implied
//   warranty. In no event will the authors be held liable for any damages
//   arising from the use of this software.
//   Permission is granted to anyone to use this software for any purpose,
//   including commercial applications, and to alter it and redistribute it
//   freely, subject to the following restrictions:
//   1. The origin of this software must not be misrepresented; you must not
//      claim that you wrote the original software. If you use this software
//      in a product, an acknowledgment in the product documentation would be
//      appreciated but is not required.
//   2. Altered source versions must be plainly marked as such, and must not be
//      misrepresented as being the original software.
//   3. This notice may not be removed or altered from any source distribution.
//
// Changes: functions renamed and made private, only what the preview
// needs, and commands run in the main loop (not in the FIFO
// interrupt) so Wi-Fi is not slowed down.
#include <nds.h>
#include "camcmd.h"

#define CAM0 0x7A	// inner
#define CAM1 0x78	// outer
#define PM 0x4A
#define PM_CAMLED 0x31

#define I2CCNT_STOP BIT(0)
#define I2CCNT_START BIT(1)
#define I2CCNT_ERROR BIT(2)
#define I2CCNT_ACK BIT(4)
#define I2CCNT_READ BIT(5)
#define I2CCNT_ENABLE_IRQ BIT(6)
#define I2CCNT_ENABLE BIT(7)

// Camera I2C registers
#define R_CHIP_VERSION 0x0000
#define R_PLL_DIVS 0x0010
#define R_PLL_P_DIVS 0x0012
#define R_PLL_CNT 0x0014
#define R_CLOCKS_CNT 0x0016
#define R_STANDBY_CNT 0x0018
#define R_RESET_MISC_CNT 0x001A
#define R_PAD_SLEW 0x001E
#define R_MCU_ADDRESS 0x098C
#define R_MCU_DATA0 0x0990
#define R_COLOR_PIPELINE_CNT 0x3210
#define R_APERTURE_PARAMS 0x326C

#define PLL_BYPASS (1 << 0)
#define PLL_ENABLE (1 << 1)
#define PLL_RESET_CNTR (1 << 8)
#define PLL_LOCK (1 << 15)
#define CLKIN_ENABLE (1 << 9)
#define STANDBY_ENABLE (1 << 0)
#define STANDBY_IRQ_ENABLE (1 << 3)
#define STANDBY_STATUS (1 << 14)
#define I2C_RESET (1 << 0)
#define MIPI_TX_RESET (1 << 1)
#define PARALLEL_ENABLE (1 << 9)

// Camera microcontroller registers
#define M8 0x8000
#define M_SEQ_CMD (M8 | 0x2103)
#define M_SEQ_CAP_MODE (M8 | 0x2115)
#define M_SEQ_PREVIEW1_AWB (M8 | 0x211F)
#define M_AE_WINDOW_POS (M8 | 0x2202)
#define M_AE_WINDOW_SIZE (M8 | 0x2203)
#define M_AE_MIN_INDEX (M8 | 0x220B)
#define M_AE_MAX_INDEX (M8 | 0x220C)
#define M_AE_TARGET_BUFFER_SPEED (M8 | 0x224C)
#define M_AE_TARGET_BASE (M8 | 0x224F)
#define M_A_OUTPUT_WIDTH 0x2703
#define M_A_OUTPUT_HEIGHT 0x2705
#define M_B_OUTPUT_WIDTH 0x2707
#define M_B_OUTPUT_HEIGHT 0x2709
#define M_A_ROW_SPEED 0x2715
#define M_A_READ_MODE 0x2717
#define M_A_FINE_CORRECTION 0x2719
#define M_A_FINE_IT_MIN 0x271B
#define M_A_FINE_IT_MAX_MARGIN 0x271D
#define M_A_FRAME_LENGTH 0x271F
#define M_A_LINE_LENGTH_PCK 0x2721
#define M_B_ROW_SPEED 0x272B
#define M_B_READ_MODE 0x272D
#define M_B_FINE_CORRECTION 0x272F
#define M_B_FINE_IT_MIN 0x2731
#define M_B_FINE_IT_MAX_MARGIN 0x2733
#define M_B_FRAME_LENGTH 0x2735
#define M_B_LINE_LENGTH_PCK 0x2737
#define M_A_OUTPUT_FORMAT 0x2755
#define M_B_OUTPUT_FORMAT 0x2757
#define M_HG_LL_AP_CORR1 (M8 | 0x2B22)

#define CAP_VIDEO_ENABLE (1 << 1)
#define CAP_VIDEO_AWB_ENABLE (1 << 4)
#define CAP_VIDEO_HG_ENABLE (1 << 5)
#define FMT_SWAP_LUMA_CHROMA (1 << 1)
#define READ_X_MIRROR (1 << 0)
#define READ_Y_ODD_INC(n) ((n) << 2)
#define READ_X_ODD_INC(n) ((n) << 5)
#define SEQ_CMD_REFRESH 5
#define SEQ_CMD_REFRESH_MODE 6

// ---------------------------------------------------------------- I2C

static u32 delay_cycles;

static void wait_busy(void) { while (REG_I2CCNT & 0x80) {} }
static void i2c_delay(void) { wait_busy(); if (delay_cycles) swiDelay(delay_cycles); }
static u8 result(void) { wait_busy(); return (REG_I2CCNT >> 4) & 1; }

static u8 put(u8 data, u8 flags) {
	REG_I2CDATA = data;
	REG_I2CCNT = I2CCNT_ENABLE | I2CCNT_ENABLE_IRQ | flags;
	return result();
}

static u8 get(u8 flags) {
	REG_I2CCNT = I2CCNT_ENABLE | I2CCNT_ENABLE_IRQ | flags;
	wait_busy();
	return REG_I2CDATA;
}

static u8 select_device(u8 dev, u8 flags) {
	wait_busy();
	return put(dev, flags);
}

static u8 select_reg(u8 reg, u8 flags) {
	i2c_delay();
	return put(reg, flags);
}

static u8 apt_write(u8 dev, u16 reg, u16 data) {
	delay_cycles = 0;
	for (int i = 0; i < 8; i++) {
		if (select_device(dev, I2CCNT_START) && select_reg(reg >> 8, 0) && select_reg(reg & 0xFF, 0)) {
			i2c_delay();
			if (put(data >> 8, 0) && put(data & 0xFF, I2CCNT_STOP)) return 1;
		}
		REG_I2CCNT = I2CCNT_ENABLE | I2CCNT_ENABLE_IRQ | I2CCNT_ERROR | I2CCNT_STOP;
	}
	return 0;
}

static u16 apt_read(u8 dev, u16 reg) {
	delay_cycles = 0;
	for (int i = 0; i < 8; i++) {
		if (select_device(dev, I2CCNT_START) && select_reg(reg >> 8, 0) && select_reg(reg & 0xFF, I2CCNT_STOP)) {
			i2c_delay();
			if (select_device(dev | 1, I2CCNT_START)) {
				u16 hi = get(I2CCNT_READ | I2CCNT_ACK);
				return (hi << 8) | get(I2CCNT_STOP | I2CCNT_READ);
			}
		}
		REG_I2CCNT = I2CCNT_ENABLE | I2CCNT_ENABLE_IRQ | I2CCNT_ERROR | I2CCNT_STOP;
	}
	return 0xFFFF;
}

// Bounded wait: if the camera does not respond the console does not hang
static bool wait_bits(u8 dev, u16 reg, u16 mask, bool set) {
	for (int i = 0; i < 20000; i++) {
		u16 v = apt_read(dev, reg);
		if (v == 0xFFFF) return false;
		if (set ? (v & mask) == mask : !(v & mask)) return true;
	}
	return false;
}

static void set_bits(u8 dev, u16 reg, u16 mask) { apt_write(dev, reg, apt_read(dev, reg) | mask); }
static void clear_bits(u8 dev, u16 reg, u16 mask) { apt_write(dev, reg, apt_read(dev, reg) & ~mask); }

static u16 mcu_read(u8 dev, u16 reg) {
	apt_write(dev, R_MCU_ADDRESS, reg);
	return apt_read(dev, R_MCU_DATA0);
}

static void mcu_write(u8 dev, u16 reg, u16 data) {
	apt_write(dev, R_MCU_ADDRESS, reg);
	apt_write(dev, R_MCU_DATA0, data);
}

static void mcu_set_bits(u8 dev, u16 reg, u16 mask) { mcu_write(dev, reg, mcu_read(dev, reg) | mask); }

static void seq_cmd(u8 dev, u8 cmd) {
	mcu_write(dev, M_SEQ_CMD, cmd);
	for (int i = 0; i < 20000 && (mcu_read(dev, M_SEQ_CMD) & 0xFF); i++) {}
}

// ---------------------------------------------------------------- camera

static void cam_init(u8 dev) {
	apt_write(dev, R_RESET_MISC_CNT, MIPI_TX_RESET | I2C_RESET);
	apt_write(dev, R_RESET_MISC_CNT, 0);
	apt_write(dev, R_STANDBY_CNT, STANDBY_STATUS | STANDBY_IRQ_ENABLE | (1 << 5));
	apt_write(dev, R_PAD_SLEW, 1 | (2 << 8));
	apt_write(dev, R_CLOCKS_CNT, CLKIN_ENABLE | 0x40DF);
	wait_bits(dev, R_STANDBY_CNT, STANDBY_STATUS, false);
	wait_bits(dev, 0x301A, 0x0004, true);

	mcu_write(dev, 0x02F0, 0x0000);
	mcu_write(dev, 0x02F2, 0x0210);
	mcu_write(dev, 0x02F4, 0x001A);
	mcu_write(dev, 0x2145, 0x02F4);
	mcu_write(dev, M8 | 0x2134, 0x01);

	mcu_set_bits(dev, M_SEQ_CAP_MODE, CAP_VIDEO_ENABLE);
	mcu_write(dev, M_A_OUTPUT_FORMAT, FMT_SWAP_LUMA_CHROMA);
	mcu_write(dev, M_B_OUTPUT_FORMAT, FMT_SWAP_LUMA_CHROMA);

	// PLL tuned to the console's timings
	apt_write(dev, R_PLL_CNT, 0x2044 | PLL_RESET_CNTR | PLL_BYPASS);
	apt_write(dev, R_PLL_DIVS, 17 | (1 << 8));
	apt_write(dev, R_PLL_P_DIVS, 0);
	apt_write(dev, R_PLL_CNT, 0x2448 | PLL_ENABLE | PLL_BYPASS);
	apt_write(dev, R_PLL_CNT, 0x3048 | PLL_ENABLE | PLL_BYPASS);
	wait_bits(dev, R_PLL_CNT, PLL_LOCK, true);
	clear_bits(dev, R_PLL_CNT, PLL_BYPASS);

	mcu_write(dev, M_A_OUTPUT_WIDTH, 256);
	mcu_write(dev, M_A_OUTPUT_HEIGHT, 192);
	mcu_write(dev, M_B_OUTPUT_WIDTH, 640);
	mcu_write(dev, M_B_OUTPUT_HEIGHT, 480);

	u16 read_mode = READ_X_ODD_INC(1) | READ_Y_ODD_INC(1);
	if (dev == CAM1) read_mode |= READ_X_MIRROR;
	mcu_write(dev, M_A_ROW_SPEED, 1);
	mcu_write(dev, M_A_FINE_CORRECTION, 26);
	mcu_write(dev, M_A_FINE_IT_MIN, 107);
	mcu_write(dev, M_A_FINE_IT_MAX_MARGIN, 107);
	mcu_write(dev, M_A_FRAME_LENGTH, 704);
	mcu_write(dev, M_A_LINE_LENGTH_PCK, 843);
	mcu_write(dev, M_AE_MIN_INDEX, 0);
	mcu_write(dev, M_AE_MAX_INDEX, 6);
	mcu_write(dev, M_B_ROW_SPEED, 1);
	mcu_write(dev, M_B_FINE_CORRECTION, 26);
	mcu_write(dev, M_B_FINE_IT_MIN, 107);
	mcu_write(dev, M_B_FINE_IT_MAX_MARGIN, 107);
	mcu_write(dev, M_B_FRAME_LENGTH, 704);
	mcu_write(dev, M_B_LINE_LENGTH_PCK, 843);
	set_bits(dev, R_COLOR_PIPELINE_CNT, 1 << 3);
	mcu_write(dev, M8 | 0x2208, 0x00);
	mcu_write(dev, M_AE_TARGET_BUFFER_SPEED, 32);
	mcu_write(dev, M_AE_TARGET_BASE, 112);
	mcu_write(dev, M_A_READ_MODE, read_mode);
	mcu_write(dev, M_B_READ_MODE, read_mode);
	if (dev == CAM0) {
		mcu_write(dev, M_AE_WINDOW_POS, 2 | (2 << 4));
		mcu_write(dev, M_AE_WINDOW_SIZE, 11 | (11 << 4));
	} else {
		mcu_write(dev, M_AE_WINDOW_POS, 0);
		mcu_write(dev, M_AE_WINDOW_SIZE, 15 | (15 << 4));
	}
	set_bits(dev, R_CLOCKS_CNT, 1 << 5);
	mcu_write(dev, M_SEQ_CAP_MODE, 0x40 | CAP_VIDEO_HG_ENABLE | CAP_VIDEO_AWB_ENABLE | CAP_VIDEO_ENABLE);
	mcu_write(dev, M_SEQ_PREVIEW1_AWB, 0x01);
	if (dev == CAM0) {
		apt_write(dev, R_APERTURE_PARAMS, (1 << 8) | (1 << 11));
		mcu_write(dev, M_HG_LL_AP_CORR1, 1);
	} else {
		apt_write(dev, R_APERTURE_PARAMS, (0 << 8) | (2 << 11));
		mcu_write(dev, M_HG_LL_AP_CORR1, 2);
	}
	seq_cmd(dev, SEQ_CMD_REFRESH_MODE);
	seq_cmd(dev, SEQ_CMD_REFRESH);
}

static void cam_activate(u8 dev) {
	clear_bits(dev, R_STANDBY_CNT, STANDBY_ENABLE);
	wait_bits(dev, R_STANDBY_CNT, STANDBY_STATUS, false);
	wait_bits(dev, 0x301A, 0x0004, true);
	set_bits(dev, R_RESET_MISC_CNT, PARALLEL_ENABLE);
	if (dev == CAM1) i2cWriteRegister(PM, PM_CAMLED, 1);
}

static void cam_deactivate(u8 dev) {
	clear_bits(dev, R_RESET_MISC_CNT, PARALLEL_ENABLE);
	set_bits(dev, R_STANDBY_CNT, STANDBY_ENABLE);
	wait_bits(dev, R_STANDBY_CNT, STANDBY_STATUS, true);
	wait_bits(dev, 0x301A, 0x0004, false);
	if (dev == CAM1) i2cWriteRegister(PM, PM_CAMLED, 0);
}

// ---------------------------------------------------------------- FIFO

static volatile u32 pending;
static volatile bool has_pending;
static u8 active = 0xFF;

static void fifo_handler(u32 value, void *data) {
	(void)data;
	pending = value;
	has_pending = true;
}

void camera_init(void) {
	fifoSetValue32Handler(FIFO_CAM, fifo_handler, NULL);
}

// Called from the ARM7 main loop
void camera_poll(void) {
	if (!has_pending) return;
	u32 v = pending;
	has_pending = false;
	u32 cmd = v >> 24, arg = v & 0xFFFFFF;
	if (!isDSiMode()) { fifoSendValue32(FIFO_CAM, 0); return; }
	switch (cmd) {
	case CAM_CMD_INIT:
		cam_init(CAM0);
		cam_init(CAM1);
		cam_deactivate(CAM0);
		cam_deactivate(CAM1);
		active = 0xFF;
		fifoSendValue32(FIFO_CAM, apt_read(CAM0, R_CHIP_VERSION));
		break;
	case CAM_CMD_SELECT:
		if (active != 0xFF) cam_deactivate(active);
		active = arg == 0 ? CAM0 : arg == 1 ? CAM1 : 0xFF;
		if (active != 0xFF) cam_activate(active);
		fifoSendValue32(FIFO_CAM, 1);
		break;
	case CAM_CMD_SEQ:
		if (active != 0xFF) seq_cmd(active, arg & 0xFF);
		fifoSendValue32(FIFO_CAM, 1);
		break;
	case CAM_CMD_DEINIT:
		if (active != 0xFF) cam_deactivate(active);
		active = 0xFF;
		fifoSendValue32(FIFO_CAM, 1);
		break;
	default:
		fifoSendValue32(FIFO_CAM, 0);
	}
}
