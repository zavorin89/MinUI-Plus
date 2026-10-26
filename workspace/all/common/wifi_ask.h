#ifndef WIFI_ASK_H
#define WIFI_ASK_H

#include "sdl.h"

// for a tool that needs the internet and found Wi-Fi off: says why, offers to turn it on (joining the network saved
// in Tools > Wi-Fi), and returns 1 once it's connected, 0 when it's left off
int WIFI_offer(SDL_Surface* screen, char* why);

#endif
