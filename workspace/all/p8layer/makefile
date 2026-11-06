###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=my282 make)
endif

ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

include ../../$(PLATFORM)/platform/makefile.env
SDL?=SDL

###########################################################

TARGET = p8layer
MINARCH = ../minarch
INCDIR = -I. -I$(MINARCH) -I$(MINARCH)/libretro-common/include/ -I../common/ -I../../$(PLATFORM)/platform/
SOURCE = $(TARGET).c ../common/bgm.c ../common/utils.c ../common/api.c ../common/scaler.c ../../$(PLATFORM)/platform/platform.c

CC = $(CROSS_COMPILE)gcc
CFLAGS   = $(ARCH) -fomit-frame-pointer -fPIC -shared
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -DUSE_$(SDL) -O2 -std=gnu99
LDFLAGS	 = -ldl $(LIBS) -l$(SDL) -l$(SDL)_image -l$(SDL)_ttf -lpthread -lm -lz -lmsettings

all: $(MINARCH)/libretro-common
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o build/$(PLATFORM)/lib$(TARGET).so $(CFLAGS) $(LDFLAGS)
	$(CC) $(SOURCE) -DP8LAYER_TEST -o build/$(PLATFORM)/lib$(TARGET)_test.so $(CFLAGS) $(LDFLAGS)
$(MINARCH)/libretro-common:
	cd $(MINARCH) && git clone https://github.com/libretro/libretro-common
clean:
	rm -rf build
