#!/bin/bash
# Build the installed gpSP revision (the one stock MinUI ships, so save states keep working) with MinUI+'s GB Player
# rumble fix, into tools/dev/out/gpsp/. tools/package-a30.sh puts it in the release over the stock core.
# Does not install anything. Stock's own build settings: workspace/rg35xx/cores/patches/gpsp.patch.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/tools/dev/out/gpsp"
REV=36061ca
mkdir -p "$OUT"
cat "$REPO/workspace/rg35xx/cores/patches/gpsp.patch" "$REPO/tools/dev/patches/gpsp-gb-player-rumble.patch" |
docker run --rm -i my282-toolchain /bin/bash -c '
set -e
cat > /tmp/all.patch
git clone -q https://github.com/libretro/gpsp /root/gpsp
cd /root/gpsp
git checkout -q '"$REV"'
git submodule update --init -q 2>/dev/null || true
git apply -p1 /tmp/all.patch
make -j4 platform=rg35xx CROSS_COMPILE=/opt/my282-toolchain/usr/bin/arm-buildroot-linux-gnueabihf- >/tmp/build.log 2>&1 || { tail -40 /tmp/build.log >&2; exit 1; }
/opt/my282-toolchain/usr/bin/arm-buildroot-linux-gnueabihf-strip gpsp_libretro.so
n=$(/opt/my282-toolchain/usr/bin/arm-buildroot-linux-gnueabihf-objdump -d gpsp_libretro.so |
 grep -cE "\s(vmaxnm|vminnm|vrint[anpmxz]|vcvt[anpm]\.|vsel|crc32|lda|stl)\s" || true)
echo "ARMv8-only instructions: $n" >&2
[ "$n" = 0 ]
tar -cf - gpsp_libretro.so
' | tar -xf - -C "$OUT"
ls -la "$OUT/gpsp_libretro.so"
