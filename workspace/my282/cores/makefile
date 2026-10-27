ARCH_FLAGS = -O3 -fPIC -DNDEBUG -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -mcpu=cortex-a7
LIBRETRO_MAKE = make CC=$(CROSS_COMPILE)gcc CXX=$(CROSS_COMPILE)g++ AR=$(CROSS_COMPILE)ar platform=unix CFLAGS="$(ARCH_FLAGS)" CXXFLAGS="$(ARCH_FLAGS)" -j4

all: stock snes

stock:
	# the stock and extra cores are rg35xx's (they run as is on the A30): built here when there's no rg35xx build
	test -d ../../rg35xx/cores/output || $(MAKE) -C ../../rg35xx/cores PLATFORM=rg35xx
	rm -rf output
	cp -r ../../rg35xx/cores/output ./

# MinUI+'s other SNES cores, a game at a time from the game menu (MENU + Y): snes9x 2010 and current snes9x,
# both from libretro, built here for the A30's Cortex-A7
snes: output/snes9x2010_libretro.so output/snes9x_libretro.so

src/snes9x2010:
	mkdir -p src
	cd src && git clone --depth 1 https://github.com/libretro/snes9x2010

src/snes9x:
	mkdir -p src
	cd src && git clone --depth 1 https://github.com/libretro/snes9x

output/snes9x2010_libretro.so: src/snes9x2010
	mkdir -p output
	cd src/snes9x2010 && $(LIBRETRO_MAKE) -f Makefile.libretro
	$(CROSS_COMPILE)strip src/snes9x2010/snes9x2010_libretro.so -o $@

output/snes9x_libretro.so: src/snes9x
	mkdir -p output
	cd src/snes9x/libretro && $(LIBRETRO_MAKE)
	$(CROSS_COMPILE)strip src/snes9x/libretro/snes9x_libretro.so -o $@

clean:
	rm -rf output src

.PHONY: all stock snes clean
