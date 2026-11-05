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

TARGET = ndslayer
MINARCH = ../minarch
INCDIR = -I. -I$(MINARCH) -I$(MINARCH)/libretro-common/include/ -I../common/ -I../../$(PLATFORM)/platform/
SOURCE = $(TARGET).c ../common/bgm.c ../common/utils.c ../common/api.c ../common/scaler.c ../../$(PLATFORM)/platform/platform.c

# RetroAchievements, the same code and rcheevos as minarch
RC = $(MINARCH)/rcheevos/src
SOURCE += $(MINARCH)/ra.c $(MINARCH)/ra_http.c $(MINARCH)/ra_cache.c $(MINARCH)/ra_chd.c $(MINARCH)/libchdr/unity.c
SOURCE += $(wildcard $(RC)/api/*.c $(RC)/runtime/*.c $(RC)/util/*.c $(RC)/hash/*.c) $(RC)/client/rc_client.c $(RC)/libretro/rc_libretro.c
INCDIR += -I$(MINARCH)/rcheevos/include -I$(RC) -I$(RC)/libretro -I$(MINARCH)/libchdr/include

CC = $(CROSS_COMPILE)gcc
CFLAGS   = $(ARCH) -fomit-frame-pointer -fPIC -shared
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -DUSE_$(SDL) -Ofast -std=gnu99
CFLAGS  += -DBGM_FLAC_LINKED -DWANT_RAW_DATA_SECTOR=1 -DWANT_SUBCODE=0 -DVERIFY_BLOCK_CRC=0 -DRC_CLIENT_SUPPORTS_HASH
LDFLAGS	 = -ldl $(LIBS) -l$(SDL) -l$(SDL)_image -l$(SDL)_ttf -lpthread -lm -lz -lmsettings

all: $(MINARCH)/libretro-common
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o build/$(PLATFORM)/lib$(TARGET).so $(CFLAGS) $(LDFLAGS)
	$(CC) $(SOURCE) -DNDSLAYER_TEST -o build/$(PLATFORM)/lib$(TARGET)_test.so $(CFLAGS) $(LDFLAGS)
	echo 'int snd_pcm_avail,snd_pcm_close,snd_pcm_hw_params,snd_pcm_hw_params_any,snd_pcm_hw_params_free,snd_pcm_hw_params_malloc,snd_pcm_hw_params_set_access,snd_pcm_hw_params_set_buffer_size_near,snd_pcm_hw_params_set_channels,snd_pcm_hw_params_set_format,snd_pcm_hw_params_set_period_size_near,snd_pcm_hw_params_set_rate_near,snd_pcm_open,snd_pcm_prepare,snd_pcm_readi,snd_pcm_recover,snd_pcm_start,snd_pcm_sw_params,snd_pcm_sw_params_current,snd_pcm_sw_params_free,snd_pcm_sw_params_malloc,snd_pcm_writei,snd_strerror;' | $(CC) -x c - -shared -fPIC -Wl,-soname,libasound.so.2 -o build/$(PLATFORM)/libasound.so.2
$(MINARCH)/libretro-common:
	cd $(MINARCH) && git clone https://github.com/libretro/libretro-common
clean:
	rm -rf build
