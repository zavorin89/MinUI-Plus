#!/bin/bash
# Builds the MinUI+ release for the Miyoo A30 (my282) into releases/.
#
#   tools/package-a30.sh <stock MinUI card or extracted release>
#
# The stock cores (gambatte, gpsp, pcsx_rearmed and the rest) are MinUI's own builds, taken from a stock MinUI card
# or release: <dir>/.system/my282/cores and <dir>/Emus/my282/*.pak. snes9x 2010 and current snes9x are built from
# libretro source (or taken from SNES_CORES=<dir> when they're already built).
#
# Unlike `make`, this never mounts the workspace into Docker: the workspace goes into the my282-toolchain image
# through tar and the builds come back the same way, so it works from a drive Docker can't share. Everything
# happens in a copy of the repo (STAGE=<dir>, a temporary one otherwise); only the zip is copied back.
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
STOCK="$1"
[ -d "$STOCK/.system/my282/cores" ] && [ -d "$STOCK/Emus/my282" ] || {
	echo "usage: $0 <stock MinUI card or extracted release, with .system/my282/cores and Emus/my282>" >&2
	exit 1
}
STOCK="$(cd "$STOCK" && pwd)"
"$REPO/tools/dev/build-https.sh"
STAGE="${STAGE:-$(mktemp -d)}/MinUI-src"

echo "== staging in $STAGE"
mkdir -p "$STAGE"
rsync -a --delete --exclude build --exclude releases --exclude '._*' --exclude .DS_Store "$REPO/" "$STAGE/"
cp "$REPO/tools/dev/out/all/https/build/my282/https.elf" "$STAGE/skeleton/SYSTEM/my282/bin/https.elf"
cd "$STAGE"
find workspace -type d -name build -prune -exec rm -rf {} +
# the releases already made, so a second one on the same day is numbered after them (MinUIPlus-<date>-1 and so on)
mkdir -p releases
for zip in "$REPO"/releases/MinUIPlus-*.zip; do [ -f "$zip" ] && touch "releases/$(basename "$zip")"; done

echo "== setup"
script -q /dev/null make setup </dev/null >/dev/null # (it checks for a terminal)

echo "== building in my282-toolchain"
# the cores are put in place below, the rest is the workspace makefile's list
COPYFILE_DISABLE=1 tar -cf - workspace | docker run --rm -i -e BUILD_SNES=$([ -n "$SNES_CORES" ] && echo 0 || echo 1) my282-toolchain /bin/bash -c '
	set -e
	cd /root && tar -xf - && cd workspace && . ~/.bashrc
	export PLATFORM=my282
	{
		(cd my282/libmsettings && make)
		(cd my282 && make early)
		(cd my282/keymon && make)
		for d in minui minarch clock minput syncsettings say rasync wifi boxart controls; do (cd all/$d && make); done
		(cd my282 && make)
		if [ "$BUILD_SNES" = 1 ]; then (cd my282/cores && make snes); fi
	} > /tmp/build.log 2>&1 || { tail -40 /tmp/build.log >&2; exit 1; }
	tar -cf - .
' | tar -xf - -C workspace

echo "== cores"
OUT=workspace/my282/cores/output
mkdir -p $OUT
cp "$STOCK"/.system/my282/cores/*.so $OUT/
cp "$STOCK"/Emus/my282/*.pak/*.so $OUT/
# gpSP is the stock revision with MinUI+'s GB Player rumble fix, from tools/dev/build-gpsp.sh
cp "$REPO/tools/dev/out/gpsp/gpsp_libretro.so" $OUT/ || { echo "build gpSP first: tools/dev/build-gpsp.sh" >&2; exit 1; }
if [ -n "$SNES_CORES" ]; then cp "$SNES_CORES"/snes9x2010_libretro.so "$SNES_CORES"/snes9x_libretro.so $OUT/; fi
for core in fceumm gambatte gpsp picodrive snes9x2005_plus pcsx_rearmed fake08 mgba mednafen_pce_fast pokemini race \
	mednafen_supafaust mednafen_vb snes9x2010 snes9x; do
	[ -f $OUT/${core}_libretro.so ] || { echo "missing $OUT/${core}_libretro.so" >&2; exit 1; }
done

echo "== packaging"
make system cores PLATFORM=my282
make special package
NAME=$(cat build/latest.txt)
mkdir -p "$REPO/releases"
cp "releases/$NAME.zip" "$REPO/releases/"
echo "== $REPO/releases/$NAME.zip"
