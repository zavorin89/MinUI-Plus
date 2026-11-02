#!/bin/sh

EMU_EXE=fbneo
CORE="$(dirname "$0")/../FBN.pak/${EMU_EXE}_libretro.so" # FBN.pak's, shared

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
minarch.elf "$CORE" "$ROM" > "$LOGS_PATH/$EMU_TAG.txt" 2>&1
