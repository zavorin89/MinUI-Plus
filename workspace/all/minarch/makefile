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

TARGET = minarch
INCDIR = -I. -I./libretro-common/include/ -I../common/ -I../../$(PLATFORM)/platform/
SOURCE = $(TARGET).c ra.c ra_http.c ra_cache.c ra_chd.c libchdr/unity.c ../common/scaler.c ../common/utils.c ../common/api.c ../../$(PLATFORM)/platform/platform.c

# RetroAchievements (rcheevos, vendored in rcheevos/), minus the Windows-only RAIntegration bridge
RC = rcheevos/src
SOURCE += $(wildcard $(RC)/api/*.c $(RC)/runtime/*.c $(RC)/util/*.c $(RC)/hash/*.c) $(RC)/client/rc_client.c $(RC)/libretro/rc_libretro.c
INCDIR += -Ircheevos/include -I$(RC) -I$(RC)/libretro -Ilibchdr/include

CC = $(CROSS_COMPILE)gcc
CFLAGS   = $(ARCH) -fomit-frame-pointer
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -DUSE_$(SDL) -Ofast -std=gnu99
CFLAGS  += -DWANT_RAW_DATA_SECTOR=1 -DWANT_SUBCODE=0 -DVERIFY_BLOCK_CRC=0 # libchdr, for hashing PlayStation .chd discs
CFLAGS	+= -Os -flto
CFLAGS	+= -DRC_CLIENT_SUPPORTS_HASH
LDFLAGS	 = -ldl $(LIBS) -lmsettings -l$(SDL) -l$(SDL)_image -l$(SDL)_ttf -lpthread -lm -lz
# CFLAGS  += -Wall -Wno-unused-variable -Wno-unused-function -Wno-format-overflow
# CFLAGS  += -fsanitize=address -fno-common
# LDFLAGS += -lasan

BUILD_DATE!=date +%Y.%m.%d
BUILD_HASH!=cat ../../hash.txt
CFLAGS += -DBUILD_DATE=\"${BUILD_DATE}\" -DBUILD_HASH=\"${BUILD_HASH}\"

# CFLAGS += -DBUILD_DATE=\"20230326\" -DBUILD_HASH=\"d34db33f\"

PRODUCT= build/$(PLATFORM)/$(TARGET).elf

all: libretro-common $(PREFIX)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
clean:
	rm -f $(PRODUCT)

libretro-common:
	git clone https://github.com/libretro/libretro-common
	
$(PREFIX)/include/msettings.h:
	cd /root/workspace/$(PLATFORM)/libmsettings && make