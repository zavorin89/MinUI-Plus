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

TARGET = rasync
INCDIR = -I. -I../common/ -I../../$(PLATFORM)/platform/
# shares minarch's RetroAchievements cache, HTTPS client, and vendored rcheevos
RA = ../minarch
RC = $(RA)/rcheevos/src
INCDIR += -I$(RA) -I$(RA)/rcheevos/include -I$(RC) -I$(RA)/libchdr/include
SOURCE = $(TARGET).c ../common/keyboard.c ../common/utils.c ../common/api.c ../common/scaler.c ../../$(PLATFORM)/platform/platform.c
SOURCE += $(RA)/ra_http.c $(RA)/ra_cache.c $(RA)/ra_chd.c $(RA)/libchdr/unity.c $(wildcard $(RC)/api/*.c $(RC)/runtime/*.c $(RC)/util/*.c $(RC)/hash/*.c)

CC = $(CROSS_COMPILE)gcc
CFLAGS   = $(ARCH) -fomit-frame-pointer
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -DUSE_$(SDL)  -Ofast 
CFLAGS  += -DWANT_RAW_DATA_SECTOR=1 -DWANT_SUBCODE=0 -DVERIFY_BLOCK_CRC=0 # libchdr, for hashing PlayStation .chd discs
LDFLAGS	 = -ldl $(LIBS) -l$(SDL) -l$(SDL)_image -l$(SDL)_ttf -lpthread -lm -lz
LDFLAGS +=  -lmsettings

PRODUCT= build/$(PLATFORM)/$(TARGET).elf

all: $(PREFIX)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
clean:
	rm -f $(PRODUCT)

$(PREFIX)/include/msettings.h:
	cd /root/workspace/$(PLATFORM)/libmsettings && make