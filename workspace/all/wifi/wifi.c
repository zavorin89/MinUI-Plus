// Wi-Fi: scan for networks, pick one, type its password on an on-screen keyboard, connect.
// Connecting is done by wifi.sh (in the system bin), which reads the network saved here in wifi.txt.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
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

///////////////////////////////

typedef struct Network {
	char ssid[MAX_SSID];
	int signal; // dBm
	int secure;
} Network;

static Network networks[MAX_NETWORKS];
static int network_count = 0;

static char saved_ssid[MAX_SSID];
static char saved_psk[MAX_PSK];

static void readCreds(void) {
	saved_ssid[0] = '\0';
	saved_psk[0] = '\0';
	FILE* file = fopen(CREDS_PATH, "r");
	if (!file) return;
	char line[128];
	while (fgets(line, sizeof(line), file)) {
		normalizeNewline(line);
		trimTrailingNewlines(line);
		if (prefixMatch("ssid=", line)) snprintf(saved_ssid, MAX_SSID, "%s", line+5);
		else if (prefixMatch("psk=", line)) snprintf(saved_psk, MAX_PSK, "%s", line+4);
	}
	fclose(file);
}
static void writeCreds(char* ssid, char* psk) {
	FILE* file = fopen(CREDS_PATH, "w");
	if (!file) return;
	fprintf(file, "ssid=%s\npsk=%s\n", ssid, psk);
	fclose(file);
	sync();
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
	SDL_BlitSurface(surface, NULL, screen, &(SDL_Rect){x, y});
	SDL_FreeSurface(surface);
}
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

static int connectTo(char* ssid, char* psk) { // returns 1 when connected
	char old_ssid[MAX_SSID], old_psk[MAX_PSK];
	strcpy(old_ssid, saved_ssid);
	strcpy(old_psk, saved_psk);

	char msg[128];
	snprintf(msg, sizeof(msg), "Connecting to %s...", ssid);
	showMessage(msg, 0);

	writeCreds(ssid, psk);
	int ok = system("wifi.sh up >/dev/null 2>&1")==0;
	if (ok) {
		strcpy(saved_ssid, ssid);
		strcpy(saved_psk, psk);
		char address[32];
		getAddress(address);
		snprintf(msg, sizeof(msg), "Connected to %s\n%s", ssid, address);
	}
	else {
		// put the previous network back so reconnecting at boot still works
		if (old_ssid[0]) writeCreds(old_ssid, old_psk);
		else unlink(CREDS_PATH);
		snprintf(msg, sizeof(msg), "Couldn't connect to %s\nCheck the password and try again", ssid);
	}
	showMessage(msg, 1);
	return ok;
}

int main(int argc , char* argv[]) {
	PWR_setCPUSpeed(CPU_SPEED_MENU);

	screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	InitSettings();
	SFX_forInput(1); // clicks for moving, choosing and going back
	BGM_start(); // the menus' music, when it's on

	readCreds();
	int was_on = exists(FLAG_PATH);
	char address[32] = "";
	if (was_on) getAddress(address);

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
	
	enum { ROW_TURN_OFF, ROW_AUTOSTART, ROW_NETWORK };
	int row_types[MAX_NETWORKS+2];
	int row_networks[MAX_NETWORKS+2];
	
	while (!quit) {
		GFX_startFrame();
		PAD_poll();
		
		// rows: "turn off" while wifi is on, the startup setting, then the networks
		int total = 0;
		if (enabled) row_types[total++] = ROW_TURN_OFF;
		row_types[total++] = ROW_AUTOSTART;
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
				int ready = 1;
				if (exactMatch(network->ssid, saved_ssid)) strcpy(psk, saved_psk); // known network
				else if (network->secure) {
					char title[96];
					snprintf(title, sizeof(title), "Password for %s", network->ssid);
					ready = KB_enterText(screen, title, psk, MAX_PSK, "JOIN", 0);
				}
				if (ready && connectTo(network->ssid, psk)) {
					enabled = 1;
					getAddress(address);
					selected = 0;
				}
			}
			dirty = 1;
		}
		
		if (selected<start) start = selected;
		if (selected>=start+MAIN_ROW_COUNT) start = selected - MAIN_ROW_COUNT + 1;
		
		PWR_update(&dirty, &show_setting, NULL, NULL);
		
		if (dirty) {
			GFX_clear(screen);
			int ow = GFX_blitHardwareGroup(screen, show_setting);
			
			for (int i=start, j=0; i<total && j<MAIN_ROW_COUNT; i++, j++) {
				char name[128];
				char value[64];
				if (row_types[i]==ROW_TURN_OFF) {
					snprintf(name, sizeof(name), "Turn Wi-Fi off");
					snprintf(value, sizeof(value), "%s", address[0] ? address : "Not connected");
				}
				else if (row_types[i]==ROW_AUTOSTART) {
					snprintf(name, sizeof(name), "Connect at startup");
					snprintf(value, sizeof(value), "%s", exists(AUTOSTART_OFF_PATH) ? "Off" : "On");
				}
				else {
					Network* network = &networks[row_networks[i]];
					int is_current = address[0] && exactMatch(network->ssid, saved_ssid);
					snprintf(name, sizeof(name), "%s%s", network->ssid, is_current ? " \xE2\x80\xA2" : ""); // dot like favorites
					snprintf(value, sizeof(value), "%s%s", network->secure ? "" : "Open  ", signalLabel(network->signal));
				}
				
				int y = SCALE1(PADDING + j*PILL_SIZE);
				int width = screen->w - SCALE1(PADDING*2);
				if (j==0) width -= ow; // first row shares space with the battery/wifi group
				int is_selected = i==selected;
				if (is_selected) GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){ SCALE1(PADDING), y, width, SCALE1(PILL_SIZE) });
				
				int value_w;
				TTF_SizeUTF8(font.small, value, &value_w, NULL);
				char shown[128];
				GFX_truncateText(font.large, name, shown, width - value_w - SCALE1(BUTTON_PADDING*3), 0);
				blitText(font.large, shown, is_selected ? COLOR_BLACK : COLOR_WHITE, SCALE1(PADDING + BUTTON_PADDING), y + SCALE1(4));
				
				SDL_Surface* text = TTF_RenderUTF8_Blended(font.small, value, is_selected ? COLOR_BLACK : COLOR_DARK_TEXT);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ SCALE1(PADDING) + width - SCALE1(BUTTON_PADDING) - text->w, y + (SCALE1(PILL_SIZE) - text->h)/2 });
				SDL_FreeSurface(text);
			}
			if (network_count==0) {
				int y = SCALE1(PADDING + (total-start)*PILL_SIZE);
				GFX_blitMessage(font.large, "No networks found\nPress X to scan again", screen, &(SDL_Rect){0, y, screen->w, screen->h - y - SCALE1(PADDING+PILL_SIZE)});
			}
			
			char* action = "JOIN";
			if (row_types[selected]==ROW_TURN_OFF) action = "TURN OFF";
			else if (row_types[selected]==ROW_AUTOSTART) action = "CHANGE";
			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
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

	BGM_quit();
	SFX_quit();
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return EXIT_SUCCESS;
}
