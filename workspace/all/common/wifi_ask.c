// "Turn on Wi-Fi?" for tools that need the internet, doing what the Wi-Fi setup (Settings > Wi-Fi) does to connect

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "defines.h"
#include "api.h"
#include "utils.h"
#include "wifi_ask.h"

#define WIFI_CREDS_PATH SHARED_USERDATA_PATH "/wifi.txt"
#define WIFI_FLAG_PATH SHARED_USERDATA_PATH "/.minui/wifi-on" // while set, MinUI's launch.sh leaves wlan0 alone

// a message, with buttons when pairs is set (waiting for A or B then, returning 1 for A)
static int ask(SDL_Surface* screen, char* message, char** pairs) {
	int dirty = 1;
	int show_setting = 0;
	while (1) {
		GFX_startFrame();
		PAD_poll();
		if (pairs && PAD_justPressed(BTN_A)) return 1;
		if (pairs && PAD_justPressed(BTN_B)) return 0;
		PWR_update(&dirty, &show_setting, NULL, NULL);
		if (dirty) {
			GFX_clear(screen);
			GFX_blitHardwareGroup(screen, show_setting);
			GFX_blitMessage(font.large, message, screen, &(SDL_Rect){0,0,screen->w,screen->h});
			if (pairs) GFX_blitButtonGroup(pairs, 1, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
		if (!pairs) return 1;
	}
}

static void savedNetwork(char* ssid, int size) {
	ssid[0] = '\0';
	FILE* file = fopen(WIFI_CREDS_PATH, "r");
	if (!file) return;
	char line[256];
	while (fgets(line, sizeof(line), file)) {
		normalizeNewline(line);
		trimTrailingNewlines(line);
		if (prefixMatch("ssid=", line)) snprintf(ssid, size, "%s", line+5);
	}
	fclose(file);
}

int WIFI_offer(SDL_Surface* screen, char* why) {
	char ssid[128];
	savedNetwork(ssid, sizeof(ssid));
	char message[512];
	if (!ssid[0]) {
		snprintf(message, sizeof(message), "%s\nNo Wi-Fi network is saved yet:\njoin one in Settings > Wi-Fi first", why);
		ask(screen, message, (char*[]){ "A","OKAY", NULL });
		return 0;
	}
	snprintf(message, sizeof(message), "%s\nWi-Fi is off. Turn it on?", why);
	if (!ask(screen, message, (char*[]){ "B","CANCEL", "A","TURN ON", NULL })) return 0;

	snprintf(message, sizeof(message), "Connecting to %s...", ssid);
	ask(screen, message, NULL);
	touch(WIFI_FLAG_PATH);
	sync();
	sleep(2); // let launch.sh's loop see the flag before wlan0 comes up, or it switches it back off
	if (system("wifi.sh up >/dev/null 2>&1")==0) return 1;

	unlink(WIFI_FLAG_PATH);
	sync();
	system("wifi.sh down >/dev/null 2>&1");
	snprintf(message, sizeof(message), "Couldn't connect to %s\nCheck it in Settings > Wi-Fi", ssid);
	ask(screen, message, (char*[]){ "A","OKAY", NULL });
	return 0;
}
