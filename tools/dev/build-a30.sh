#!/bin/sh
# Builds the launcher, minarch and the MinUI+ tools for the Miyoo A30 in the my282-toolchain Docker image, into
# tools/dev/out/. The workspace goes in through tar (Docker can't mount this drive), and every binary is scanned for
# ARMv8-only instructions: the A30's Cortex-A7 is ARMv7, and one of those crashes it (see logs/minui-crash-vmaxnm.txt)
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/tools/dev/out"
mkdir -p "$OUT"
cd "$REPO/workspace"
COPYFILE_DISABLE=1 tar --exclude='build' --exclude='._*' --exclude='my282/cores/src' -cf - \
	my282 all/common all/minui all/minarch all/rasync all/wifi all/boxart all/controls all/clock all/ndslayer 2>/dev/null |
docker run --rm -i my282-toolchain /bin/bash -c '
	mkdir -p /root/workspace && cd /root/workspace && tar -xf - 2>/dev/null && . ~/.bashrc
	(cd my282/libmstick && make) >/tmp/make.log 2>&1 && (cd my282/libmsettings && make) >>/tmp/make.log 2>&1
	for d in minui minarch rasync wifi boxart controls clock ndslayer; do (cd all/$d && make) >>/tmp/make.log 2>&1 || { echo "FAIL $d" >&2; tail -5 /tmp/make.log | cut -c1-300 >&2; }; done
	grep -E " error|undefined|warning: implicit|multiple definition" /tmp/make.log >&2 || true
	for f in all/*/build/my282/*.elf all/ndslayer/build/my282/libndslayer.so; do
		n=$(/opt/my282-toolchain/usr/bin/arm-buildroot-linux-gnueabihf-objdump -d $f | grep -cE "\s(vmaxnm|vminnm|vrint[anpmxz]|vcvt[anpm]\.|vsel|crc32|lda|stl)" || true)
		[ "$n" = 0 ] || echo "ARMv8 instructions: $n in $f" >&2
	done
	tar -cf - all/*/build/my282/*.elf all/ndslayer/build/my282/lib* $(find my282/libmstick /root/workspace/my282 -maxdepth 2 -name libmstick.so | head -1)' | tar -xf - -C "$OUT"
"$REPO/tools/dev/build-https.sh"
echo "built into $OUT/all/*/build/my282/"
