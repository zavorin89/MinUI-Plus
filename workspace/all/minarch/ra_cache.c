// Offline cache and unlock queue for RetroAchievements, see ra_cache.h

#define _GNU_SOURCE // strndup
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rc_api_runtime.h"
#include "rc_consoles.h"

#include "defines.h"
#include "utils.h"
#include "ra_cache.h"
#include "ra_http.h"

#define PENDING_PATH RA_CACHE_DIR "/pending.txt" // "<unlock unix time> <achievement id> <game hash> <hardcore 0|1>" per line

///////////////////////////////

int RACache_readCreds(RA_Creds* creds) {
	memset(creds, 0, sizeof(RA_Creds));
	FILE* file = fopen(RA_CREDS_PATH, "r");
	if (!file) return 0;
	char line[256];
	while (fgets(line, sizeof(line), file)) {
		normalizeNewline(line);
		trimTrailingNewlines(line);
		if (prefixMatch("username=", line)) snprintf(creds->username, sizeof(creds->username), "%s", line+9);
		else if (prefixMatch("password=", line)) snprintf(creds->password, sizeof(creds->password), "%s", line+9);
		else if (prefixMatch("token=", line)) snprintf(creds->token, sizeof(creds->token), "%s", line+6);
	}
	fclose(file);
	return creds->username[0] && (creds->password[0] || creds->token[0]);
}
void RACache_saveToken(const char* username, const char* token) {
	FILE* file = fopen(RA_CREDS_PATH, "w");
	if (!file) return;
	fprintf(file, "username=%s\ntoken=%s\n", username, token);
	fclose(file);
	sync();
}

uint32_t RACache_consoleForTag(const char* tag) {
	static const struct { const char* tag; uint32_t console; } map[] = {
		{"GB", RC_CONSOLE_GAMEBOY}, {"SGB", RC_CONSOLE_GAMEBOY}, {"GBC", RC_CONSOLE_GAMEBOY_COLOR},
		{"GBA", RC_CONSOLE_GAMEBOY_ADVANCE}, {"MGBA", RC_CONSOLE_GAMEBOY_ADVANCE},
		{"SFC", RC_CONSOLE_SUPER_NINTENDO}, {"SFC10", RC_CONSOLE_SUPER_NINTENDO}, {"SNESX", RC_CONSOLE_SUPER_NINTENDO}, {"SUPA", RC_CONSOLE_SUPER_NINTENDO},
		{"MD", RC_CONSOLE_MEGA_DRIVE}, {"FC", RC_CONSOLE_NINTENDO}, {"PCE", RC_CONSOLE_PC_ENGINE},
		{"GG", RC_CONSOLE_GAME_GEAR}, {"SMS", RC_CONSOLE_MASTER_SYSTEM},
		{"NGP", RC_CONSOLE_NEOGEO_POCKET}, {"NGPC", RC_CONSOLE_NEOGEO_POCKET},
		{"VB", RC_CONSOLE_VIRTUAL_BOY}, {"PKM", RC_CONSOLE_POKEMON_MINI},
		{"PS", RC_CONSOLE_PLAYSTATION}, // .chd discs are read by ra_chd.c
	};
	for (size_t i=0; i<sizeof(map)/sizeof(map[0]); i++) {
		if (exactMatch((char*)tag, (char*)map[i].tag)) return map[i].console;
	}
	return 0;
}

///////////////////////////////

static int getParam(const char* post_data, const char* name, char* out, size_t size) {
	out[0] = '\0';
	if (!post_data) return 0;
	size_t len = strlen(name);
	const char* p = post_data;
	while (p && *p) {
		if (strncmp(p, name, len)==0 && p[len]=='=') {
			p += len + 1;
			size_t i = 0;
			while (*p && *p!='&' && i<size-1) out[i++] = *p++;
			out[i] = '\0';
			return 1;
		}
		p = strchr(p, '&');
		if (p) p += 1;
	}
	return 0;
}

int RACache_key(const char* post_data, char* key, size_t size) {
	char r[32], value[64];
	getParam(post_data, "r", r, sizeof(r));
	if (exactMatch(r, "login2")) snprintf(key, size, "login");
	else if ((exactMatch(r, "achievementsets") || exactMatch(r, "gameid")) && getParam(post_data, "m", value, sizeof(value))) snprintf(key, size, "%s_%s", r, value);
	else if ((exactMatch(r, "startsession") || exactMatch(r, "unlocks") || exactMatch(r, "patch")) && getParam(post_data, "g", value, sizeof(value))) snprintf(key, size, "%s_%s", r, value);
	else return 0;
	return 1;
}

static void cachePath(const char* key, char* path) {
	sprintf(path, "%s/%s.json", RA_CACHE_DIR, key);
}
int RACache_has(const char* key) {
	char path[MAX_PATH];
	cachePath(key, path);
	return exists(path);
}
char* RACache_read(const char* key, size_t* length) {
	char path[MAX_PATH];
	cachePath(key, path);
	FILE* file = fopen(path, "rb");
	if (!file) return NULL;
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	char* body = size>=0 ? malloc(size+1) : NULL;
	if (!body) {
		fclose(file);
		return NULL;
	}
	size_t got = fread(body, 1, size, file);
	fclose(file);
	body[got] = '\0';
	if (length) *length = got;
	return body;
}
void RACache_write(const char* key, const char* body, size_t length) {
	mkdir(RA_CACHE_DIR, 0755);
	char path[MAX_PATH], tmp_path[MAX_PATH];
	cachePath(key, path);
	sprintf(tmp_path, "%s.tmp", path);
	FILE* file = fopen(tmp_path, "wb");
	if (!file) return;
	fwrite(body, 1, length, file);
	fclose(file);
	rename(tmp_path, path);
}

void RACache_store(const char* post_data, int status, const char* body, size_t length) {
	char key[128];
	if (!RACache_key(post_data, key, sizeof(key))) return;
	if (status==200) RACache_write(key, body, length);
	else if (status==404) {
		strncat(key, "_unknown", sizeof(key)-strlen(key)-1);
		RACache_write(key, body, length);
	}
}
int RACache_isKnownUnknown(const char* key) {
	char unknown[160];
	snprintf(unknown, sizeof(unknown), "%s_unknown", key);
	return RACache_has(unknown);
}

///////////////////////////////

// adds every "<field>":<number> in body to ids (crude, but these responses only use the field for achievement ids)
static int collectIds(const char* body, const char* field, uint32_t* ids, int count, int max) {
	if (!body) return count;
	char needle[32];
	snprintf(needle, sizeof(needle), "\"%s\":", field);
	const char* p = body;
	while ((p = strstr(p, needle)) && count<max) {
		p += strlen(needle);
		uint32_t id = strtoul(p, NULL, 10);
		int known = 0;
		for (int i=0; i<count; i++) if (ids[i]==id) known = 1;
		if (id && !known) ids[count++] = id;
	}
	return count;
}
static int collectArray(const char* body, const char* field, uint32_t* ids, int count, int max) { // "field":[1,2,3]
	if (!body) return count;
	char needle[32];
	snprintf(needle, sizeof(needle), "\"%s\":[", field);
	const char* p = strstr(body, needle);
	if (!p) return count;
	p += strlen(needle);
	while (*p && *p!=']' && count<max) {
		uint32_t id = strtoul(p, (char**)&p, 10);
		if (id) ids[count++] = id;
		while (*p && *p!=',' && *p!=']') p += 1;
		if (*p==',') p += 1;
	}
	return count;
}

// the ids in a json array of objects, eg. "HardcoreUnlocks":[{"ID":1,...},...]
static int collectIdsIn(const char* body, const char* array, uint32_t* ids, int count, int max) {
	if (!body) return count;
	char needle[48];
	snprintf(needle, sizeof(needle), "\"%s\":[", array);
	const char* start = strstr(body, needle);
	if (!start) return count;
	start += strlen(needle);
	const char* end = strchr(start, ']'); // the objects hold no arrays of their own
	if (!end) return count;
	char* part = strndup(start, end - start);
	count = collectIds(part, "ID", ids, count, max);
	free(part);
	return count;
}
// an unlock line's hardcore flag, lines from before it was recorded are softcore
static int pendingHardcore(const char* line) {
	unsigned long t, id;
	char hash[64];
	int hardcore = 0;
	return sscanf(line, "%lu %lu %63s %d", &t, &id, hash, &hardcore)==4 && hardcore;
}

static void queueUnlock(const char* post_data) {
	char a[16], m[64], h[8];
	if (!getParam(post_data, "a", a, sizeof(a))) return;
	getParam(post_data, "m", m, sizeof(m));
	int hardcore = getParam(post_data, "h", h, sizeof(h)) && atoi(h);

	// once per achievement (and mode)
	FILE* file = fopen(PENDING_PATH, "r");
	if (file) {
		char line[128];
		while (fgets(line, sizeof(line), file)) {
			unsigned long t, id;
			if (sscanf(line, "%lu %lu", &t, &id)==2 && id==strtoul(a, NULL, 10) && pendingHardcore(line)==hardcore) {
				fclose(file);
				return;
			}
		}
		fclose(file);
	}
	mkdir(RA_CACHE_DIR, 0755);
	file = fopen(PENDING_PATH, "a");
	if (!file) return;
	fprintf(file, "%lu %s %s %i\n", (unsigned long)time(NULL), a, m[0] ? m : "-", hardcore);
	fclose(file);
	sync();
}

char* RACache_offlineResponse(const char* post_data, size_t* length, int* status) {
	char r[32], key[128];
	getParam(post_data, "r", r, sizeof(r));
	char* body = NULL;
	*status = 200;

	if (exactMatch(r, "startsession")) {
		// rebuild the unlock list from the last session, a sync, and anything earned offline so they don't fire again
		// softcore and hardcore unlocks are kept apart so hardcore ones already earned don't fire again either
		#define MAX_UNLOCKS 1024
		static uint32_t ids[MAX_UNLOCKS], hard_ids[MAX_UNLOCKS];
		int count = 0, hard_count = 0;
		char g[16];
		getParam(post_data, "g", g, sizeof(g));
		snprintf(key, sizeof(key), "startsession_%s", g);
		char* session = RACache_read(key, NULL);
		count = collectIdsIn(session, "Unlocks", ids, count, MAX_UNLOCKS);
		hard_count = collectIdsIn(session, "HardcoreUnlocks", hard_ids, hard_count, MAX_UNLOCKS);
		if (session) free(session);
		snprintf(key, sizeof(key), "unlocks_%s", g);
		char* unlocks = RACache_read(key, NULL);
		count = collectArray(unlocks, "UserUnlocks", ids, count, MAX_UNLOCKS);
		if (unlocks) free(unlocks);
		FILE* file = fopen(PENDING_PATH, "r");
		if (file) {
			char line[128];
			unsigned long t, id;
			while (fgets(line, sizeof(line), file)) {
				if (sscanf(line, "%lu %lu", &t, &id)!=2) continue;
				if (pendingHardcore(line)) { if (hard_count<MAX_UNLOCKS) hard_ids[hard_count++] = id; }
				else if (count<MAX_UNLOCKS) ids[count++] = id;
			}
			fclose(file);
		}

		body = malloc(64 + (count+hard_count)*32);
		char* out = body + sprintf(body, "{\"Success\":true,\"Unlocks\":[");
		for (int i=0; i<count; i++) out += sprintf(out, "%s{\"ID\":%u,\"When\":0}", i ? "," : "", ids[i]);
		out += sprintf(out, "],\"HardcoreUnlocks\":[");
		for (int i=0; i<hard_count; i++) out += sprintf(out, "%s{\"ID\":%u,\"When\":0}", i ? "," : "", hard_ids[i]);
		sprintf(out, "]}");
	}
	else if (RACache_key(post_data, key, sizeof(key))) {
		body = RACache_read(key, NULL); // sign in and game data only work offline once cached
		if (!body && RACache_isKnownUnknown(key)) { // not in RetroAchievements, answer the same way the server did
			strncat(key, "_unknown", sizeof(key)-strlen(key)-1);
			body = RACache_read(key, NULL);
			*status = 404;
		}
	}
	else if (exactMatch(r, "awardachievement")) {
		queueUnlock(post_data);
		body = strdup("{\"Success\":true}");
	}
	else if (exactMatch(r, "ping") || exactMatch(r, "submitlbentry")) {
		body = strdup("{\"Success\":true}"); // nothing worth keeping
	}

	if (body && length) *length = strlen(body);
	return body;
}

///////////////////////////////

#define BADGE_DIR RA_CACHE_DIR "/badges"
#define BADGE_MAGIC "RAB1"

typedef struct BadgeEntry {
	RA_BadgeName name;
	uint32_t offset;
	uint32_t size;
} BadgeEntry;

static void badgePackPath(uint32_t game_id, char* path) {
	sprintf(path, "%s/%u.bin", BADGE_DIR, game_id);
}

// reads a pack's index (and optionally all of its data), returns the entry count or -1
static int readBadgePack(uint32_t game_id, BadgeEntry** entries, unsigned char** data, size_t* data_size) {
	*entries = NULL;
	if (data) *data = NULL;
	char path[MAX_PATH];
	badgePackPath(game_id, path);
	FILE* file = fopen(path, "rb");
	if (!file) return -1;
	char magic[4];
	uint32_t count = 0;
	if (fread(magic, 1, 4, file)!=4 || memcmp(magic, BADGE_MAGIC, 4)!=0 || fread(&count, 4, 1, file)!=1 || count>4096) {
		fclose(file);
		return -1;
	}
	*entries = calloc(count ? count : 1, sizeof(BadgeEntry));
	if (fread(*entries, sizeof(BadgeEntry), count, file)!=count) {
		free(*entries);
		*entries = NULL;
		fclose(file);
		return -1;
	}
	if (data) {
		fseek(file, 0, SEEK_END);
		*data_size = ftell(file);
		*data = malloc(*data_size);
		fseek(file, 0, SEEK_SET);
		*data_size = fread(*data, 1, *data_size, file);
	}
	fclose(file);
	return count;
}

static unsigned char* downloadBadge(const char* name, const char* user_agent, size_t* size) {
	rc_api_fetch_image_request_t params = {0};
	params.image_name = name;
	params.image_type = RC_IMAGE_TYPE_ACHIEVEMENT;
	rc_api_request_t request;
	if (rc_api_init_fetch_image_request(&request, &params)!=RC_OK) return NULL;
	unsigned char* png = NULL;
	HTTP_Response response;
	if (HTTP_request(request.url, NULL, NULL, user_agent, RA_CA_PATH, &response)) {
		// only keep real PNGs, an error page saved here would never be replaced
		if (response.status==200 && response.body_length>8 && memcmp(response.body, "\x89PNG", 4)==0) {
			png = (unsigned char*)response.body;
			*size = response.body_length;
			response.body = NULL; // handed to the caller
		}
		HTTP_freeResponse(&response);
	}
	rc_api_destroy_request(&request);
	return png;
}

int RACache_fetchBadges(uint32_t game_id, const RA_BadgeName* names, int count, const char* user_agent) {
	if (!game_id || count<=0) return 0;
	BadgeEntry* old_entries;
	unsigned char* old_data;
	size_t old_size = 0;
	int old_count = readBadgePack(game_id, &old_entries, &old_data, &old_size);
	if (old_count<0) old_count = 0;
	
	// everything already in the pack is kept; anything new is downloaded
	BadgeEntry* entries = calloc(old_count + count, sizeof(BadgeEntry));
	unsigned char** blobs = calloc(old_count + count, sizeof(unsigned char*));
	int total = 0, downloaded = 0;
	for (int i=0; i<old_count; i++) {
		if (old_entries[i].offset + old_entries[i].size > old_size) continue;
		entries[total] = old_entries[i];
		blobs[total] = old_data + old_entries[i].offset;
		total += 1;
	}
	for (int i=0; i<count; i++) {
		if (!names[i][0]) continue;
		int known = 0;
		for (int j=0; j<total; j++) if (strncmp(entries[j].name, names[i], sizeof(RA_BadgeName))==0) known = 1;
		if (known) continue;
		size_t size;
		unsigned char* png = downloadBadge(names[i], user_agent, &size);
		if (!png) continue;
		memset(entries[total].name, 0, sizeof(RA_BadgeName));
		strncpy(entries[total].name, names[i], sizeof(RA_BadgeName)-1);
		entries[total].size = size;
		blobs[total] = png;
		total += 1;
		downloaded += 1;
	}
	
	if (downloaded) {
		mkdir(RA_CACHE_DIR, 0755);
		mkdir(BADGE_DIR, 0755);
		char path[MAX_PATH], tmp_path[MAX_PATH];
		badgePackPath(game_id, path);
		sprintf(tmp_path, "%s.tmp", path);
		FILE* file = fopen(tmp_path, "wb");
		if (file) {
			uint32_t offset = 8 + total * sizeof(BadgeEntry);
			for (int i=0; i<total; i++) {
				entries[i].offset = offset;
				offset += entries[i].size;
			}
			uint32_t n = total;
			fwrite(BADGE_MAGIC, 1, 4, file);
			fwrite(&n, 4, 1, file);
			fwrite(entries, sizeof(BadgeEntry), total, file);
			for (int i=0; i<total; i++) fwrite(blobs[i], 1, entries[i].size, file);
			fclose(file);
			rename(tmp_path, path);
		}
	}
	
	for (int i=0; i<total; i++) { // new downloads live outside the old pack's memory
		if (blobs[i] && (!old_data || blobs[i]<old_data || blobs[i]>=old_data+old_size)) free(blobs[i]);
	}
	free(blobs);
	free(entries);
	if (old_entries) free(old_entries);
	if (old_data) free(old_data);
	return total;
}

unsigned char* RACache_loadBadge(uint32_t game_id, const char* name, size_t* size) {
	if (!game_id || !name || !name[0]) return NULL;
	BadgeEntry* entries;
	int count = readBadgePack(game_id, &entries, NULL, NULL);
	if (count<=0) {
		if (entries) free(entries);
		return NULL;
	}
	unsigned char* png = NULL;
	for (int i=0; i<count; i++) {
		if (strncmp(entries[i].name, name, sizeof(RA_BadgeName))!=0) continue;
		char path[MAX_PATH];
		badgePackPath(game_id, path);
		FILE* file = fopen(path, "rb");
		if (file) {
			png = malloc(entries[i].size);
			fseek(file, entries[i].offset, SEEK_SET);
			if (fread(png, 1, entries[i].size, file)!=entries[i].size) {
				free(png);
				png = NULL;
			}
			else *size = entries[i].size;
			fclose(file);
		}
		break;
	}
	free(entries);
	return png;
}

///////////////////////////////

int RACache_pendingCount(void) {
	FILE* file = fopen(PENDING_PATH, "r");
	if (!file) return 0;
	int count = 0;
	char line[128];
	while (fgets(line, sizeof(line), file)) if (strlen(line)>2) count += 1;
	fclose(file);
	return count;
}

int RACache_sendPending(const RA_Creds* creds, const char* user_agent) {
	if (!creds->token[0]) return 0;
	FILE* file = fopen(PENDING_PATH, "r");
	if (!file) return 0;

	char kept[64][128];
	int kept_count = 0;
	int sent = 0;
	char line[128];
	while (fgets(line, sizeof(line), file)) {
		unsigned long when, id;
		char hash[64];
		if (sscanf(line, "%lu %lu %63s", &when, &id, hash)!=3) continue;

		rc_api_award_achievement_request_t params = {0};
		params.username = creds->username;
		params.api_token = creds->token;
		params.achievement_id = id;
		params.hardcore = pendingHardcore(line);
		params.game_hash = exactMatch(hash, "-") ? NULL : hash;
		unsigned long now = time(NULL);
		params.seconds_since_unlock = now>when ? now - when : 0; // keeps the original unlock time

		rc_api_request_t request;
		int accepted = 0;
		if (rc_api_init_award_achievement_request(&request, &params)==RC_OK) {
			HTTP_Response response;
			if (HTTP_request(request.url, request.post_data, request.content_type, user_agent, RA_CA_PATH, &response)) {
				// a 4xx means the server will never take it (eg. already unlocked), so stop retrying those too
				accepted = response.status==200 || (response.status>=400 && response.status<500);
				HTTP_freeResponse(&response);
			}
			rc_api_destroy_request(&request);
		}
		if (accepted) sent += 1;
		else if (kept_count<64) snprintf(kept[kept_count++], sizeof(kept[0]), "%s", line);
	}
	fclose(file);

	if (kept_count==0) unlink(PENDING_PATH);
	else {
		file = fopen(PENDING_PATH, "w");
		if (file) {
			for (int i=0; i<kept_count; i++) fputs(kept[i], file);
			fclose(file);
		}
	}
	sync();
	return sent;
}
