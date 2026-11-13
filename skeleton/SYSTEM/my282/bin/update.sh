#!/bin/sh
# update.sh check|install
# Quietly checks the latest stable release on GitLab, or downloads, verifies and installs the update the launcher offered.

SDCARD_PATH="${SDCARD_PATH:-/mnt/SDCARD}"
SYSTEM_PATH="${SYSTEM_PATH:-$SDCARD_PATH/.system/my282}"
SHARED_USERDATA_PATH="${SHARED_USERDATA_PATH:-$SDCARD_PATH/.userdata/shared}"
STATE_DIR="$SHARED_USERDATA_PATH/.minui"
AVAILABLE="$STATE_DIR/update-available.txt"
NOTES="$STATE_DIR/update-notes.txt"
VERSION="$SDCARD_PATH/.system/version.txt"
LOG="${LOGS_PATH:-$SDCARD_PATH/.userdata/my282/logs}/update.txt"
GITLAB_API="https://gitlab.com/api/v4/projects/zavorin89-group%2FMinUI-Plus/releases/permalink/latest"
GITLAB_DOWNLOADS="https://gitlab.com/zavorin89-group/MinUI-Plus/-/releases/"
HTTPS="$SYSTEM_PATH/bin/https.elf"
CA_CERT="$SDCARD_PATH/.system/res/cacert.pem"
RUNTIME_PATH="${MINUI_RUNTIME_PATH:-/tmp}"
CHECKED="$RUNTIME_PATH/minui_update_checked"
CHECK_LOCK="$RUNTIME_PATH/minui_update_check.lock"
HTTPS_ERROR="$RUNTIME_PATH/minui_https_error"
UNZIP="$SDCARD_PATH/.tmp_update/my282/unzip"
DOWNLOAD="$SDCARD_PATH/.minui-update.zip"
PART="$DOWNLOAD.part"
UPDATE_IMAGE="$SDCARD_PATH/.tmp_update/my282/updating.png"
PROGRESS="$RUNTIME_PATH/minui_update_progress"

mkdir -p "$STATE_DIR" "$(dirname "$LOG")"

log() {
	echo "$(date +'%F %T') $*" >> "$LOG"
}

# whether release A (MinUIPlus-YYYYMMDD-N, with or without .zip) is newer than B. Anything not named like that
# (stock MinUI, a development card) counts as oldest, so it's offered the latest. The date and the number are
# compared apart: as one 11-digit number they'd overflow the A30's 32-bit shell arithmetic
release_parts() { # "DATE N", or "0 0"
	NAME=${1%.zip}
	case "$NAME" in
		MinUIPlus-[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]-*) ;;
		*) echo "0 0"; return ;;
	esac
	REST=${NAME#MinUIPlus-}; DOT=${REST#*-}
	case "$DOT" in *[!0-9]*|'') echo "0 0"; return ;; esac
	echo "${REST%%-*} $DOT"
}
release_newer() {
	set -- $(release_parts "$1") $(release_parts "$2")
	[ "$1" -gt "$3" ] || { [ "$1" -eq "$3" ] && [ "$2" -gt "$4" ]; }
}

# Fetches one source's latest release into TAG, ASSET, URL, DIGEST, SIZE and BODY: read_release NAME API DOWNLOADS
read_release() {
	# An address can exist a moment before DNS/internet access is ready, especially with phone hotspots. Retry like
	# the clock sync does, but a server that answered "no" (a 4xx, like a missing release) won't change its mind.
	RELEASE=
	n=0
	while [ $n -lt 4 ]; do
		RAW=$(CA_CERT_PATH="$CA_CERT" "$HTTPS" -release -t 25s "$2" 2>"$HTTPS_ERROR")
		if [ $? -eq 0 ] && [ -n "$RAW" ]; then
			RELEASE="$RAW"
			break
		fi
		grep -q 'server returned 4' "$HTTPS_ERROR" 2>/dev/null && break
		n=$((n+1))
		[ $n -lt 4 ] && sleep 3
	done
	if [ -z "$RELEASE" ]; then
		DETAIL=$(tail -n 1 "$HTTPS_ERROR" 2>/dev/null | tr '\r\n' ' ')
		log "$1 check failed: ${DETAIL:-unknown HTTPS error}"
		rm -f "$HTTPS_ERROR"
		return 1
	fi
	rm -f "$HTTPS_ERROR"
	# The HTTPS helper parses JSON itself; BusyBox only reads its five fixed lines followed by the release notes.
	TAG=$(printf '%s\n' "$RELEASE" | sed -n '1p')
	ASSET=$(printf '%s\n' "$RELEASE" | sed -n '2p')
	URL=$(printf '%s\n' "$RELEASE" | sed -n '3p')
	DIGEST=$(printf '%s\n' "$RELEASE" | sed -n '4p')
	SIZE=$(printf '%s\n' "$RELEASE" | sed -n '5p')
	BODY=$(printf '%s\n' "$RELEASE" | sed '1,5d')
	URL_STATE=missing; [ -n "$URL" ] && URL_STATE=present
	case "$TAG|$ASSET|$URL" in
		v*'|MinUIPlus-'*".zip|$3"*) ;;
		*) log "$1 check failed: release fields tag=${TAG:-missing}, asset=${ASSET:-missing}, url=$URL_STATE"; return 1 ;;
	esac
	case "$DIGEST" in *[!0-9a-fA-F]*|'') log "$1 check failed: the release has no SHA-256"; return 1 ;; esac
	[ ${#DIGEST} -eq 64 ] || { log "$1 check failed: the release SHA-256 is invalid"; return 1; }
	case "$SIZE" in *[!0-9]*|'') log "$1 check failed: the release size is invalid"; return 1 ;; esac
}

check() {
	# Reconnecting after sleep should not spend GitLab's anonymous API allowance: check at most once an hour.
	NOW=$(date +%s)
	LAST=$(cat "$CHECKED" 2>/dev/null)
	case "$LAST" in *[!0-9]*|'') LAST=0 ;; esac
	# A clock correction backwards must not suppress checks until the old time catches up.
	if [ "$NOW" -ge "$LAST" ] && [ $((NOW - LAST)) -lt 3600 ]; then exit 0; fi
	mkdir "$CHECK_LOCK" 2>/dev/null || exit 0
	trap 'rmdir "$CHECK_LOCK" 2>/dev/null' EXIT INT TERM
	[ -x "$HTTPS" ] || { log "check failed: the HTTPS downloader is missing"; exit 1; }
	[ -f "$CA_CERT" ] || { log "check failed: the CA bundle is missing"; exit 1; }

	read_release GitLab "$GITLAB_API" "$GITLAB_DOWNLOADS" || exit 1

	CURRENT=$(sed -n '1p' "$VERSION" 2>/dev/null)
	if ! release_newer "$ASSET" "$CURRENT"; then # the installed one, or older: nothing to offer
		rm -f "$AVAILABLE" "$NOTES"
		echo "$NOW" > "$CHECKED"
		log "up to date: $TAG ($CURRENT)"
		exit 0
	fi

	# Remove the small amount of Markdown the menu cannot render.
	[ -n "$BODY" ] || BODY="No patch notes were included with this release."
	printf '%s\n' "$BODY" | sed 's/\r//g; s/\*\*//g; s/`//g; s/^### //; s/^## //; s/^# //' > "$NOTES.tmp"
	{
		echo "$TAG"
		echo "$ASSET"
		echo "$URL"
		echo "$DIGEST"
		echo "$SIZE"
	} > "$AVAILABLE.tmp"
	mv "$NOTES.tmp" "$NOTES"
	mv "$AVAILABLE.tmp" "$AVAILABLE" # last: the launcher never sees metadata without its notes
	echo "$NOW" > "$CHECKED"
	log "available: $TAG ($ASSET)"
}

fail() {
	log "install failed: $*"
	rm -f "$PART" "$PROGRESS"
	if [ -n "$DOWNLOAD_PID" ]; then kill "$DOWNLOAD_PID" 2>/dev/null; wait "$DOWNLOAD_PID" 2>/dev/null; fi
	stop_show
	if [ "$UNPACK_STARTED" = 1 ]; then
		RECOVERY="Some update files may have been copied.
Retry the update before shutting down."
	else
		RECOVERY="Nothing was installed."
	fi
	say.elf "Update failed
$*

$RECOVERY"
	exit 1
}

stop_show() {
	[ -n "$SHOW_PID" ] || return 0
	kill "$SHOW_PID" 2>/dev/null
	# A display helper must never prevent an error or restart prompt. Fixed show.elf exits cleanly on the first
	# signal; this watchdog is only a fallback for an old or damaged binary.
	(sleep 3; kill -9 "$SHOW_PID" 2>/dev/null) &
	SHOW_WATCHDOG_PID=$!
	wait "$SHOW_PID" 2>/dev/null
	kill "$SHOW_WATCHDOG_PID" 2>/dev/null
	wait "$SHOW_WATCHDOG_PID" 2>/dev/null
	SHOW_PID=
}

install_update() {
	[ -f "$AVAILABLE" ] || fail "Check for updates again"
	TAG=$(sed -n '1p' "$AVAILABLE")
	ASSET=$(sed -n '2p' "$AVAILABLE")
	URL=$(sed -n '3p' "$AVAILABLE")
	DIGEST=$(sed -n '4p' "$AVAILABLE")
	SIZE=$(sed -n '5p' "$AVAILABLE")
	case "$TAG|$ASSET|$URL" in
		v*'|MinUIPlus-'*".zip|$GITLAB_DOWNLOADS"*) ;;
		*) fail "The update information is invalid" ;;
	esac
	case "$DIGEST" in *[!0-9a-fA-F]*|'') fail "The update has no SHA-256" ;; esac
	[ ${#DIGEST} -eq 64 ] || fail "The update SHA-256 is invalid"
	case "$SIZE" in *[!0-9]*|'') fail "The update size is invalid" ;; esac
	[ "$SIZE" -gt 0 ] || fail "The update size is invalid"
	[ -x "$HTTPS" ] || fail "The HTTPS downloader is missing"
	[ -f "$CA_CERT" ] || fail "The CA bundle is missing"
	[ -x "$UNZIP" ] || fail "The system unzip tool is missing"
	FREE_KB=$(df -k "$SDCARD_PATH" 2>/dev/null | awk 'END {print $4}')
	case "$FREE_KB" in *[!0-9]*|'') FREE_KB=0 ;; esac
	NEEDED_KB=$((SIZE / 1024 * 3 + 16384)) # download plus room to merge its compressed and uncompressed contents
	[ "$FREE_KB" -ge "$NEEDED_KB" ] || fail "The card needs more free space"

	echo 2 > "$PROGRESS"
	show.elf "$UPDATE_IMAGE" 2400 "$PROGRESS" & # (outlasts the download, with room to verify and unpack)
	SHOW_PID=$!
	log "downloading: $TAG ($ASSET)"
	rm -f "$PART" "$DOWNLOAD"
	CA_CERT_PATH="$CA_CERT" "$HTTPS" -t 30m "$URL" "$PART" 2>>"$LOG" &
	DOWNLOAD_PID=$!
	while kill -0 "$DOWNLOAD_PID" 2>/dev/null; do
		BYTES=0
		[ -f "$PART" ] && BYTES=$(wc -c < "$PART" 2>/dev/null)
		case "$BYTES" in *[!0-9]*|'') BYTES=0 ;; esac
		PERCENT=$((5 + BYTES / (SIZE / 70 + 1))) # avoids overflowing the A30's 32-bit shell arithmetic
		[ "$PERCENT" -gt 75 ] && PERCENT=75
		echo "$PERCENT" > "$PROGRESS"
		sleep 1
	done
	wait "$DOWNLOAD_PID" || fail "The download did not finish"
	DOWNLOAD_PID=
	mv "$PART" "$DOWNLOAD"

	echo 80 > "$PROGRESS"
	command -v sha256sum >/dev/null 2>&1 || fail "SHA-256 verification is unavailable"
	ACTUAL=$(sha256sum "$DOWNLOAD" | sed 's/ .*//')
	[ "$ACTUAL" = "$DIGEST" ] || fail "The download did not verify"
	"$UNZIP" -t "$DOWNLOAD" >/dev/null 2>&1 || fail "The downloaded ZIP is damaged"

	# Merge more than MinUI.zip (patches also update tools, emulator paks and art), in two passes: MinUI+'s own files
	# are replaced, while the folders people make their own (system art, bezels, songs, BIOS, saves) only gain what's
	# new, so a customized one is never overwritten. The first-install miyoo folder isn't needed on an installed card.
	# (unzip exits 11 when a pattern matches nothing, 1 for warnings: neither is a failure)
	echo 88 > "$PROGRESS"
	UNPACK_STARTED=1
	"$UNZIP" -o "$DOWNLOAD" MinUI.zip README.txt CHANGES.txt 'Tools/*' 'Emus/*' '.res/*' -d "$SDCARD_PATH" >>"$LOG" 2>&1
	case $? in 0|1|11) ;; *) fail "The update could not be unpacked" ;; esac
	echo 94 > "$PROGRESS"
	"$UNZIP" -n "$DOWNLOAD" 'Roms/*' 'Bezels/*' 'BGM/*' 'Bios/*' 'Saves/*' -d "$SDCARD_PATH" >>"$LOG" 2>&1
	case $? in 0|1|11) ;; *) fail "The update could not be unpacked" ;; esac
	[ -f "$SDCARD_PATH/MinUI.zip" ] || fail "The update did not contain MinUI.zip"
	rm -f "$DOWNLOAD" "$AVAILABLE" "$NOTES"
	echo 100 > "$PROGRESS"
	sync
	log "installed: $TAG; waiting for restart confirmation"
	stop_show
	rm -f "$PROGRESS"
	if say.elf --confirm "Update ready
Shut down, then turn the A30 on to finish."; then
		log "shutdown confirmed: $TAG"
		shutdown
	else
		log "shutdown deferred: $TAG"
	fi
}

case "$1" in
	check) check ;;
	install) install_update ;;
	*) echo "usage: $0 check|install" >&2; exit 2 ;;
esac
