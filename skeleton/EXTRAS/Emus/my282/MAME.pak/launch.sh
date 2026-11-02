#!/bin/sh

EMU_EXE=mame2003_plus
CORE="$(dirname "$0")/${EMU_EXE}_libretro.so" # in this pak

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
minarch.elf "$CORE" "$ROM" > "$LOGS_PATH/$EMU_TAG.txt" 2>&1
