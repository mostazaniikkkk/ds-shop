// ARM7: entry point, RTC, power, the SSEQ sound player and the DSi cameras.
#include <nds.h>
#include <dswifi7.h>

void sseq_init(void);
void camera_init(void);
void camera_poll(void);

static volatile bool exitflag = false;

static void VblankHandler(void) {
	Wifi_Update();
}

static void VcountHandler(void) {
	inputGetAndSend();
}

static void powerButtonCB(void) {
	exitflag = true;
}

int main(void) {
	dmaFillWords(0, (void *)0x04000400, 0x100);

	REG_SOUNDCNT |= SOUND_ENABLE;
	writePowerManagement(PM_CONTROL_REG, (readPowerManagement(PM_CONTROL_REG) & ~PM_SOUND_MUTE) | PM_SOUND_AMP);
	powerOn(POWER_SOUND);
	REG_MASTER_VOLUME = 127;

	readUserSettings();
	ledBlink(0);

	irqInit();
	initClockIRQ();
	fifoInit();
	touchInit();

	SetYtrigger(80);
	installWifiFIFO();
	installSystemFIFO();
	sseq_init();
	camera_init();

	irqSet(IRQ_VCOUNT, VcountHandler);
	irqSet(IRQ_VBLANK, VblankHandler);
	irqEnable(IRQ_VBLANK | IRQ_VCOUNT | IRQ_NETWORK);

	setPowerButtonCB(powerButtonCB);

	while (!exitflag) {
		swiWaitForVBlank();
		camera_poll();	// DSi cameras (commands from the ARM9)
	}
	return 0;
}
