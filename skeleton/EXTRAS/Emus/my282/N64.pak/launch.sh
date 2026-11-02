#!/bin/sh

EMU_EXE=parallel_n64
CORE="$(dirname "$0")/${EMU_EXE}_libretro.so" # in this pak

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
export MINARCH_GLES=1 # it draws with the GPU (see minarch's hwrender.c)
export MINARCH_GLES_DIRECT=1 # and its frames go straight to the screen, when nothing else is needed over them
export MINARCH_N64_SKIP_DRAWING=1 # and when it falls behind, a frame isn't drawn at all rather than drawn and hidden
minarch.elf "$CORE" "$ROM" > "$LOGS_PATH/$EMU_TAG.txt" 2>&1
