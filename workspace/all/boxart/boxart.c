// Box Art: finds missing box art for every game on the card from the libretro thumbnail library (no account needed),
// and resizes all box art to 250 pixels wide so it loads quickly and looks the same everywhere.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zlib.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "ra_http.h"
#include "ra_cache.h" // RA_CA_PATH
#include "wifi_ask.h"

#define ART_WIDTH 250
#define USER_AGENT "MinUI/" PLATFORM " box art"
#define THUMBS_URL "https://thumbnails.libretro.com/"
#define LIST_DIR SHARED_USERDATA_PATH "/.boxart" // each system's list of available art, so it's fetched once
#define LIST_MAX_AGE (14*24*60*60)
#define LOG_PATH USERDATA_PATH "/logs/box-art.txt"
#define MAX_GAMES 4096

static SDL_Surface* screen;
static FILE* log_file = NULL; // which games had no match, so they can be fixed by hand
#define ART_LOG(...) do { if (log_file) { fprintf(log_file, __VA_ARGS__); fflush(log_file); } } while (0)

///////////////////////////////

// MinUI's folder tags and the libretro library's name for each system
static const char* systems[][2] = {
	{ "GBA", "Nintendo - Game Boy Advance" },
	{ "MGBA", "Nintendo - Game Boy Advance" },
	{ "GBC", "Nintendo - Game Boy Color" },
	{ "GB", "Nintendo - Game Boy" },
	{ "SGB", "Nintendo - Game Boy" },
	{ "FC", "Nintendo - Nintendo Entertainment System" },
	{ "NES", "Nintendo - Nintendo Entertainment System" },
	{ "SFC", "Nintendo - Super Nintendo Entertainment System" },
	{ "SNES", "Nintendo - Super Nintendo Entertainment System" },
	{ "N64", "Nintendo - Nintendo 64" },
	{ "NDS", "Nintendo - Nintendo DS" },
	{ "VB", "Nintendo - Virtual Boy" },
	{ "PKM", "Nintendo - Pokemon Mini" },
	{ "MD", "Sega - Mega Drive - Genesis" },
	{ "GG", "Sega - Game Gear" },
	{ "SMS", "Sega - Master System - Mark III" },
	{ "SEGACD", "Sega - Mega-CD - Sega CD" },
	{ "32X", "Sega - 32X" },
	{ "PS", "Sony - PlayStation" },
	{ "PCE", "NEC - PC Engine - TurboGrafx 16" },
	{ "NGP", "SNK - Neo Geo Pocket" },
	{ "NGPC", "SNK - Neo Geo Pocket Color" },
	{ "WS", "Bandai - WonderSwan" },
	{ "WSC", "Bandai - WonderSwan Color" },
	{ "LYNX", "Atari - Lynx" },
	{ "A2600", "Atari - 2600" },
	{ NULL, NULL },
};
// the library's name for a Roms folder's system, NULL when it has none
static const char* librarySystem(char* folder) {
	char probe[MAX_PATH], tag[MAX_PATH];
	sprintf(probe, "%s/x", folder);
	getEmuName(probe, tag);
	for (int i=0; systems[i][0]; i++) if (!strcasecmp(systems[i][0], tag)) return systems[i][1];
	// variants like SFC10 or SNESX share their base system's art
	for (int i=0; systems[i][0]; i++) if (prefixMatch((char*)systems[i][0], tag) && strlen(systems[i][0])>=2) return systems[i][1];
	return NULL;
}

static int isGameFile(char* name) {
	if (name[0]=='.') return 0;
	char* skip[] = { ".txt", ".xml", ".png", ".jpg", ".jpeg", ".dat", ".ini", ".cfg", ".sav", ".srm", ".m3u", ".bak", ".log", NULL };
	for (int i=0; skip[i]; i++) if (suffixMatch(skip[i], name)) return 0;
	return 1;
}
// a game's name without its extension (folders, eg. multi-disc ones, have none)
static void gameName(char* file, int is_dir, char* name) {
	strcpy(name, file);
	if (is_dir) return;
	char* dot = strrchr(name, '.');
	if (dot && dot!=name) *dot = '\0';
}

///////////////////////////////

static void showMessage(char* msg, int wait) {
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
		if (!wait) break;
	}
}

// title, game, a bar that fills as games are done, and a count
static void showProgress(char* title, int done, int count, char* name) {
	int show_setting = 0;
	int dirty = 1;
	GFX_startFrame();
	PWR_update(&dirty, &show_setting, NULL, NULL);
	GFX_clear(screen);
	GFX_blitHardwareGroup(screen, show_setting);

	int width = screen->w - SCALE1(PADDING*4);
	int x = SCALE1(PADDING*2);
	int y = screen->h/2 - SCALE1(PILL_SIZE*2);

	SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
	SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ (screen->w-text->w)/2, y });
	y += text->h + SCALE1(4);
	SDL_FreeSurface(text);

	char shown[256];
	GFX_truncateText(font.small, name, shown, width, 0);
	text = TTF_RenderUTF8_Blended(font.small, shown, COLOR_DARK_TEXT);
	SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ (screen->w-text->w)/2, y });
	y += text->h + SCALE1(PADDING);
	SDL_FreeSurface(text);

	int bar_h = SCALE1(20);
	GFX_blitRect(ASSET_OPTION, screen, &(SDL_Rect){ x, y, width, bar_h });
	int fill = count ? (int)((long)width * done / count) : 0;
	if (fill>0) GFX_blitRect(ASSET_BUTTON, screen, &(SDL_Rect){ x, y, MAX(fill, bar_h), bar_h });
	y += bar_h + SCALE1(6);

	char status[128];
	snprintf(status, sizeof(status), "%i of %i (%i%% done)", MIN(done+1, count), count, count ? done*100/count : 0);
	text = TTF_RenderUTF8_Blended(font.small, status, COLOR_WHITE);
	SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ (screen->w-text->w)/2, y });
	SDL_FreeSurface(text);

	GFX_blitButtonGroup((char*[]){ "B","CANCEL", NULL }, 0, screen, 1);
	GFX_flip(screen);
}
static int cancelled(void) {
	PAD_poll();
	return PAD_justPressed(BTN_B);
}

static int hasWifi(void) {
	char address[32] = "";
	FILE* pipe = popen("wifi.sh status 2>/dev/null", "r");
	if (pipe) {
		if (fgets(address, sizeof(address), pipe)) trimTrailingNewlines(address);
		pclose(pipe);
	}
	return address[0]!='\0';
}

///////////////////////////////

// PNG writing with zlib, so this doesn't depend on the firmware's SDL_image being able to save
static void putBE32(unsigned char* p, uint32_t v) {
	p[0] = v>>24; p[1] = v>>16; p[2] = v>>8; p[3] = v;
}
static void writeChunk(FILE* file, const char* type, const unsigned char* data, uint32_t length) {
	unsigned char head[8];
	putBE32(head, length);
	memcpy(head+4, type, 4);
	fwrite(head, 1, 8, file);
	if (length) fwrite(data, 1, length, file);
	uLong crc = crc32(0, (const Bytef*)type, 4);
	if (length) crc = crc32(crc, data, length);
	unsigned char tail[4];
	putBE32(tail, crc);
	fwrite(tail, 1, 4, file);
}
static int savePNG(const char* path, const uint8_t* rgba, int w, int h) { // rgba rows, 4 bytes per pixel
	size_t raw_size = (size_t)h * (w*4 + 1);
	unsigned char* raw = malloc(raw_size);
	for (int y=0; y<h; y++) {
		raw[y*(w*4+1)] = 0; // no filter
		memcpy(raw + y*(w*4+1) + 1, rgba + (size_t)y*w*4, w*4);
	}
	uLongf packed_size = compressBound(raw_size);
	unsigned char* packed = malloc(packed_size);
	int ok = compress2(packed, &packed_size, raw, raw_size, 6)==Z_OK;
	free(raw);
	if (!ok) { free(packed); return 0; }

	char tmp_path[MAX_PATH];
	sprintf(tmp_path, "%s.tmp", path);
	FILE* file = fopen(tmp_path, "wb");
	if (!file) { free(packed); return 0; }
	static const unsigned char signature[8] = { 0x89,'P','N','G','\r','\n',0x1a,'\n' };
	fwrite(signature, 1, 8, file);
	unsigned char ihdr[13];
	putBE32(ihdr, w);
	putBE32(ihdr+4, h);
	ihdr[8] = 8; // bits per channel
	ihdr[9] = 6; // RGBA
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	writeChunk(file, "IHDR", ihdr, 13);
	writeChunk(file, "IDAT", packed, packed_size);
	writeChunk(file, "IEND", NULL, 0);
	ok = !ferror(file);
	fclose(file);
	free(packed);
	if (ok) rename(tmp_path, path); // never leaves a half written file in place of good art
	else unlink(tmp_path);
	return ok;
}

// the width of a PNG from its header, 0 if it isn't one
static int pngWidth(const char* path) {
	unsigned char head[24];
	FILE* file = fopen(path, "rb");
	if (!file) return 0;
	size_t got = fread(head, 1, sizeof(head), file);
	fclose(file);
	if (got<24 || memcmp(head+1, "PNG", 3)) return 0;
	return (head[16]<<24) | (head[17]<<16) | (head[18]<<8) | head[19];
}

// resizes to ART_WIDTH wide (keeping its shape) and saves as PNG: averages when shrinking, blends when growing
static int saveResized(SDL_Surface* image, const char* path) {
	SDL_Surface* src = SDL_ConvertSurfaceFormat(image, SDL_PIXELFORMAT_RGBA32, 0);
	if (!src) return 0;
	int sw = src->w, sh = src->h;
	int dw = ART_WIDTH;
	int dh = (int)((long)sh * dw / sw);
	if (dh<1) dh = 1;
	uint8_t* out = malloc((size_t)dw * dh * 4);
	SDL_LockSurface(src);
	for (int y=0; y<dh; y++) {
		for (int x=0; x<dw; x++) {
			uint8_t* o = out + ((size_t)y*dw + x)*4;
			if (sw>dw) {
				// box average of every source pixel this one covers
				int x0 = x * sw / dw, x1 = MAX(x0+1, (x+1) * sw / dw);
				int y0 = y * sh / dh, y1 = MAX(y0+1, (y+1) * sh / dh);
				uint32_t sum[4] = {0,0,0,0};
				int n = 0;
				for (int sy=y0; sy<y1 && sy<sh; sy++) {
					uint8_t* row = (uint8_t*)src->pixels + sy*src->pitch;
					for (int sx=x0; sx<x1 && sx<sw; sx++) {
						for (int k=0; k<4; k++) sum[k] += row[sx*4+k];
						n += 1;
					}
				}
				for (int k=0; k<4; k++) o[k] = n ? sum[k] / n : 0;
			}
			else {
				// bilinear between the four nearest
				float fx = (x + 0.5f) * sw / dw - 0.5f;
				float fy = (y + 0.5f) * sh / dh - 0.5f;
				if (fx<0) fx = 0;
				if (fy<0) fy = 0;
				int x0 = (int)fx, y0 = (int)fy;
				int x1 = MIN(x0+1, sw-1), y1 = MIN(y0+1, sh-1);
				float tx = fx - x0, ty = fy - y0;
				uint8_t* r0 = (uint8_t*)src->pixels + y0*src->pitch;
				uint8_t* r1 = (uint8_t*)src->pixels + y1*src->pitch;
				for (int k=0; k<4; k++) {
					float top = r0[x0*4+k] * (1-tx) + r0[x1*4+k] * tx;
					float bottom = r1[x0*4+k] * (1-tx) + r1[x1*4+k] * tx;
					o[k] = (uint8_t)(top * (1-ty) + bottom * ty + 0.5f);
				}
			}
		}
	}
	SDL_UnlockSurface(src);
	SDL_FreeSurface(src);
	int ok = savePNG(path, out, dw, dh);
	free(out);
	return ok;
}

///////////////////////////////

// names compared without case, punctuation, "the", or anything in brackets, so "Legend of Zelda, The (USA)" matches "The Legend of Zelda"
// (the library writes & as _ in names, so it's dropped like any other punctuation)
static void normalize(const char* in, char* out) {
	char words[512] = "";
	int depth = 0;
	int n = 0;
	for (const unsigned char* c=(const unsigned char*)in; *c && n<(int)sizeof(words)-2; c++) {
		if (*c=='(' || *c=='[') { depth += 1; continue; }
		if (*c==')' || *c==']') { if (depth) depth -= 1; continue; }
		if (depth) continue;
		if (*c==0xC3 && (c[1]==0xA9 || c[1]==0xA8 || c[1]==0x89)) { words[n++] = 'e'; c++; continue; } // é è É
		words[n++] = isalnum(*c) ? tolower(*c) : ' ';
	}
	words[n] = '\0';
	int o = 0;
	for (char* word=strtok(words, " "); word; word=strtok(NULL, " ")) {
		if (!strcmp(word, "the")) continue;
		int len = strlen(word);
		if (o+len>=MAX_PATH-1) break;
		memcpy(out+o, word, len);
		o += len;
	}
	out[o] = '\0';
}
// higher is better: USA and World first, never betas, demos or hacks
static int regionScore(const char* name) {
	int score = 0;
	if (strstr(name, "(USA")) score += 50;
	else if (strstr(name, "(World")) score += 45;
	else if (strstr(name, "Europe")) score += 20;
	else if (strstr(name, "Japan")) score += 5;
	char* bad[] = { "Beta", "Proto", "Demo", "Sample", "Kiosk", "Pirate", "(Unl", "Hack", "Aftermarket", NULL };
	for (int i=0; bad[i]; i++) if (strstr(name, bad[i])) score -= 100;
	if (strstr(name, "(Rev")) score += 1;
	return score;
}

static void urlDecode(const char* in, char* out) {
	int o = 0;
	for (int i=0; in[i] && o<MAX_PATH-1; i++) {
		if (in[i]=='%' && isxdigit((unsigned char)in[i+1]) && isxdigit((unsigned char)in[i+2])) {
			char hex[3] = { in[i+1], in[i+2], 0 };
			out[o++] = (char)strtol(hex, NULL, 16);
			i += 2;
		}
		else out[o++] = in[i];
	}
	out[o] = '\0';
}
static void urlEncode(const char* in, char* out) {
	int o = 0;
	for (const unsigned char* c=(const unsigned char*)in; *c && o<MAX_PATH-4; c++) {
		if (isalnum(*c) || strchr("-_.~()", *c)) out[o++] = *c;
		else o += sprintf(out+o, "%%%02X", *c);
	}
	out[o] = '\0';
}

typedef struct Art {
	char* href; // as listed, already url encoded
	char key[MAX_PATH]; // normalized name
	int score;
} Art;
typedef struct ArtList {
	Art* items;
	int count;
} ArtList;

// the library's list of art for a system: from the card when it's recent, otherwise fetched (and kept)
static int loadArtList(const char* system, ArtList* list) {
	list->items = NULL;
	list->count = 0;
	mkdir(LIST_DIR, 0755);
	char cache_path[MAX_PATH];
	sprintf(cache_path, "%s/%s.txt", LIST_DIR, system);

	struct stat st;
	int fresh = stat(cache_path, &st)==0 && time(NULL)-st.st_mtime<LIST_MAX_AGE;
	if (!fresh) {
		char encoded[MAX_PATH], url[MAX_PATH*2];
		urlEncode(system, encoded);
		sprintf(url, THUMBS_URL "%s/Named_Boxarts/", encoded);
		HTTP_Response response = {0};
		if (HTTP_request(url, NULL, NULL, USER_AGENT, RA_CA_PATH, &response) && response.status==200) {
			FILE* file = fopen(cache_path, "w");
			if (file) {
				char* p = response.body;
				while ((p = strstr(p, "href=\""))) {
					p += 6;
					char* end = strchr(p, '"');
					if (!end) break;
					if (end-p>4 && !strncmp(end-4, ".png", 4)) fprintf(file, "%.*s\n", (int)(end-p), p);
					p = end;
				}
				fclose(file);
			}
			HTTP_freeResponse(&response);
		}
		else {
			if (response.body) HTTP_freeResponse(&response);
			if (!exists(cache_path)) return 0; // an old list beats none
		}
	}

	FILE* file = fopen(cache_path, "r");
	if (!file) return 0;
	int capacity = 1024;
	list->items = malloc(capacity * sizeof(Art));
	char line[MAX_PATH];
	while (fgets(line, sizeof(line), file)) {
		trimTrailingNewlines(line);
		if (!line[0]) continue;
		if (list->count==capacity) {
			capacity *= 2;
			list->items = realloc(list->items, capacity * sizeof(Art));
		}
		Art* art = &list->items[list->count++];
		art->href = strdup(line);
		char decoded[MAX_PATH];
		urlDecode(line, decoded);
		decoded[strlen(decoded)-4] = '\0'; // .png
		normalize(decoded, art->key);
		art->score = regionScore(decoded);
	}
	fclose(file);
	return list->count;
}
static void freeArtList(ArtList* list) {
	for (int i=0; i<list->count; i++) free(list->items[i].href);
	free(list->items);
	list->items = NULL;
	list->count = 0;
}
// the best art for a game name: an exact (normalized) match, or failing that the only title that starts with it
static Art* findArt(ArtList* list, const char* game) {
	char key[MAX_PATH];
	normalize(game, key);
	if (!key[0]) return NULL;
	Art* best = NULL;
	for (int i=0; i<list->count; i++) {
		Art* art = &list->items[i];
		if (!strcmp(art->key, key) && (!best || art->score>best->score)) best = art;
	}
	if (best) return best;

	// eg. "Pokemon Emerald" for "Pokemon - Emerald Version": accept only when one title fits
	char* only = NULL;
	int len = strlen(key);
	if (len<6) return NULL; // too short to be sure
	for (int i=0; i<list->count; i++) {
		Art* art = &list->items[i];
		if (strncmp(art->key, key, len)) continue;
		if (only && strcmp(only, art->key)) return NULL; // more than one title, don't guess
		only = art->key;
		if (!best || art->score>best->score) best = art;
	}
	return best;
}

///////////////////////////////

typedef struct Game {
	char folder[MAX_PATH]; // the system's Roms folder
	char file[MAX_PATH]; // the game's file (or folder) name
	char name[MAX_PATH]; // without the extension
	const char* system;
} Game;

// every game in the Roms folders, each system's games together; only ones without art when missing_only
static int collectGames(Game* games, int missing_only) {
	int count = 0;
	DIR* roms = opendir(ROMS_PATH);
	if (!roms) return 0;
	struct dirent* sys;
	while ((sys = readdir(roms)) && count<MAX_GAMES) {
		if (sys->d_name[0]=='.' || sys->d_type!=DT_DIR) continue;
		char folder[MAX_PATH];
		sprintf(folder, "%s/%s", ROMS_PATH, sys->d_name);
		const char* system = librarySystem(folder);
		DIR* dh = opendir(folder);
		if (!dh) continue;
		struct dirent* dp;
		while ((dp = readdir(dh)) && count<MAX_GAMES) {
			int is_dir = dp->d_type==DT_DIR;
			if (dp->d_name[0]=='.' || (!is_dir && !isGameFile(dp->d_name))) continue;
			if (is_dir && suffixMatch(".pak", dp->d_name)) continue;
			char art_path[MAX_PATH];
			sprintf(art_path, "%s/.res/%s.png", folder, dp->d_name);
			if (missing_only && exists(art_path)) continue;
			Game* game = &games[count++];
			strcpy(game->folder, folder);
			strcpy(game->file, dp->d_name);
			gameName(dp->d_name, is_dir, game->name);
			game->system = system;
		}
		closedir(dh);
	}
	closedir(roms);
	return count;
}

// downloads and sizes one game's art from the system's list: ART_FOUND (href is the art's name),
// ART_NO_MATCH, or ART_FAILED (href is the address that failed)
enum { ART_FOUND, ART_NO_MATCH, ART_FAILED };
static int fetchArt(Game* game, ArtList* list, char* href) {
	Art* art = findArt(list, game->name);
	if (!art) return ART_NO_MATCH;

	char system_encoded[MAX_PATH], url[MAX_PATH*3];
	urlEncode(game->system, system_encoded);
	sprintf(url, THUMBS_URL "%s/Named_Boxarts/%s", system_encoded, art->href);
	HTTP_Response response = {0};
	int ok = 0;
	if (HTTP_request(url, NULL, NULL, USER_AGENT, RA_CA_PATH, &response)) {
		if (response.status==200) {
			SDL_Surface* image = IMG_Load_RW(SDL_RWFromConstMem(response.body, response.body_length), 1);
			if (image) {
				char res_dir[MAX_PATH], art_path[MAX_PATH];
				sprintf(res_dir, "%s/.res", game->folder);
				mkdir(res_dir, 0755);
				sprintf(art_path, "%s/%s.png", res_dir, game->file);
				ok = saveResized(image, art_path); // replaces any art it had, only once the new art is in hand
				SDL_FreeSurface(image);
			}
		}
		HTTP_freeResponse(&response);
	}
	snprintf(href, MAX_PATH, "%s", ok ? art->href : url);
	return ok ? ART_FOUND : ART_FAILED;
}

// just one game's art, for the launcher's game menu (boxart.elf <path to the game>)
static void findOne(char* path) {
	if (!hasWifi() && !WIFI_offer(screen, "Box art needs Wi-Fi")) return;
	Game game = {0};
	snprintf(game.folder, sizeof(game.folder), "%s", path);
	char* slash = strrchr(game.folder, '/');
	if (!slash) return;
	*slash = '\0';
	snprintf(game.file, sizeof(game.file), "%s", slash+1);
	struct stat st;
	gameName(game.file, stat(path, &st)==0 && S_ISDIR(st.st_mode), game.name);
	game.system = librarySystem(game.folder);
	if (!game.system) {
		showMessage("The box art library doesn't cover this system", 1);
		return;
	}
	PWR_setCPUSpeed(CPU_SPEED_PERFORMANCE);
	showMessage("Finding box art...", 0);
	ArtList list = {0};
	int result = ART_FAILED;
	char href[MAX_PATH] = "";
	if (loadArtList(game.system, &list)) result = fetchArt(&game, &list, href);
	freeArtList(&list);
	sync();
	PWR_setCPUSpeed(CPU_SPEED_MENU);
	char message[MAX_PATH*2];
	if (result==ART_FOUND) snprintf(message, sizeof(message), "Box art found");
	else if (result==ART_NO_MATCH) snprintf(message, sizeof(message), "No box art matches\n%s", game.name);
	else snprintf(message, sizeof(message), "Couldn't download the box art\nCheck the Wi-Fi connection");
	showMessage(message, 1);
}

static void findMissing(void) {
	if (!hasWifi() && !WIFI_offer(screen, "Box art needs Wi-Fi")) return;
	PWR_setCPUSpeed(CPU_SPEED_PERFORMANCE); // decoding and resizing is the slow part
	PWR_disableAutosleep(); // a long run with no buttons pressed
	log_file = fopen(LOG_PATH, "w");

	Game* games = calloc(MAX_GAMES, sizeof(Game));
	int count = collectGames(games, 1);
	int found = 0, unmatched = 0, failed = 0, unsupported = 0;
	int stopped = 0;
	ArtList list = {0};
	const char* list_system = NULL;
	for (int i=0; i<count; i++) {
		Game* game = &games[i];
		showProgress("Finding box art", i, count, game->name);
		if (cancelled()) { stopped = 1; break; }
		if (!game->system) { unsupported += 1; continue; }

		if (game->system!=list_system) {
			freeArtList(&list);
			list_system = game->system;
			showProgress("Finding box art", i, count, "Getting the list of art for this system...");
			if (!loadArtList(game->system, &list)) ART_LOG("couldn't get the art list for %s\n", game->system);
		}
		char href[MAX_PATH];
		int result = fetchArt(game, &list, href);
		if (result==ART_FOUND) {
			found += 1;
			ART_LOG("found: %s -> %s\n", game->file, href);
		}
		else if (result==ART_NO_MATCH) {
			unmatched += 1;
			ART_LOG("no match: %s/%s\n", strrchr(game->folder, '/')+1, game->file);
		}
		else {
			failed += 1;
			ART_LOG("download failed: %s (%s)\n", game->file, href);
		}
	}
	freeArtList(&list);
	free(games);
	if (log_file) fclose(log_file);
	log_file = NULL;
	sync();
	PWR_enableAutosleep();
	PWR_setCPUSpeed(CPU_SPEED_MENU);

	char summary[512];
	int n = snprintf(summary, sizeof(summary), "%s\n%i found", stopped ? "Stopped" : count ? "Box art done" : "Every game has box art", found);
	if (unmatched) n += snprintf(summary+n, sizeof(summary)-n, "\n%i with no match (listed in logs/box-art.txt)", unmatched);
	if (failed) n += snprintf(summary+n, sizeof(summary)-n, "\n%i couldn't be downloaded", failed);
	if (unsupported) n += snprintf(summary+n, sizeof(summary)-n, "\n%i on systems the library doesn't cover", unsupported);
	showMessage(summary, 1);
}

// existing art that isn't ART_WIDTH wide
static int collectResizable(Game* games) {
	Game* all = calloc(MAX_GAMES, sizeof(Game));
	int total = collectGames(all, 0);
	int count = 0;
	for (int i=0; i<total; i++) {
		char art_path[MAX_PATH];
		sprintf(art_path, "%s/.res/%s.png", all[i].folder, all[i].file);
		int width = pngWidth(art_path);
		if (width && width!=ART_WIDTH) games[count++] = all[i];
	}
	free(all);
	return count;
}
static void resizeAll(void) {
	PWR_setCPUSpeed(CPU_SPEED_PERFORMANCE);
	PWR_disableAutosleep();
	Game* games = calloc(MAX_GAMES, sizeof(Game));
	int count = collectResizable(games);
	int resized = 0, failed = 0, stopped = 0;
	for (int i=0; i<count; i++) {
		Game* game = &games[i];
		showProgress("Resizing box art", i, count, game->name);
		if (cancelled()) { stopped = 1; break; }
		char art_path[MAX_PATH];
		sprintf(art_path, "%s/.res/%s.png", game->folder, game->file);
		SDL_Surface* image = IMG_Load(art_path);
		if (image && saveResized(image, art_path)) resized += 1;
		else failed += 1;
		if (image) SDL_FreeSurface(image);
	}
	free(games);
	sync();
	PWR_enableAutosleep();
	PWR_setCPUSpeed(CPU_SPEED_MENU);

	char summary[256];
	int n = snprintf(summary, sizeof(summary), "%s\n%i resized to %i pixels wide", stopped ? "Stopped" : count ? "Resizing done" : "All box art is already the right size", resized, ART_WIDTH);
	if (failed) snprintf(summary+n, sizeof(summary)-n, "\n%i couldn't be read", failed);
	showMessage(summary, 1);
}

///////////////////////////////

int main(int argc , char* argv[]) {
	PWR_setCPUSpeed(CPU_SPEED_MENU);
	screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	InitSettings();

	if (argc>1) { // one game, from the launcher's game menu
		findOne(argv[1]);
		QuitSettings();
		PWR_quit();
		PAD_quit();
		GFX_quit();
		return EXIT_SUCCESS;
	}

	enum { ROW_MISSING, ROW_RESIZE, ROW_COUNT };
	int selected = 0;
	int dirty = 1;
	int show_setting = 0;
	Game* scratch = calloc(MAX_GAMES, sizeof(Game));
	showMessage("Checking box art...", 0); // reading every game's art takes a moment on a big card
	int missing = collectGames(scratch, 1);
	int resizable = collectResizable(scratch);

	while (1) {
		GFX_startFrame();
		PAD_poll();

		if (PAD_justPressed(BTN_B)) break;
		else if (PAD_justRepeated(BTN_UP) || PAD_justRepeated(BTN_DOWN)) {
			selected = (selected + 1) % ROW_COUNT;
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_A)) {
			if (selected==ROW_MISSING) findMissing();
			else resizeAll();
			showMessage("Checking box art...", 0);
			missing = collectGames(scratch, 1);
			resizable = collectResizable(scratch);
			PAD_reset();
			dirty = 1;
		}

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (dirty) {
			GFX_clear(screen);
			int ow = GFX_blitHardwareGroup(screen, show_setting);

			for (int i=0; i<ROW_COUNT; i++) {
				char name[64], value[64];
				if (i==ROW_MISSING) {
					strcpy(name, "Find missing box art");
					if (missing) sprintf(value, "%i missing", missing);
					else strcpy(value, "None missing");
				}
				else {
					sprintf(name, "Resize to %ipx wide", ART_WIDTH);
					if (resizable) sprintf(value, "%i to resize", resizable);
					else strcpy(value, "All sized");
				}
				int y = SCALE1(PADDING + i*PILL_SIZE);
				int width = screen->w - SCALE1(PADDING*2);
				if (i==0) width -= ow;
				int is_selected = i==selected;
				if (is_selected) GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){ SCALE1(PADDING), y, width, SCALE1(PILL_SIZE) });

				int value_w;
				TTF_SizeUTF8(font.small, value, &value_w, NULL);
				char shown[64];
				GFX_truncateText(font.large, name, shown, width - value_w - SCALE1(BUTTON_PADDING*3), 0);
				SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, shown, is_selected ? COLOR_BLACK : COLOR_WHITE);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ SCALE1(PADDING+BUTTON_PADDING), y + SCALE1(4) });
				SDL_FreeSurface(text);
				text = TTF_RenderUTF8_Blended(font.small, value, is_selected ? COLOR_BLACK : COLOR_DARK_TEXT);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ SCALE1(PADDING) + width - SCALE1(BUTTON_PADDING) - text->w, y + (SCALE1(PILL_SIZE) - text->h)/2 });
				SDL_FreeSurface(text);
			}

			char* hint = selected==ROW_MISSING
				? "Downloads art from the libretro thumbnail library\nand sizes it to fit (Wi-Fi needs to be on)"
				: "Shrinks or grows existing art to the same width\nso lists and the carousel load quickly";
			int y = SCALE1(PADDING + ROW_COUNT*PILL_SIZE + PADDING);
			GFX_blitMessage(font.small, hint, screen, &(SDL_Rect){ 0, y, screen->w, screen->h - y - SCALE1(PADDING+PILL_SIZE) });

			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			GFX_blitButtonGroup((char*[]){ "B","BACK", "A",selected==ROW_MISSING ? "FIND" : "RESIZE", NULL }, 1, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}

	free(scratch);
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return EXIT_SUCCESS;
}
