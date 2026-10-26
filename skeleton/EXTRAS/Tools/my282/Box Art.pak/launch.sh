#!/bin/sh
# Finds missing box art from the libretro thumbnail library and resizes all box art to 250 pixels wide.
cd "$(dirname "$0")"
./boxart.elf "$@" # a game's path finds art for just that game
