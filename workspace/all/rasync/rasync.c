// Sync Achievements: caches the RetroAchievements data for every game on the card so they work offline,
// and sends unlocks that were earned offline. Uses the same cache as minarch (see minarch/ra_cache.c).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <msettings.h>

#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "rc_hash.h"

#include "defines.h"
#include "api.h"
#include "tools.h"
#include "bgm.h"
#include "utils.h"
#include "ra_cache.h"
#include "ra_http.h"
#include "keyboard.h"
#include "ra_chd.h"
#include "wifi_ask.h"

#define UNZIP_PATH SDCARD_PATH "/.tmp_update/my282/unzip"
#define USER_AGENT "MinUI/" PLATFORM " rcheevos" // same client name minarch uses
#define LOG_PATH USERDATA_PATH "/logs/sync-achievements.txt"
#define MAX_GAMES 2048

static SDL_Surface* screen;
static FILE* log_file = NULL; // what happened to each game, for troubleshooting
// each line is on the card before going on, so after a freeze the last one says where it was
#define SYNC_LOG(...) do { if (log_file) { fprintf(log_file, __VA_ARGS__); fflush(log_file); fsync(fileno(log_file)); } } while (0)

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
			else GFX_blitButtonGroup((char*[]){ "B","CANCEL", NULL }, 0, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
		if (!wait) break;
	}
}

static int last_connected = 1; // whether the last request reached the server at all
static int last_status = 0; // HTTP status of the last request

// title, game, a bar that fills as games are done, and a count
#define PROGRESS_MS 100
static void showProgress(int done, int count, char* name, char* stage, int force) {
	// drawn at most every PROGRESS_MS: a frame per game (waiting on vsync) was most of the time for games it skips
	static uint32_t drawn_at = 0;
	uint32_t now = SDL_GetTicks();
	if (!force && drawn_at && now-drawn_at<PROGRESS_MS) return;
	drawn_at = now;
	int show_setting = 0;
	int dirty = 1;
	GFX_startFrame();
	PWR_update(&dirty, &show_setting, NULL, NULL);
	GFX_clear(screen);
	GFX_blitHardwareGroup(screen, show_setting);
	
	int width = screen->w - SCALE1(PADDING*4);
	int x = SCALE1(PADDING*2);
	int y = screen->h/2 - SCALE1(PILL_SIZE*2);
	
	SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, "Syncing achievements", COLOR_WHITE);
	SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ (screen->w-text->w)/2, y });
	y += text->h + SCALE1(4);
	SDL_FreeSurface(text);
	
	char shown[256];
	GFX_truncateText(font.small, name, shown, width, 0);
	text = TTF_RenderUTF8_Blended(font.small, shown, COLOR_DARK_TEXT);
	SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ (screen->w-text->w)/2, y });
	y += text->h + SCALE1(PADDING);
	SDL_FreeSurface(text);
	
	// the bar: a dark track with a white fill, both rounded
	int bar_h = SCALE1(20);
	GFX_blitRect(ASSET_OPTION, screen, &(SDL_Rect){ x, y, width, bar_h });
	int fill = count ? (int)((long)width * done / count) : 0;
	if (fill>0) GFX_blitRect(ASSET_BUTTON, screen, &(SDL_Rect){ x, y, MAX(fill, bar_h), bar_h });
	y += bar_h + SCALE1(6);
	
	char status[128];
	snprintf(status, sizeof(status), "Game %i of %i (%i%% done)%s%s", MIN(done+1, count), count, count ? done*100/count : 0, stage ? "  \xC2\xB7  " : "", stage ? stage : "");
	text = TTF_RenderUTF8_Blended(font.small, status, COLOR_WHITE);
	SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ (screen->w-text->w)/2, y });
	SDL_FreeSurface(text);
	
	GFX_blitButtonGroup((char*[]){ "B","CANCEL", NULL }, 0, screen, 1);
	GFX_flip(screen);
}

static int request(rc_api_request_t* req, HTTP_Response* response, int cache) { // returns 1 on a 200
	int connected = HTTP_request(req->url, req->post_data, req->content_type, USER_AGENT, RA_CA_PATH, response);
	last_connected = connected;
	last_status = connected ? response->status : 0;
	int ok = connected && response->status==200;
	if (!ok) SYNC_LOG("  request failed: %s (status %i) %.120s\n", connected ? "server error" : "no connection", response->status, response->body ? response->body : "");
	if (connected && cache) RACache_store(req->post_data, response->status, response->body, response->body_length);
	return ok;
}

// signs in if there's only a password, and makes sure the sign in itself is cached for offline launches
static int signIn(RA_Creds* creds) {
	if (creds->token[0] && RACache_has("login")) return 1;
	rc_api_login_request_t params = {0};
	params.username = creds->username;
	params.api_token = creds->token[0] ? creds->token : NULL;
	params.password = creds->token[0] ? NULL : creds->password;
	rc_api_request_t req;
	if (rc_api_init_login_request(&req, &params)!=RC_OK) return 0;
	HTTP_Response response;
	int ok = request(&req, &response, 1);
	if (ok) {
		rc_api_login_response_t login;
		rc_api_server_response_t server = { response.body, response.body_length, response.status };
		if (rc_api_process_login_server_response(&login, &server)==RC_OK && login.api_token) {
			if (!exactMatch(creds->token, (char*)login.api_token)) RACache_saveToken(login.username, login.api_token);
			snprintf(creds->token, sizeof(creds->token), "%s", login.api_token);
		}
		else ok = 0;
		rc_api_destroy_login_response(&login);
	}
	HTTP_freeResponse(&response);
	rc_api_destroy_request(&req);
	return ok && creds->token[0];
}

// each game's hash, by its path, size and date: a rescan only hashes (and unzips) what's new or changed
#define HASHES_PATH RA_CACHE_DIR "/hashes.txt"
typedef struct KnownHash {
	char hash[33];
	long long size;
	long long mtime;
	char* path;
	int line; // later lines (rehashed after a change) win
} KnownHash;
static KnownHash* known_hashes = NULL;
static int known_count = 0;
static int byPathThenLine(const void* a, const void* b) {
	const KnownHash* x = a;
	const KnownHash* y = b;
	int order = strcmp(x->path, y->path);
	return order ? order : x->line - y->line;
}
// read whole (it's appended to, so it can outgrow the game count), sorted by path keeping only each path's
// newest line, and written back compacted when that dropped any
static void loadHashes(void) {
	int capacity = 1024;
	known_hashes = calloc(capacity, sizeof(KnownHash));
	FILE* file = fopen(HASHES_PATH, "r");
	if (!file) return;
	char line[MAX_PATH+128];
	int lines = 0;
	while (fgets(line, sizeof(line), file)) { // <hash> <size> <mtime> <path>
		trimTrailingNewlines(line);
		lines += 1;
		if (known_count==capacity) known_hashes = realloc(known_hashes, (capacity *= 2) * sizeof(KnownHash));
		KnownHash* known = &known_hashes[known_count];
		int at = 0;
		if (sscanf(line, "%32s %lld %lld %n", known->hash, &known->size, &known->mtime, &at)!=3 || !at || !line[at]) continue;
		known->path = strdup(line + at);
		known->line = lines;
		known_count += 1;
	}
	fclose(file);
	qsort(known_hashes, known_count, sizeof(KnownHash), byPathThenLine);
	int kept = 0;
	for (int i=0; i<known_count; i++) {
		if (i+1<known_count && !strcmp(known_hashes[i].path, known_hashes[i+1].path)) { free(known_hashes[i].path); continue; }
		known_hashes[kept++] = known_hashes[i];
	}
	known_count = kept;
	if (lines==kept) return;
	char tmp_path[MAX_PATH];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", HASHES_PATH);
	file = fopen(tmp_path, "w");
	if (!file) return;
	for (int i=0; i<known_count; i++) fprintf(file, "%s %lld %lld %s\n", known_hashes[i].hash, known_hashes[i].size, known_hashes[i].mtime, known_hashes[i].path);
	int ok = !ferror(file);
	fclose(file);
	if (ok) rename(tmp_path, HASHES_PATH);
	else unlink(tmp_path);
}
static void freeHashes(void) {
	for (int i=0; i<known_count; i++) free(known_hashes[i].path);
	free(known_hashes);
	known_hashes = NULL;
	known_count = 0;
}
static int knownHash(const char* path, struct stat* st, char hash[33]) {
	int lo = 0, hi = known_count; // sorted by path, one line each
	while (lo<hi) {
		int mid = (lo+hi)/2;
		if (strcmp(known_hashes[mid].path, path)<0) lo = mid+1;
		else hi = mid;
	}
	if (lo>=known_count || strcmp(known_hashes[lo].path, path)) return 0;
	KnownHash* known = &known_hashes[lo];
	if (known->size!=(long long)st->st_size || known->mtime!=(long long)st->st_mtime) return 0;
	strcpy(hash, known->hash);
	return 1;
}
static void rememberHash(const char* path, struct stat* st, const char* hash) {
	FILE* file = fopen(HASHES_PATH, "a");
	if (!file) return;
	fprintf(file, "%s %lld %lld %s\n", hash, (long long)st->st_size, (long long)st->st_mtime, path);
	fclose(file);
}

static int hashFile(const char* path, uint32_t console, char hash[33]) {
	if (!suffixMatch(".zip", (char*)path)) return rc_hash_generate_from_file(hash, console, path);

	// zips hash by the rom inside, the same file minarch extracts before loading
	char escaped[MAX_PATH*2], cmd[MAX_PATH*2+64];
	char* out = escaped;
	for (const char* in=path; *in; in++) {
		if (*in=='\'') { strcpy(out, "'\\''"); out += 4; }
		else *out++ = *in;
	}
	*out = '\0';
	snprintf(cmd, sizeof(cmd), "'%s' -p '%s' 2>/dev/null", UNZIP_PATH, escaped);
	FILE* pipe = popen(cmd, "r");
	if (!pipe) return 0;
	size_t capacity = 1<<20, size = 0;
	uint8_t* data = malloc(capacity);
	size_t n;
	while ((n = fread(data+size, 1, capacity-size, pipe))>0) {
		size += n;
		if (size==capacity) {
			capacity *= 2;
			uint8_t* bigger = realloc(data, capacity);
			if (!bigger) break;
			data = bigger;
		}
	}
	pclose(pipe);
	int ok = size>0 && rc_hash_generate_from_buffer(hash, console, data, size);
	free(data);
	return ok;
}
static int hashGame(const char* path, uint32_t console, char hash[33]) {
	struct stat st;
	if (stat(path, &st)!=0) return 0;
	if (knownHash(path, &st, hash)) return 1;
	SYNC_LOG("%s: hashing\n", path);
	if (!hashFile(path, console, hash)) return 0;
	rememberHash(path, &st, hash);
	return 1;
}

// the CPU's clock and the voltages, as the kernel sees them: a lead if the device ever freezes during a sync
static void logPower(void) {
	char value[64];
	char* cpufreq[] = { "scaling_governor", "scaling_cur_freq", "cpuinfo_cur_freq", "scaling_setspeed", NULL };
	for (int i=0; cpufreq[i]; i++) {
		char path[128];
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cpufreq/%s", cpufreq[i]);
		memset(value, 0, sizeof(value)); // sysfs files say they're 4096 bytes, so getFile keeps whatever was here before
		getFile(path, value, sizeof(value));
		trimTrailingNewlines(value);
		if (value[0]) SYNC_LOG("cpu0 %s: %s\n", cpufreq[i], value);
	}
	DIR* regulators = opendir("/sys/class/regulator");
	if (!regulators) return;
	struct dirent* entry;
	while ((entry = readdir(regulators))) {
		if (entry->d_name[0]=='.') continue;
		char path[MAX_PATH], name[64] = "", microvolts[32] = "";
		snprintf(path, sizeof(path), "/sys/class/regulator/%s/name", entry->d_name);
		getFile(path, name, sizeof(name));
		snprintf(path, sizeof(path), "/sys/class/regulator/%s/microvolts", entry->d_name);
		getFile(path, microvolts, sizeof(microvolts));
		trimTrailingNewlines(name);
		trimTrailingNewlines(microvolts);
		if (microvolts[0]) SYNC_LOG("%s %s: %s uV\n", entry->d_name, name, microvolts);
	}
	closedir(regulators);
}

typedef struct Game {
	char path[MAX_PATH];
	uint32_t console;
} Game;

static int collectGames(Game* games) {
	int count = 0;
	DIR* roms = opendir(ROMS_PATH);
	if (!roms) return 0;
	struct dirent* system;
	while ((system = readdir(roms)) && count<MAX_GAMES) {
		if (hide(system->d_name) || system->d_type!=DT_DIR) continue;
		char tag[64];
		getEmuName(system->d_name, tag);
		uint32_t console = RACache_consoleForTag(tag);
		if (!console) continue;

		char dir_path[MAX_PATH];
		snprintf(dir_path, sizeof(dir_path), "%s/%s", ROMS_PATH, system->d_name);
		DIR* dir = opendir(dir_path);
		if (!dir) continue;
		struct dirent* file;
		while ((file = readdir(dir)) && count<MAX_GAMES) {
			if (hide(file->d_name)) continue;
			char* path = games[count].path;
			snprintf(path, MAX_PATH, "%s/%s", dir_path, file->d_name);
			if (file->d_type==DT_DIR) {
				// a multi-disc game: its folder holds <folder>.m3u, which rcheevos hashes by the first disc
				snprintf(path, MAX_PATH, "%s/%s/%s.m3u", dir_path, file->d_name, file->d_name);
				if (!exists(path)) continue;
			}
			else if (suffixMatch(".txt", file->d_name) || suffixMatch(".png", file->d_name)) continue; // map.txt and the like
			else if (suffixMatch(".bin", file->d_name)) { // a disc's data, hashed through its .cue when there is one
				char cue[MAX_PATH];
				snprintf(cue, sizeof(cue), "%s", path);
				strcpy(cue + strlen(cue) - 4, ".cue");
				if (exists(cue) || strstr(file->d_name, "(Track ")) continue; // or one track of several in a .cue
			}
			games[count].console = console;
			count += 1;
		}
		closedir(dir);
	}
	closedir(roms);
	return count;
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

// the games a sync found no achievements for, shown after it: "Name" and "TAG, why"
typedef struct Missed { char** names; char** notes; int count; } Missed;
static void addMissed(Missed* missed, const char* path, const char* why) {
	char name[256];
	getDisplayName(path, name);
	char folder[MAX_PATH];
	snprintf(folder, sizeof(folder), "%s", path);
	char* slash = strrchr(folder, '/');
	if (slash) *slash = '\0';
	char tag[64] = "";
	char* open = strrchr(folder, '(');
	char* close = open ? strchr(open, ')') : NULL;
	if (open && close && close-open-1<(int)sizeof(tag)) snprintf(tag, sizeof(tag), "%.*s, ", (int)(close-open-1), open+1);
	char note[128];
	snprintf(note, sizeof(note), "%s%s", tag, why);
	missed->names[missed->count] = strdup(name);
	missed->notes[missed->count] = strdup(note);
	missed->count += 1;
}
// whether a game's saved reply from an earlier sync has a set (a local read, no network)
static int cachedHasAchievements(const char* key) {
	size_t length = 0;
	char* body = RACache_read(key, &length);
	if (!body) return 1; // can't tell, so don't list it
	int has = 1;
	rc_api_fetch_game_sets_response_t sets;
	rc_api_server_response_t server = { body, length, 200 };
	if (rc_api_process_fetch_game_sets_server_response(&sets, &server)==RC_OK) has = sets.id!=0;
	rc_api_destroy_fetch_game_sets_response(&sets);
	free(body);
	return has;
}
// the summary, with a way to see that list when there is one: 1 for A
static int askSummary(char* msg, int has_list) {
	if (!has_list) {
		showMessage(msg, 1);
		return 0;
	}
	int dirty = 1;
	int show_setting = 0;
	PAD_reset();
	while (1) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_A)) return 1;
		if (PAD_justPressed(BTN_B)) return 0;
		PWR_update(&dirty, &show_setting, NULL, NULL);
		if (dirty) {
			GFX_clear(screen);
			GFX_blitHardwareGroup(screen, show_setting);
			GFX_blitMessage(font.large, msg, screen, &(SDL_Rect){ 0, 0, screen->w, screen->h - SCALE1(PILL_SIZE+PADDING) });
			GFX_blitButtonGroup((char*[]){ "B","DONE", "A","SEE LIST", NULL }, 1, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}
}

static void runSync(void) {
	PWR_setCPUSpeed(CPU_SPEED_NORMAL); // hashing waits on the card more than the CPU, and minutes flat out at the top speed is asking for trouble
	PWR_disableAutosleep(); // a full sync takes a while with no buttons pressed
	log_file = fopen(LOG_PATH, "w");
	RA_Creds creds;

	if (!RACache_readCreds(&creds)) showMessage("Sync failed\nSign in first", 1);
	else if (!hasWifi() && !WIFI_offer(screen, "Syncing needs Wi-Fi")) { // left off: nothing to say
	}
	else {
		showMessage("Signing in...", 0);
		if (!signIn(&creds)) showMessage("Sync failed\nCouldn't sign in to RetroAchievements\nSign in again from this menu", 1);
		else {
			int sent = RACache_sendPending(&creds, USER_AGENT);

			Game* games = calloc(MAX_GAMES, sizeof(Game));
			int count = collectGames(games);
			Missed missed = { calloc(MAX(1, count), sizeof(char*)), calloc(MAX(1, count), sizeof(char*)), 0 };
			int synced = 0, skipped = 0, with_achievements = 0, failed = 0, cancelled = 0;
			int offline_streak = 0, lost = 0, done = 0; // three games in a row without reaching the server means the connection dropped
			int rejected = 0; // the server refused the saved login
			int unknown = 0; // not in RetroAchievements (usually romhacks)

			SYNC_LOG("%i games found, %i offline unlocks sent\n", count, sent);
			logPower();
			loadHashes();
			int badges = 0;
			for (int i=0; i<count; i++) {
				PAD_poll();
				if (PAD_isPressed(BTN_B)) { // held, since the screen only updates between games
					cancelled = 1;
					break;
				}
				char name[256];
				getDisplayName(games[i].path, name);
				done = i;
				showProgress(i, count, name, "Identifying", i==0);

				char hash[33];
				if (!hashGame(games[i].path, games[i].console, hash)) {
					SYNC_LOG("%s: couldn't identify\n", games[i].path);
					failed += 1;
					addMissed(&missed, games[i].path, "couldn't identify");
					continue;
				}

				rc_api_fetch_game_sets_request_t sets_params = {0};
				sets_params.username = creds.username;
				sets_params.api_token = creds.token;
				sets_params.game_hash = hash;
				rc_api_request_t req;
				if (rc_api_init_fetch_game_sets_request(&req, &sets_params)!=RC_OK) continue;
				char key[128];
				if (RACache_key(req.post_data, key, sizeof(key)) && (RACache_has(key) || RACache_isKnownUnknown(key))) { // synced before
					SYNC_LOG("%s: already synced\n", games[i].path);
					skipped += 1;
					if (!RACache_has(key)) addMissed(&missed, games[i].path, "not in RetroAchievements"); // (a 404 kept from before)
					else if (!cachedHasAchievements(key)) addMissed(&missed, games[i].path, "no achievements yet");
					rc_api_destroy_request(&req);
					continue;
				}

				HTTP_Response response;
				uint32_t ids[2] = {0, 0};
				if (request(&req, &response, 1)) {
					synced += 1;
					rc_api_fetch_game_sets_response_t sets;
					rc_api_server_response_t server = { response.body, response.body_length, response.status };
					if (rc_api_process_fetch_game_sets_server_response(&sets, &server)==RC_OK) {
						ids[0] = sets.id;
						ids[1] = sets.session_game_id!=sets.id ? sets.session_game_id : 0;
						int game_badges = 0;
						if (ids[0]) {
							showProgress(i, count, name, "Downloading badges", 1);
							int total = 0;
							for (uint32_t k=0; k<sets.num_sets; k++) total += sets.sets[k].num_achievements;
							RA_BadgeName* badge_names = calloc(total ? total : 1, sizeof(RA_BadgeName));
							int n = 0;
							for (uint32_t k=0; k<sets.num_sets; k++) {
								for (uint32_t j=0; j<sets.sets[k].num_achievements; j++) {
									const char* badge = sets.sets[k].achievements[j].badge_name;
									if (badge) strncpy(badge_names[n++], badge, sizeof(RA_BadgeName)-1);
								}
							}
							game_badges = RACache_fetchBadges(sets.id, badge_names, n, USER_AGENT);
							free(badge_names);
						}
						badges += game_badges;
						SYNC_LOG("%s: game %u, %i badges\n", games[i].path, sets.id, game_badges);
					}
					rc_api_destroy_fetch_game_sets_response(&sets);
				}
				else if (last_status==404) {
					unknown += 1;
					SYNC_LOG("%s: not in RetroAchievements\n", games[i].path);
					addMissed(&missed, games[i].path, "not in RetroAchievements");
				}
				else if (last_status!=401) {
					failed += 1;
					if (last_connected) addMissed(&missed, games[i].path, "couldn't check"); // (offline ones are tried again next time)
				}
				HTTP_freeResponse(&response);
				rc_api_destroy_request(&req);
				
				if (last_status==401) {
					rejected = 1;
					break;
				}
				offline_streak = last_connected ? 0 : offline_streak + 1;
				if (offline_streak>=3) {
					lost = 1;
					break;
				}

				// your unlocks, so offline play doesn't re-award achievements you already have
				if (ids[0]) with_achievements += 1;
				else if (last_status==200) addMissed(&missed, games[i].path, "no achievements yet");
				for (int j=0; j<2; j++) {
					if (!ids[j]) continue;
					rc_api_fetch_user_unlocks_request_t unlock_params = {0};
					unlock_params.username = creds.username;
					unlock_params.api_token = creds.token;
					unlock_params.game_id = ids[j];
					if (rc_api_init_fetch_user_unlocks_request(&req, &unlock_params)!=RC_OK) continue;
					if (request(&req, &response, 1)) HTTP_freeResponse(&response);
					rc_api_destroy_request(&req);
				}
			}
			free(games);
			freeHashes();
			if (!lost && !cancelled && !rejected) done = count;

			char summary[512];
			int n;
			if (rejected) n = snprintf(summary, sizeof(summary), "Sync failed\nRetroAchievements rejected your login\nSign in again from this menu");
			else if (lost) n = snprintf(summary, sizeof(summary), "Sync failed\nLost the connection to RetroAchievements\nStopped at game %i of %i\nRun it again to continue where it left off", done+1, count);
			else {
				n = snprintf(summary, sizeof(summary), "%s\n%i synced, %i already synced", cancelled ? "Sync cancelled" : "Sync complete", synced, skipped);
				if (with_achievements) n += snprintf(summary+n, sizeof(summary)-n, "\n%i new with achievements", with_achievements);
				if (unknown) n += snprintf(summary+n, sizeof(summary)-n, "\n%i not in RetroAchievements", unknown);
				if (failed) n += snprintf(summary+n, sizeof(summary)-n, "\n%i couldn't be checked", failed);
			}
			if (sent) n += snprintf(summary+n, sizeof(summary)-n, "\n%i offline unlock%s sent", sent, sent==1 ? "" : "s");
			SYNC_LOG("unknown to RetroAchievements: %i\n", unknown);
			SYNC_LOG("done: %s, %i synced, %i skipped, %i with achievements, %i failed, %i badges\n", rejected ? "login rejected" : lost ? "lost connection" : cancelled ? "cancelled" : "complete", synced, skipped, with_achievements, failed, badges);
			PWR_enableAutosleep(); // the summary waits for a button, sleeping is fine again
			if (askSummary(summary, missed.count && !rejected)) {
				char title[64];
				snprintf(title, sizeof(title), "No achievements: %i game%s", missed.count, missed.count==1 ? "" : "s");
				GFX_showList(screen, title, missed.names, missed.notes, missed.count);
			}
			for (int i=0; i<missed.count; i++) {
				free(missed.names[i]);
				free(missed.notes[i]);
			}
			free(missed.names);
			free(missed.notes);
		}
	}

	if (log_file) fclose(log_file);
	log_file = NULL;
	PWR_enableAutosleep();
	PWR_setCPUSpeed(CPU_SPEED_MENU);
}

// returns 1 for A, 0 for B
static int confirm(char* msg) {
	int dirty = 1;
	int show_setting = 0;
	PAD_reset();
	while (1) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_A)) return 1;
		if (PAD_justPressed(BTN_B)) return 0;
		PWR_update(&dirty, &show_setting, NULL, NULL);
		if (dirty) {
			GFX_clear(screen);
			GFX_blitHardwareGroup(screen, show_setting);
			GFX_blitMessage(font.large, msg, screen, &(SDL_Rect){0,0,screen->w,screen->h});
			GFX_blitButtonGroup((char*[]){ "B","CANCEL", "A","YES", NULL }, 1, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}
}

static void signInFromConsole(void) {
	if (!hasWifi() && !WIFI_offer(screen, "Signing in needs Wi-Fi")) return;
	RA_Creds creds, saved;
	RACache_readCreds(&saved);
	memset(&creds, 0, sizeof(creds));
	snprintf(creds.username, sizeof(creds.username), "%s", saved.username);
	if (!KB_enterText(screen, "RetroAchievements username", creds.username, sizeof(creds.username), "NEXT", 0) || !creds.username[0]) return;
	if (!KB_enterText(screen, "RetroAchievements password", creds.password, sizeof(creds.password), "SIGN IN", 0) || !creds.password[0]) return;
	
	showMessage("Signing in...", 0);
	// the saved login is only replaced once this one is accepted (signIn saves the new token)
	if (signIn(&creds)) {
		char msg[192];
		snprintf(msg, sizeof(msg), "Signed in as %s", creds.username);
		showMessage(msg, 1);
	}
	else showMessage("Couldn't sign in\nCheck your username and password", 1);
}

static int countSynced(void) { // games with achievement data cached
	int count = 0;
	DIR* dir = opendir(RA_CACHE_DIR);
	if (!dir) return 0;
	struct dirent* entry;
	while ((entry = readdir(dir))) if (prefixMatch("achievementsets_", entry->d_name) && suffixMatch(".json", entry->d_name)) count += 1;
	closedir(dir);
	return count;
}

void RetroAchievements_open(SDL_Surface* main_screen) {
	screen = main_screen;
	struct rc_hash_cdreader disc_reader; // so PlayStation .chd games can be identified too
	RA_getDiscReader(&disc_reader);
	rc_hash_init_custom_cdreader(&disc_reader);
	
	enum { ROW_ACCOUNT, ROW_SYNC, ROW_HARDCORE }; // (hardcore hidden while RA_HARDCORE_AVAILABLE is 0)
	#define ROW_COUNT (RA_HARDCORE_AVAILABLE ? ROW_HARDCORE+1 : ROW_HARDCORE)
	int selected = 0;
	int dirty = 1;
	int show_setting = 0;
	int synced = countSynced();
	RA_Creds creds;
	int signed_in = RACache_readCreds(&creds);
	
	while (1) {
		GFX_startFrame();
		PAD_poll();
		
		if (PAD_justPressed(BTN_B)) break;
		else if (PAD_justRepeated(BTN_UP)) {
			selected = (selected + ROW_COUNT - 1) % ROW_COUNT;
			dirty = 1;
		}
		else if (PAD_justRepeated(BTN_DOWN)) {
			selected = (selected + 1) % ROW_COUNT;
			dirty = 1;
		}
		else if (selected==ROW_HARDCORE && (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_LEFT) || PAD_justPressed(BTN_RIGHT))) {
			if (exists(RA_HARDCORE_PATH)) unlink(RA_HARDCORE_PATH);
			else {
				mkdir(RA_CACHE_DIR, 0755);
				putFile(RA_HARDCORE_PATH, "1");
			}
			sync();
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_A)) {
			if (selected==ROW_ACCOUNT) {
				if (!signed_in) signInFromConsole();
				else {
					char msg[192];
					snprintf(msg, sizeof(msg), "Sign out %s?\nCached achievements stay on the card", creds.username);
					if (confirm(msg)) {
						unlink(RA_CREDS_PATH);
						unlink(RA_CACHE_DIR "/login.json");
						sync();
					}
				}
			}
			else if (!signed_in) {
				if (confirm("Sign in to RetroAchievements first?")) signInFromConsole();
			}
			else {
				runSync();
				synced = countSynced();
			}
			signed_in = RACache_readCreds(&creds);
			PAD_reset();
			dirty = 1;
		}
		
		PWR_update(&dirty, &show_setting, NULL, NULL);
		
		if (dirty) {
			GFX_clear(screen);
			// the header beside the time and battery, the rows below them at full width so a long name has room
			GFX_blitHeader(screen, "RetroAchievements", GFX_blitHardwareGroup(screen, show_setting));
			#define ROWS_TOP (PADDING + PILL_SIZE + 6)
			
			for (int i=0; i<ROW_COUNT; i++) {
				char name[192], value[64] = "";
				if (i==ROW_ACCOUNT) {
					if (signed_in) {
						snprintf(name, sizeof(name), "Signed in as %s", creds.username);
						snprintf(value, sizeof(value), "Sign out");
					}
					else snprintf(name, sizeof(name), "Sign in");
				}
				else if (i==ROW_SYNC) {
					snprintf(name, sizeof(name), "Sync all games");
					if (synced) snprintf(value, sizeof(value), "%i cached", synced);
				}
				else {
					snprintf(name, sizeof(name), "Hardcore mode");
					snprintf(value, sizeof(value), exists(RA_HARDCORE_PATH) ? "On" : "Off");
				}
				int y = SCALE1(ROWS_TOP + i*PILL_SIZE);
				int width = screen->w - SCALE1(PADDING*2);
				int is_selected = i==selected;
				if (is_selected) GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){ SCALE1(PADDING), y, width, SCALE1(PILL_SIZE) });
				
				int value_w = 0;
				if (value[0]) TTF_SizeUTF8(font.small, value, &value_w, NULL);
				char shown[192];
				GFX_truncateText(font.large, name, shown, width - value_w - SCALE1(BUTTON_PADDING*3), 0);
				SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, shown, is_selected ? COLOR_BLACK : COLOR_WHITE);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ SCALE1(PADDING+BUTTON_PADDING), y + SCALE1(4) });
				SDL_FreeSurface(text);
				if (value[0]) {
					text = TTF_RenderUTF8_Blended(font.small, value, is_selected ? COLOR_BLACK : COLOR_DARK_TEXT);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ SCALE1(PADDING) + width - SCALE1(BUTTON_PADDING) - text->w, y + (SCALE1(PILL_SIZE) - text->h)/2 });
					SDL_FreeSurface(text);
				}
			}
			
			// what offline play depends on, so it's clear why syncing matters
			char* hint;
			if (selected==ROW_HARDCORE) hint = "Unlocks count as hardcore: no save states or resume\n(loading one switches that session to softcore)";
			else hint = signed_in ? "Syncing caches every game's achievements and badges\nso they work without Wi-Fi" : "Sign in with the on-screen keyboard\n(Wi-Fi needs to be on)";
			int y = SCALE1(ROWS_TOP + ROW_COUNT*PILL_SIZE + 4);
			GFX_blitMessage(font.small, hint, screen, &(SDL_Rect){ 0, y, screen->w, screen->h - y - SCALE1(PADDING+PILL_SIZE) });
			
			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			char* action = selected==ROW_ACCOUNT ? (signed_in ? "SIGN OUT" : "SIGN IN") : selected==ROW_SYNC ? "SYNC" : "CHANGE";
			GFX_blitButtonGroup((char*[]){ "B","BACK", "A",action, NULL }, 1, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
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
	RetroAchievements_open(screen);
	BGM_quit();
	SFX_quit();
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return EXIT_SUCCESS;
}
#endif
