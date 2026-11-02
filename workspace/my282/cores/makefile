ARCH_FLAGS = -O3 -fPIC -DNDEBUG -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -mcpu=cortex-a7
LIBRETRO_MAKE = make CC=$(CROSS_COMPILE)gcc CXX=$(CROSS_COMPILE)g++ AR=$(CROSS_COMPILE)ar platform=unix CFLAGS="$(ARCH_FLAGS)" CXXFLAGS="$(ARCH_FLAGS)" -j4

all: stock snes arcade n64

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

# the arcade systems' cores (CPS1/2 and Neo Geo on fbalpha2012, CPS3 and FinalBurn Neo on FBNeo, MAME 2003-Plus),
# from libretro's own sources for the A30's Cortex-A7. The CPU flags are on the compiler itself so nothing in a core's
# makefile can pick a newer CPU: one ARMv8 instruction (eg. vsel, in some prebuilt CPS cores) crashes the A30
ARM_CPU = -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
ARCADE_CC = CC="$(CROSS_COMPILE)gcc $(ARM_CPU)" CXX="$(CROSS_COMPILE)g++ $(ARM_CPU)" AR=$(CROSS_COMPILE)ar -j8
# FBNeo's 7-Zip asks the kernel which ARMv8 extensions there are (none on a Cortex-A7), with HWCAP2 names this
# toolchain's old kernel headers don't have: their Linux values, so it builds (only checked at run time). Its
# makefile leaves out the threads library for this platform, so -pthread
HWCAP2 = -DHWCAP2_AES=1 -DHWCAP2_PMULL=2 -DHWCAP2_SHA1=4 -DHWCAP2_SHA2=8 -DHWCAP2_CRC32=16
FBNEO_CC = CC="$(CROSS_COMPILE)gcc $(ARM_CPU) $(HWCAP2) -pthread" CXX="$(CROSS_COMPILE)g++ $(ARM_CPU) $(HWCAP2) -pthread" AR=$(CROSS_COMPILE)ar -j8
arcade: output/fbalpha2012_cps1_libretro.so output/fbalpha2012_cps2_libretro.so output/fbalpha2012_neogeo_libretro.so \
	output/fbneo_libretro.so output/mame2003_plus_libretro.so

src/fbalpha2012_cps1 src/fbalpha2012_cps2 src/fbalpha2012_neogeo src/FBNeo src/mame2003-plus-libretro:
	mkdir -p src
	cd src && git clone --depth 1 https://github.com/libretro/$(notdir $@)

output/fbalpha2012_%_libretro.so: src/fbalpha2012_%
	mkdir -p output
	cd src/fbalpha2012_$* && make -f makefile.libretro platform=unix $(ARCADE_CC)
	$(CROSS_COMPILE)strip src/fbalpha2012_$*/fbalpha2012_$*_libretro.so -o $@

output/fbneo_libretro.so: src/FBNeo
	mkdir -p output
	cd src/FBNeo/src/burner/libretro && make platform=classic_armv7_a7 USE_EXPERIMENTAL_FLAGS=0 $(FBNEO_CC) # (its link-time optimization needs more memory than Docker gives)
	$(CROSS_COMPILE)strip src/FBNeo/src/burner/libretro/fbneo_libretro.so -o $@

output/mame2003_plus_libretro.so: src/mame2003-plus-libretro
	mkdir -p output
	cd src/mame2003-plus-libretro && make platform=rpi2 $(ARCADE_CC)
	$(CROSS_COMPILE)strip src/mame2003-plus-libretro/mame2003_plus_libretro.so -o $@

# N64: parallel-n64, built as for the SNES Classic (a Cortex-A7 with a Mali-400 GPU, as the A30 has): GLES2, the ARM
# dynarec and NEON. It draws with the GPU through minarch (hwrender.c). Its GL library isn't linked (the toolchain has
# none): GL_LIB is left without it, and minarch loads the A30's libGLESv2.so before opening it (MINARCH_GLES=1)
n64: output/parallel_n64_libretro.so

src/parallel-n64:
	mkdir -p src
	# (pinned: upstream changes daily)
	cd src && git clone --depth 1 https://github.com/libretro/parallel-n64
	cd src/parallel-n64 && git fetch --depth 1 origin 9dc453242eb6521553c3a1a960dbf92a44aa698a && git checkout -q FETCH_HEAD

# the GLES2 and EGL headers (Khronos's own), which the toolchain lacks too
KHRONOS = https://raw.githubusercontent.com/KhronosGroup
src/khronos:
	mkdir -p src/khronos/GLES2 src/khronos/EGL src/khronos/KHR
	for h in egl.h eglext.h eglplatform.h; do wget -q $(KHRONOS)/EGL-Registry/main/api/EGL/$$h -O src/khronos/EGL/$$h; done
	for h in gl2.h gl2ext.h gl2platform.h; do wget -q $(KHRONOS)/OpenGL-Registry/main/api/GLES2/$$h -O src/khronos/GLES2/$$h; done
	wget -q $(KHRONOS)/EGL-Registry/main/api/KHR/khrplatform.h -O src/khronos/KHR/khrplatform.h

output/parallel_n64_libretro.so: src/parallel-n64 src/khronos
	mkdir -p output
	# (its whole-program link-time optimization, for an executable, drops a shared library's own exports: left out)
	# (and it insists every symbol is there at link: the GL ones come from minarch's preload at run time)
	sed -i 's/-flto=4 -fwhole-program -fuse-linker-plugin//; s/ -Wl,--no-undefined//' src/parallel-n64/Makefile
	# (a frame the frontend's behind on isn't drawn at all, rather than drawn and hidden)
	cd src/parallel-n64 && (git apply --reverse --check ../../parallel-n64-frameskip.patch 2>/dev/null || git apply ../../parallel-n64-frameskip.patch)
	cd src/parallel-n64 && make platform=classic_armv7_a7 GL_LIB=-lm \
		CC="$(CROSS_COMPILE)gcc $(ARM_CPU) -I$(CURDIR)/src/khronos" CXX="$(CROSS_COMPILE)g++ $(ARM_CPU) -I$(CURDIR)/src/khronos" AR=$(CROSS_COMPILE)ar -j8
	$(CROSS_COMPILE)strip src/parallel-n64/parallel_n64_libretro.so -o $@

clean:
	rm -rf output src

.PHONY: all stock snes arcade n64 clean
