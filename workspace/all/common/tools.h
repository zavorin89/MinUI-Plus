#ifndef TOOLS_H
#define TOOLS_H

#include "sdl.h"

// MinUI+'s tools, built into the launcher so they open at once like Play Time (no process to start, and no launcher
// to start again after). Each returns to whatever called it, on its last frame. Each also still builds alone, as its
// pak's .elf, with a main() around the same function (when MINUI_TOOLS isn't defined)
void Controls_open(SDL_Surface* screen);
void Wifi_open(SDL_Surface* screen);
void BoxArt_open(SDL_Surface* screen, char* game); // game: just that one game's art, NULL: the tool's own menu
void RetroAchievements_open(SDL_Surface* screen);

#endif
