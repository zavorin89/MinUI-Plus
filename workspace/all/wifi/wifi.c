// Wi-Fi: scan for networks, pick one, type its password on an on-screen keyboard, connect.
// Connecting is done by wifi.sh (in the system bin), which reads the networks saved here in wifi.txt: every one
// joined is remembered (the latest first), and wifi.sh joins whichever is in range by itself.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "tools.h"
#include "bgm.h"
#include "utils.h"
#include "keyboard.h"

#define CREDS_PATH SHARED_USERDATA_PATH "/wifi.txt"
#define FLAG_PATH SHARED_USERDATA_PATH "/.minui/wifi-on" // while set, MinUI's launch.sh leaves wlan0 alone
#define AUTOSTART_OFF_PATH SHARED_USERDATA_PATH "/.minui/wifi-autostart-off" // launch.sh skips reconnecting at boot
#define SCAN_CMD "iw dev wlan0 scan 2>/dev/null" // WIFI_SCAN_CMD overrides it (for testing off-device)
// once wpa_supplicant owns the adapter (eg. after connecting at boot) iw's scans are refused or stall, so ask it instead
#define WPA_CLI "wpa_cli -p /var/run/wpa_supplicant -i wlan0"
#define WPA_SCAN_CMD WPA_CLI " scan >/dev/null 2>&1; sleep 3; " WPA_CLI " scan_results 2>/dev/null"

#define MAX_NETWORKS 32
#define MAX_SSID 33 // 32 bytes plus terminator
#define MAX_PSK 64
#define MAX_SAVED 10 // the oldest is let go past this

///////////////////////////////

typedef struct Network {
	char ssid[MAX_SSID];
	int signal; // dBm
	int secure;
} Network;

static Network networks[MAX_NETWORKS];
static int network_count = 0;

typedef struct Saved {
	char ssid[MAX_SSID];
	char psk[MAX_PSK];
} Saved;
static Saved saved[MAX_SAVED]; // the latest joined first
static int saved_count = 0;
static char current_ssid[MAX_SSID]; // the network joined now, "" when none

static void readCreds(void) { // each network's ssid= line, then its psk= line (none for an open network)
	saved_count = 0;
	FILE* file = fopen(CREDS_PATH, "r");
	if (!file) return;
	char line[128];
	while (fgets(line, sizeof(line), file)) {
		normalizeNewline(line);
		trimTrailingNewlines(line);
		if (prefixMatch("ssid=", line) && line[5] && saved_count<MAX_SAVED) {
			Saved* network = &saved[saved_count++];
			snprintf(network->ssid, MAX_SSID, "%s", line+5);
			network->psk[0] = '\0';
		}
		else if (prefixMatch("psk=", line) && saved_count) snprintf(saved[saved_count-1].psk, MAX_PSK, "%s", line+4);
	}
	fclose(file);
}
static void writeCreds(void) {
	if (!saved_count) {
		unlink(CREDS_PATH);
		sync();
		return;
	}
	// written beside it and swapped in, so a power cut mid-write can't lose every saved network
	FILE* file = fopen(CREDS_PATH ".tmp", "w");
	if (!file) return;
	for (int i=0; i<saved_count; i++) {
		fprintf(file, "ssid=%s\n", saved[i].ssid);
		if (saved[i].psk[0]) fprintf(file, "psk=%s\n", saved[i].psk);
	}
	int ok = fflush(file)==0 && fsync(fileno(file))==0;
	fclose(file);
	if (ok) rename(CREDS_PATH ".tmp", CREDS_PATH);
	else unlink(CREDS_PATH ".tmp");
	sync();
}
static int findSaved(char* ssid) { // its place in saved, -1 when it isn't
	for (int i=0; i<saved_count; i++) if (exactMatch(saved[i].ssid, ssid)) return i;
	return -1;
}
static void forgetSaved(int i) {
	memmove(&saved[i], &saved[i+1], (saved_count - i - 1) * sizeof(Saved));
	saved_count -= 1;
}
static void rememberFirst(char* ssid, char* psk) { // saved (again) as the latest, so it's the one preferred
	int i = findSaved(ssid);
	if (i>=0) forgetSaved(i);
	else if (saved_count==MAX_SAVED) saved_count -= 1;
	memmove(&saved[1], &saved[0], saved_count * sizeof(Saved));
	snprintf(saved[0].ssid, MAX_SSID, "%s", ssid);
	snprintf(saved[0].psk, MAX_PSK, "%s", psk);
	saved_count += 1;
}
static void getCurrentSsid(void) {
	current_ssid[0] = '\0';
	FILE* pipe = popen("wifi.sh ssid 2>/dev/null", "r");
	if (!pipe) return;
	if (fgets(current_ssid, MAX_SSID, pipe)) trimTrailingNewlines(current_ssid);
	pclose(pipe);
}

static void getAddress(char* out) {
	out[0] = '\0';
	FILE* pipe = popen("wifi.sh status 2>/dev/null", "r");
	if (!pipe) return;
	if (fgets(out, 32, pipe)) trimTrailingNewlines(out);
	pclose(pipe);
}

static int Network_sortBySignal(const void* a, const void* b) {
	return ((Network*)b)->signal - ((Network*)a)->signal;
}
static void addNetwork(Network* found) {
	if (!found->ssid[0]) return; // hidden
	for (int i=0; i<network_count; i++) { // same name from several access points, keep the strongest
		if (!exactMatch(networks[i].ssid, found->ssid)) continue;
		if (found->signal>networks[i].signal) networks[i] = *found;
		return;
	}
	if (network_count<MAX_NETWORKS) networks[network_count++] = *found;
}
static void scanNetworks(void) {
	network_count = 0;
	char* cmd = getenv("WIFI_SCAN_CMD");
	int use_wpa = !cmd && system("pidof wpa_supplicant >/dev/null 2>&1")==0;
	if (!cmd) cmd = use_wpa ? WPA_SCAN_CMD : SCAN_CMD;

	// the first scan after bringing the interface up is often refused as busy
	for (int attempt=0; attempt<3 && network_count==0; attempt++) {
		if (attempt) sleep(1);
		FILE* pipe = popen(cmd, "r");
		if (!pipe) return;

		Network current = {0};
		int in_bss = 0;
		char line[256];
		while (fgets(line, sizeof(line), pipe)) {
			trimTrailingNewlines(line);
			if (use_wpa) { // "bssid<tab>frequency<tab>signal<tab>flags<tab>ssid" after a header line
				char* fields[5] = {0};
				int n = 0;
				for (char* field=strtok(line, "\t"); field && n<5; field=strtok(NULL, "\t")) fields[n++] = field;
				if (n<5 || !strchr(fields[0], ':')) continue;
				Network found = {0};
				found.signal = atoi(fields[2]);
				found.secure = strstr(fields[3], "WPA") || strstr(fields[3], "WEP");
				snprintf(found.ssid, MAX_SSID, "%s", fields[4]);
				addNetwork(&found);
				continue;
			}
			char* text = line;
			while (*text=='\t' || *text==' ') text += 1;

			if (prefixMatch("BSS ", line)) { // iw prints each access point as "BSS aa:bb:..." at column 0
				if (in_bss) addNetwork(&current);
				memset(&current, 0, sizeof(current));
				current.signal = -100;
				in_bss = 1;
			}
			else if (prefixMatch("signal:", text)) current.signal = atoi(text + strlen("signal:"));
			else if (prefixMatch("SSID: ", text)) snprintf(current.ssid, MAX_SSID, "%s", text + strlen("SSID: "));
			else if (prefixMatch("RSN:", text) || prefixMatch("WPA:", text)) current.secure = 1;
			else if (prefixMatch("capability:", text) && strstr(text, "Privacy")) current.secure = 1;
		}
		if (in_bss) addNetwork(&current);
		pclose(pipe);
	}
	qsort(networks, network_count, sizeof(Network), Network_sortBySignal);
}

static char* signalLabel(int dbm) {
	if (dbm>=-55) return "Strong";
	if (dbm>=-70) return "Good";
	return "Weak";
}

///////////////////////////////

///////////////////////////////

static SDL_Surface* screen;

static void blitText(TTF_Font* font, char* text, SDL_Color color, int x, int y) {
	SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text, color);
	if (!surface) return;
	SDL_BlitSurface(surface, NULL, screen, &(SDL_Rect){x, y});
	SDL_FreeSurface(surface);
}
#define LIST_ROWS (MAIN_ROW_COUNT - 1) // the rows shown under the header (no row shares the time and battery's line)

static void showMessage(char* msg, int wait) { // wait for A or B before returning
	int dirty = 1;
	int show_setting = 0;
	while (1) {
		GFX_startFrame();
		PAD_poll();
		if (wait && (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B))) break;
		PWR_update(&dirty, &show_setting, NULL, NULL);
		if (dirty) {
			GFX_clear(screen);
			GFX_blitHardwareGroup(screen, show_setting);
			GFX_blitMessage(font.large, msg, screen, &(SDL_Rect){0,0,screen->w,screen->h});
			if (wait) GFX_blitButtonGroup((char*[]){ "A","OKAY", NULL }, 0, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
		if (!wait) break; // drawn once, the caller does the slow work next
	}
}

///////////////////////////////

static int connectTo(char* ssid, char* psk, int quiet) { // returns 1 when connected; quiet: no message when it isn't
	Saved before[MAX_SAVED];
	int before_count = saved_count;
	memcpy(before, saved, sizeof(saved));

	char msg[128];
	snprintf(msg, sizeof(msg), "Connecting to %s...", ssid);
	showMessage(msg, 0);

	rememberFirst(ssid, psk); // first, so it's the one joined now (and preferred from now on)
	writeCreds();
	int ok = system("wifi.sh up first >/dev/null 2>&1")==0;
	if (ok) {
		snprintf(current_ssid, MAX_SSID, "%s", ssid);
		char address[32];
		getAddress(address);
		snprintf(msg, sizeof(msg), "Connected to %s\n%s", ssid, address);
	}
	else {
		// the saved networks as they were (a wrong password isn't kept), and back on one of them if one's in range
		memcpy(saved, before, sizeof(saved));
		saved_count = before_count;
		writeCreds();
		if (saved_count) system("wifi.sh up >/dev/null 2>&1 &");
		current_ssid[0] = '\0';
		snprintf(msg, sizeof(msg), "Couldn't connect to %s\nCheck the password and try again", ssid);
	}
	if (ok || !quiet) showMessage(msg, 1);
	return ok;
}

// forgets a saved network; off it, when it's the one joined, and onto another saved one in range (wifi stays on for
// when one's back)
static void forgetNetwork(char* ssid, char* address) {
	char name[MAX_SSID];
	snprintf(name, sizeof(name), "%s", ssid); // (ssid may be in saved, which moves)
	int i = findSaved(name);
	if (i<0) return;
	int was_current = address[0] && exactMatch(name, current_ssid);
	forgetSaved(i);
	writeCreds();
	if (was_current) {
		address[0] = '\0';
		current_ssid[0] = '\0';
		system(saved_count ? "wifi.sh up >/dev/null 2>&1 &" : "wifi.sh down >/dev/null 2>&1");
	}
	char msg[128];
	snprintf(msg, sizeof(msg), "Forgot %s", name);
	showMessage(msg, 1);
}
static Network* findNearby(char* ssid) { // in the last scan, NULL when it isn't
	for (int i=0; i<network_count; i++) if (exactMatch(networks[i].ssid, ssid)) return &networks[i];
	return NULL;
}

// every saved network, nearby or not (the latest joined first): join one, or forget it. Returns 1 when one was joined
static int savedList(char* address) {
	int joined = 0;
	int selected = 0;
	int start = 0;
	int dirty = 1;
	int show_setting = 0;
	while (saved_count) {
		GFX_startFrame();
		PAD_poll();
		if (selected>=saved_count) selected = saved_count - 1;

		if (PAD_justRepeated(BTN_UP)) { selected = (selected + saved_count - 1) % saved_count; dirty = 1; }
		else if (PAD_justRepeated(BTN_DOWN)) { selected = (selected + 1) % saved_count; dirty = 1; }
		else if (PAD_justPressed(BTN_B)) break;
		else if (PAD_justPressed(BTN_Y)) {
			forgetNetwork(saved[selected].ssid, address);
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_A)) {
			Saved network = saved[selected]; // (connecting moves it to the top)
			if (connectTo(network.ssid, network.psk, 0)) {
				getAddress(address);
				joined = 1;
				selected = 0;
			}
			dirty = 1;
		}

		if (selected<start) start = selected;
		if (selected>=start+LIST_ROWS) start = selected - LIST_ROWS + 1;

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (dirty) {
			GFX_clear(screen);
			GFX_blitHeader(screen, "Saved Networks", GFX_blitHardwareGroup(screen, show_setting));
			for (int i=start, j=0; i<saved_count && j<LIST_ROWS; i++, j++) {
				char name[128];
				int is_current = address[0] && exactMatch(saved[i].ssid, current_ssid);
				snprintf(name, sizeof(name), "%s%s", saved[i].ssid, is_current ? " \xE2\x80\xA2" : "");
				Network* nearby = findNearby(saved[i].ssid);
				char* value = nearby ? signalLabel(nearby->signal) : "Not nearby";

				int y = SCALE1(PADDING + (j+1)*PILL_SIZE);
				int width = screen->w - SCALE1(PADDING*2);
				int is_selected = i==selected;
				if (is_selected) GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){ SCALE1(PADDING), y, width, SCALE1(PILL_SIZE) });
				int value_w;
				TTF_SizeUTF8(font.small, value, &value_w, NULL);
				char shown[128];
				GFX_truncateText(font.large, name, shown, width - value_w - SCALE1(BUTTON_PADDING*3), 0);
				blitText(font.large, shown, is_selected ? COLOR_BLACK : COLOR_WHITE, SCALE1(PADDING + BUTTON_PADDING), y + SCALE1(4));
				SDL_Surface* text = TTF_RenderUTF8_Blended(font.small, value, is_selected ? COLOR_BLACK : COLOR_DARK_TEXT);
				if (text) {
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ SCALE1(PADDING) + width - SCALE1(BUTTON_PADDING) - text->w, y + (SCALE1(PILL_SIZE) - text->h)/2 });
					SDL_FreeSurface(text);
				}
			}
			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			else GFX_blitButtonGroup((char*[]){ "Y","FORGET", NULL }, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){ "B","BACK", "A","JOIN", NULL }, 1, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}
	return joined;
}

void Wifi_open(SDL_Surface* main_screen) {
	screen = main_screen;
	readCreds();
	int was_on = exists(FLAG_PATH);
	char address[32] = "";
	current_ssid[0] = '\0';
	if (was_on) {
		getAddress(address);
		getCurrentSsid();
	}

	// keep MinUI's background loop from switching the adapter back off while we scan
	if (!was_on) {
		touch(FLAG_PATH);
		showMessage("Scanning...", 0);
		sleep(2); // let the loop see the flag before bringing wlan0 up
	}
	else showMessage("Scanning...", 0);
	system("ifconfig wlan0 up 2>/dev/null");
	scanNetworks();

	int enabled = was_on; // shows the "turn off" row, and decides whether to switch the adapter off on exit
	int selected = 0;
	int start = 0;
	int quit = 0;
	int dirty = 1;
	int show_setting = 0;
	
	enum { ROW_TURN_OFF, ROW_AUTOSTART, ROW_SAVED, ROW_NETWORK };
	int row_types[MAX_NETWORKS+2];
	int row_networks[MAX_NETWORKS+2];
	
	while (!quit) {
		GFX_startFrame();
		PAD_poll();
		
		// rows: "turn off" while wifi is on, the startup setting, then the networks
		int total = 0;
		if (enabled) row_types[total++] = ROW_TURN_OFF;
		row_types[total++] = ROW_AUTOSTART;
		if (saved_count) row_types[total++] = ROW_SAVED;
		for (int i=0; i<network_count; i++) {
			row_types[total] = ROW_NETWORK;
			row_networks[total++] = i;
		}
		if (selected>=total) selected = total-1;
		
		if (PAD_justRepeated(BTN_UP)) { selected = (selected + total - 1) % total; dirty = 1; }
		else if (PAD_justRepeated(BTN_DOWN)) { selected = (selected + 1) % total; dirty = 1; }
		else if (PAD_justPressed(BTN_B)) quit = 1;
		else if (PAD_justPressed(BTN_X)) {
			showMessage("Scanning...", 0);
			scanNetworks();
			dirty = 1;
		}
		else if (row_types[selected]==ROW_AUTOSTART && (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_LEFT) || PAD_justPressed(BTN_RIGHT))) {
			if (exists(AUTOSTART_OFF_PATH)) unlink(AUTOSTART_OFF_PATH);
			else touch(AUTOSTART_OFF_PATH);
			sync();
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_Y) && row_types[selected]==ROW_NETWORK && findSaved(networks[row_networks[selected]].ssid)>=0) {
			forgetNetwork(networks[row_networks[selected]].ssid, address);
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_A) && row_types[selected]==ROW_SAVED) {
			if (savedList(address)) enabled = 1;
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_A)) {
			if (row_types[selected]==ROW_TURN_OFF) {
				unlink(FLAG_PATH);
				sync();
				system("wifi.sh down");
				enabled = 0;
				address[0] = '\0';
				showMessage("Wi-Fi is off", 1);
				quit = 1;
			}
			else if (row_types[selected]==ROW_NETWORK) {
				Network* network = &networks[row_networks[selected]];
				char psk[MAX_PSK] = "";
				char title[96];
				snprintf(title, sizeof(title), "Password for %s", network->ssid);
				int known = findSaved(network->ssid);
				int ok = 0;
				if (known>=0) { // its saved password, and when that's stopped working, ask for the new one
					snprintf(psk, sizeof(psk), "%s", saved[known].psk);
					ok = connectTo(network->ssid, psk, network->secure);
					if (!ok && network->secure && KB_enterText(screen, title, psk, MAX_PSK, "JOIN", 0)) ok = connectTo(network->ssid, psk, 0);
				}
				else if (!network->secure || KB_enterText(screen, title, psk, MAX_PSK, "JOIN", 0)) ok = connectTo(network->ssid, psk, 0);
				if (ok) {
					enabled = 1;
					getAddress(address);
					selected = 0;
				}
			}
			dirty = 1;
		}
		
		if (selected<start) start = selected;
		if (selected>=start+LIST_ROWS) start = selected - LIST_ROWS + 1;
		
		PWR_update(&dirty, &show_setting, NULL, NULL);
		
		if (dirty) {
			GFX_clear(screen);
			GFX_blitHeader(screen, "Wi-Fi Settings", GFX_blitHardwareGroup(screen, show_setting));
			
			for (int i=start, j=0; i<total && j<LIST_ROWS; i++, j++) {
				char name[128];
				char value[64];
				if (row_types[i]==ROW_TURN_OFF) {
					snprintf(name, sizeof(name), "Turn Wi-Fi Off");
					snprintf(value, sizeof(value), "%s", address[0] ? address : "Not connected");
				}
				else if (row_types[i]==ROW_AUTOSTART) {
					snprintf(name, sizeof(name), "Connect at Startup");
					snprintf(value, sizeof(value), "%s", exists(AUTOSTART_OFF_PATH) ? "Off" : "On");
				}
				else if (row_types[i]==ROW_SAVED) {
					snprintf(name, sizeof(name), "Saved Networks");
					snprintf(value, sizeof(value), "%i", saved_count);
				}
				else {
					Network* network = &networks[row_networks[i]];
					int is_current = address[0] && exactMatch(network->ssid, current_ssid);
					snprintf(name, sizeof(name), "%s%s", network->ssid, is_current ? " \xE2\x80\xA2" : ""); // dot like favorites
					snprintf(value, sizeof(value), "%s%s", findSaved(network->ssid)>=0 ? "Saved  " : (network->secure ? "" : "Open  "), signalLabel(network->signal));
				}
				
				int y = SCALE1(PADDING + (j+1)*PILL_SIZE);
				int width = screen->w - SCALE1(PADDING*2);
				int is_selected = i==selected;
				if (is_selected) GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){ SCALE1(PADDING), y, width, SCALE1(PILL_SIZE) });
				
				int value_w;
				TTF_SizeUTF8(font.small, value, &value_w, NULL);
				char shown[128];
				GFX_truncateText(font.large, name, shown, width - value_w - SCALE1(BUTTON_PADDING*3), 0);
				blitText(font.large, shown, is_selected ? COLOR_BLACK : COLOR_WHITE, SCALE1(PADDING + BUTTON_PADDING), y + SCALE1(4));
				
				SDL_Surface* text = TTF_RenderUTF8_Blended(font.small, value, is_selected ? COLOR_BLACK : COLOR_DARK_TEXT);
				if (text) {
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ SCALE1(PADDING) + width - SCALE1(BUTTON_PADDING) - text->w, y + (SCALE1(PILL_SIZE) - text->h)/2 });
					SDL_FreeSurface(text);
				}
			}
			if (network_count==0) {
				int y = SCALE1(PADDING + (total-start+1)*PILL_SIZE);
				GFX_blitMessage(font.large, "No networks found\nPress X to scan again", screen, &(SDL_Rect){0, y, screen->w, screen->h - y - SCALE1(PADDING+PILL_SIZE)});
			}
			
			char* action = "JOIN";
			if (row_types[selected]==ROW_TURN_OFF) action = "TURN OFF";
			else if (row_types[selected]==ROW_AUTOSTART) action = "CHANGE";
			else if (row_types[selected]==ROW_SAVED) action = "OPEN";
			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			else if (row_types[selected]==ROW_NETWORK && findSaved(networks[row_networks[selected]].ssid)>=0) GFX_blitButtonGroup((char*[]){ "X","SCAN", "Y","FORGET", NULL }, 0, screen, 0);
			else GFX_blitButtonGroup((char*[]){ "X","SCAN", NULL }, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){ "B","BACK", "A",action, NULL }, 1, screen, 1);
			
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}
	
	// leave wifi off again if it was off and nothing was joined
	if (!enabled) {
		unlink(FLAG_PATH);
		sync();
		system("wifi.sh down");
	}

}

#ifndef MINUI_TOOLS
int main(int argc , char* argv[]) {
	PWR_setCPUSpeed(CPU_SPEED_MENU);
	SDL_Surface* screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	InitSettings();
	SFX_forInput(1); // clicks for moving, choosing and going back
	BGM_start(); // the menus' music, when it's on
	Wifi_open(screen);
	BGM_quit();
	SFX_quit();
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return EXIT_SUCCESS;
}
#endif
