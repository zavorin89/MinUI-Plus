#!/bin/sh
# Builds the arcade cores (workspace/my282/cores `make arcade`: fbalpha2012 CPS1/CPS2/Neo Geo, FBNeo, MAME 2003-Plus)
# in the my282-toolchain image from the sources in workspace/my282/cores/src (cloned there first if missing), into
# tools/dev/out/arcade/ (TARGETS=output/fbneo_libretro.so for just one), and scans each for ARMv8-only instructions. FBNeo and MAME take a long while.
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/tools/dev/out/arcade"
mkdir -p "$OUT"
cd "$REPO/workspace/my282"
COPYFILE_DISABLE=1 tar --exclude='._*' --exclude='cores/output' -cf - cores |
docker run --rm -i -e TARGETS="${TARGETS:-arcade}" my282-toolchain /bin/bash -c '
	mkdir -p /root/build && cd /root/build && tar -xf - && . ~/.bashrc && cd cores
	make -k $TARGETS > /tmp/arcade.log 2>&1 || { grep -E "undefined reference|error:" /tmp/arcade.log | sort -u | head -20 >&2; tail -10 /tmp/arcade.log >&2; }
	for f in output/*.so; do
		n=$(/opt/my282-toolchain/usr/bin/arm-buildroot-linux-gnueabihf-objdump -d $f | grep -cE "\s(vmaxnm|vminnm|vrint[anpmxz]|vcvt[anpm]\.|vsel|crc32|lda|stl)\s" || true)
		echo "$(basename $f): $(stat -c %s $f) bytes, ARMv8 instructions: $n" >&2
	done
	tar -cf - output' | tar -xf - -C "$OUT"
ls -la "$OUT/output"
