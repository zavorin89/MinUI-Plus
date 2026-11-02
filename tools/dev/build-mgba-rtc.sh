#!/bin/bash
# Build the installed mGBA revision with MinUI+'s explicit RTC override.
# Output stays in the project; does not install or change a release.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/work/mgba-rtc-rebuild"
REV=747362c02d2e71ee7c363e8dcb240925be8af906
mkdir -p "$OUT"
if [ ! -d "$OUT/source" ]; then
    curl -fL "https://codeload.github.com/libretro/mgba/tar.gz/$REV" -o "$OUT/source.tar.gz"
    mkdir -p "$OUT/source"
    tar -xzf "$OUT/source.tar.gz" --strip-components=1 -C "$OUT/source"
    (cd "$OUT/source" && patch -p1 < "$REPO/tools/dev/patches/mgba-rtc.patch")
fi
COPYFILE_DISABLE=1 tar -cf - -C "$OUT/source" . | docker run --rm -i my282-toolchain /bin/bash -c '
set -e
mkdir -p /root/mgba
cd /root/mgba
tar -xf - 2>/dev/null
make -j4 platform=unix \
 "CC=/opt/my282-toolchain/usr/bin/arm-buildroot-linux-gnueabihf-gcc -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard" \
 AR=/opt/my282-toolchain/usr/bin/arm-buildroot-linux-gnueabihf-ar \
 "GIT_VERSION= 747362c-minui-rtc" >/tmp/build.log 2>&1 || { tail -40 /tmp/build.log >&2; exit 1; }
n=$(/opt/my282-toolchain/usr/bin/arm-buildroot-linux-gnueabihf-objdump -d mgba_libretro.so |
 grep -cE "\s(vmaxnm|vminnm|vrint[anpmxz]|vcvt[anpm]\.|vsel|crc32|lda|stl)" || true)
echo "ARMv8-only instructions: $n" >&2
[ "$n" = 0 ]
tar -cf - mgba_libretro.so
' > "$OUT/output.tar"
tar -xf "$OUT/output.tar" -C "$OUT"
echo "Built $OUT/mgba_libretro.so"
