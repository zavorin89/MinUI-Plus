#!/bin/sh
# Builds the HTTPS-only downloader for the A30. Uses Go's TLS implementation, so the device does not depend on its
# non-TLS BusyBox wget. GO_BIN can point to another Go installation; the repo-local verified toolchain is the default.
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
GO_BIN="${GO_BIN:-$REPO/work/toolchains/go/bin/go}"
OUT="$REPO/tools/dev/out/all/https/build/my282"
[ -x "$GO_BIN" ] || { echo "Go toolchain missing: $GO_BIN" >&2; exit 1; }
mkdir -p "$OUT"
cd "$REPO/tools/https"
CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 "$GO_BIN" build -trimpath -buildvcs=false \
	-ldflags='-s -w -buildid=' -o "$OUT/https.elf" main.go
file "$OUT/https.elf"
