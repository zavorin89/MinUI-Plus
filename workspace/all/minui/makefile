###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

include ../../$(PLATFORM)/platform/makefile.env
SDL?=SDL

###########################################################

TARGET = minui
INCDIR = -I. -I../common/ -I../../$(PLATFORM)/platform/
SOURCE = $(TARGET).c ../common/keyboard.c ../common/wifi_ask.c ../common/bgm.c ../common/scaler.c ../common/utils.c ../common/api.c ../../$(PLATFORM)/platform/platform.c
# the tools, built in so they open at once (see tools.h): Controls, Wi-Fi, Box Art and RetroAchievements, with what
# Box Art and RetroAchievements share with minarch (its HTTPS client and cache, rcheevos, libchdr for .chd discs)
RA = ../minarch
RC = $(RA)/rcheevos/src
INCDIR += -I$(RA) -I$(RA)/rcheevos/include -I$(RC) -I$(RA)/libchdr/include
SOURCE += ../controls/controls.c ../wifi/wifi.c ../boxart/boxart.c ../rasync/rasync.c
SOURCE += $(RA)/ra_http.c $(RA)/ra_cache.c $(RA)/ra_chd.c $(RA)/libchdr/unity.c $(wildcard $(RC)/api/*.c $(RC)/runtime/*.c $(RC)/util/*.c $(RC)/hash/*.c)

CC = $(CROSS_COMPILE)gcc
CFLAGS   = $(ARCH) -fomit-frame-pointer
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -DUSE_$(SDL) -Ofast -std=gnu99 -DMINUI_TOOLS
CFLAGS  += -DWANT_RAW_DATA_SECTOR=1 -DWANT_SUBCODE=0 -DVERIFY_BLOCK_CRC=0 # libchdr, as rasync builds it
CFLAGS  += -DBGM_FLAC_LINKED # bgm.c uses the dr_flac libchdr builds
LDFLAGS	 = -ldl -lmsettings $(LIBS) -l$(SDL) -l$(SDL)_image -l$(SDL)_ttf -lpthread -lm -lz

PRODUCT= build/$(PLATFORM)/$(TARGET).elf

all: $(PREFIX)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
clean:
	rm -f $(PRODUCT)

$(PREFIX)/include/msettings.h:
	cd /root/workspace/$(PLATFORM)/libmsettings && make
