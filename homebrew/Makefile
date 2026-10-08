#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

export TARGET	:=	DSShop
export TOPDIR	:=	$(CURDIR)

# specify a directory which contains the nitro filesystem
# this is relative to the Makefile
NITRO_FILES	:=

# These set the information text in the nds file
GAME_TITLE     := Tienda DS
GAME_SUBTITLE1 := Homebrew para Nintendo DS
GAME_SUBTITLE2 := DSPico / melonDS
GAME_ICON      := $(CURDIR)/icon.bmp

include $(DEVKITARM)/ds_rules

.PHONY: checkarm7 checkarm9 clean r4

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all: checkarm7 checkarm9 $(TARGET).nds

#---------------------------------------------------------------------------------
checkarm7:
	$(MAKE) -C arm7
	
#---------------------------------------------------------------------------------
checkarm9:
	$(MAKE) -C arm9

#---------------------------------------------------------------------------------
$(TARGET).nds	: $(NITRO_FILES) arm7/$(TARGET).elf arm9/$(TARGET).elf
	ndstool	-c $(TARGET).nds -7 arm7/$(TARGET).elf -9 arm9/$(TARGET).elf \
	-b $(GAME_ICON) "$(GAME_TITLE);$(GAME_SUBTITLE1);$(GAME_SUBTITLE2)" \
	$(_ADDFILES)

#---------------------------------------------------------------------------------
arm7/$(TARGET).elf:
	$(MAKE) -C arm7
	
#---------------------------------------------------------------------------------
arm9/$(TARGET).elf:
	$(MAKE) -C arm9

#---------------------------------------------------------------------------------
clean:
	$(MAKE) -C arm9 clean
	$(MAKE) -C arm7 clean
	rm -f $(TARGET).nds $(TARGET)_R4.nds $(TARGET).arm7 $(TARGET).arm9

#---------------------------------------------------------------------------------
# Copy with the original R4 DLDI driver already applied (its kernel does not
# apply it by itself). Driver: https://www.chishm.com/DLDI/
#---------------------------------------------------------------------------------
r4: all
	cp $(TARGET).nds $(TARGET)_R4.nds
	dlditool dldi/r4tf.dldi $(TARGET)_R4.nds
