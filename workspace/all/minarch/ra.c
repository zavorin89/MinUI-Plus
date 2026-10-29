// RetroAchievements (softcore) for minarch, built on rcheevos' rc_client.
// Sign in uses .userdata/shared/retroachievements.txt:
//   username=you
//   password=secret      (replaced with token= after the first sign in)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <msettings.h>

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_libretro.h"

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "ra.h"
#include "ra_http.h"
#include "ra_cache.h"
#include "ra_chd.h"
#include "rc_hash.h"

#define RA_NOTIFY_MS 4000 // unlocks, a little shorter for status messages
#define RA_STATUS_MS 3000
#define RA_FADE_IN_MS 150
#define RA_FADE_OUT_MS 300
#define RA_QUIT_WAIT_MS 5000

static rc_client_t* client = NULL;
static rc_libretro_memory_regions_t memory;
static int memory_ready = 0;
static RA_GetMemory core_get_memory;
static uint32_t console_id = 0;
static uint32_t game_id = 0; // set once the game is identified, names its badge pack
static char game_path[MAX_PATH];
static const void* game_data = NULL; // minarch's copy of the ROM (it outlives RA_quit), NULL to hash from the file
static size_t game_size = 0;
static int hardcore = 0; // chosen in Tools/RetroAchievements, can drop to 0 for the session
static volatile int load_finished = 0; // sign in and identification are over, however they went
static volatile int hash_retry = 0; // identifying failed reading the game: tried once more a few seconds in
static uint32_t hash_retry_at = 0;
static void identify(void);
static char user_agent[160];

// notifications are queued from any thread and shown one after another on the main thread
typedef struct Notification {
	char title[128]; // small line on top
	char text[160]; // large line
	char detail[256]; // small line below, "" for none
	char badge[16]; // badge image name, "" for none
	int ms; // time on screen
} Notification;
#define NOTIFY_QUEUE_SIZE 8
static pthread_mutex_t notify_mx = PTHREAD_MUTEX_INITIALIZER;
static Notification notify_queue[NOTIFY_QUEUE_SIZE];
static int notify_head = 0;
static int notify_count = 0;
static int notify_showing = 0; // one is on screen
static uint32_t notify_shown_at = 0;
static int notify_ms = 0;
static int notify_alpha = -1;
static volatile int dismiss_requested = 0; // MENU pressed over a popup (asked from the core's thread too)
static uint32_t dismissed_at = 0; // fading out early, from dismissed_alpha
static int dismissed_alpha = 0;
#define DISMISS_FADE_MS 150

static pthread_mutex_t jobs_mx = PTHREAD_MUTEX_INITIALIZER;
static int jobs_running = 0;

///////////////////////////////

static void notifyFull(const char* title, const char* text, const char* detail, const char* badge, int ms) {
	pthread_mutex_lock(&notify_mx);
	if (notify_count==NOTIFY_QUEUE_SIZE) { // full: drop the oldest waiting one
		notify_head = (notify_head + 1) % NOTIFY_QUEUE_SIZE;
		notify_count -= 1;
	}
	Notification* n = &notify_queue[(notify_head + notify_count) % NOTIFY_QUEUE_SIZE];
	snprintf(n->title, sizeof(n->title), "%s", title);
	snprintf(n->text, sizeof(n->text), "%s", text ? text : "");
	snprintf(n->detail, sizeof(n->detail), "%s", detail ? detail : "");
	snprintf(n->badge, sizeof(n->badge), "%s", badge ? badge : "");
	n->ms = ms;
	notify_count += 1;
	pthread_mutex_unlock(&notify_mx);
}
static void notify(const char* title, const char* text) {
	notifyFull(title, text, NULL, NULL, RA_STATUS_MS);
}

///////////////////////////////

typedef struct Job {
	char* url;
	char* post_data;
	char* content_type;
	rc_client_server_callback_t callback;
	void* callback_data;
} Job;

static RA_Creds creds;
static pthread_mutex_t pending_mx = PTHREAD_MUTEX_INITIALIZER;
static void sendPending(void) { // after any successful request, so a connection that came back gets used
	if (pthread_mutex_trylock(&pending_mx)!=0) return; // another thread is already on it
	if (RACache_pendingCount()) {
		int sent = RACache_sendPending(&creds, user_agent);
		if (sent) {
			char text[64];
			snprintf(text, sizeof(text), "%i offline unlock%s sent", sent, sent==1 ? "" : "s");
			notify("RetroAchievements", text);
		}
	}
	pthread_mutex_unlock(&pending_mx);
}

static void* jobThread(void* arg) {
	Job* job = arg;
	HTTP_Response http = {0};
	rc_api_server_response_t response = {0};
	char key[128];
	int cacheable = RACache_key(job->post_data, key, sizeof(key));
	char* offline = NULL;
	int online = HTTP_request(job->url, job->post_data, job->content_type, user_agent, RA_CA_PATH, &http);
	if (online) {
		response.body = http.body;
		response.body_length = http.body_length;
		response.http_status_code = http.status;
		if (cacheable) RACache_store(job->post_data, http.status, http.body, http.body_length); // keeps this game playable offline
	}
	else if ((offline = RACache_offlineResponse(job->post_data, &response.body_length, &response.http_status_code))) {
		response.body = offline;
	}
	else response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;

	job->callback(&response, job->callback_data);

	HTTP_freeResponse(&http);
	if (offline) free(offline);
	if (online && http.status==200) sendPending();
	free(job->url);
	if (job->post_data) free(job->post_data);
	if (job->content_type) free(job->content_type);
	free(job);

	pthread_mutex_lock(&jobs_mx);
	jobs_running -= 1;
	pthread_mutex_unlock(&jobs_mx);
	return NULL;
}
static void serverCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t* c) {
	// requests run on their own thread so a slow connection never stalls the game
	Job* job = calloc(1, sizeof(Job));
	job->url = strdup(request->url);
	job->post_data = request->post_data ? strdup(request->post_data) : NULL;
	job->content_type = request->content_type ? strdup(request->content_type) : NULL;
	job->callback = callback;
	job->callback_data = callback_data;

	pthread_mutex_lock(&jobs_mx);
	jobs_running += 1;
	pthread_mutex_unlock(&jobs_mx);

	pthread_t thread;
	if (pthread_create(&thread, NULL, jobThread, job)==0) pthread_detach(thread);
	else jobThread(job); // still answer rc_client
}

static uint32_t readMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* c) {
	if (!memory_ready) return 0;
	return rc_libretro_memory_read(&memory, address, buffer, num_bytes);
}
static void getCoreMemoryInfo(uint32_t id, rc_libretro_core_memory_info_t* info) {
	core_get_memory(id, &info->data, &info->size);
}

///////////////////////////////

static void eventHandler(const rc_client_event_t* event, rc_client_t* c) {
	char text[160];
	switch (event->type) {
		case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
			rc_client_user_game_summary_t summary;
			rc_client_get_user_game_summary(c, &summary);
			char title[128];
			snprintf(title, sizeof(title), "Achievement unlocked  \xC2\xB7  %u pts  \xC2\xB7  %u of %u",
				event->achievement->points, summary.num_unlocked_achievements, summary.num_promoted_achievements);
			notifyFull(title, event->achievement->title, event->achievement->description, event->achievement->badge_name, RA_NOTIFY_MS);
			SFX_play(SFX_ACHIEVEMENT);
			break;
		}
		case RC_CLIENT_EVENT_GAME_COMPLETED: {
			const rc_client_game_t* game = rc_client_get_game_info(c);
			notifyFull("Set complete!", game ? game->title : NULL, "Every achievement unlocked", NULL, RA_NOTIFY_MS);
			SFX_play(SFX_ACHIEVEMENT);
			break;
		}
		case RC_CLIENT_EVENT_DISCONNECTED:
			notify("RetroAchievements", "Offline, unlocks will be sent later");
			break;
		case RC_CLIENT_EVENT_RECONNECTED:
			notify("RetroAchievements", "Unlocks sent");
			break;
	}
}

// downloads the game's badges in the background so unlock popups (and offline play) have their images
typedef struct BadgeJob {
	uint32_t game_id;
	int count;
	RA_BadgeName* names;
} BadgeJob;
static void* badgeThread(void* arg) {
	BadgeJob* job = arg;
	RACache_fetchBadges(job->game_id, job->names, job->count, user_agent);
	free(job->names);
	free(job);
	return NULL;
}
static void fetchBadges(rc_client_t* c) {
	rc_client_achievement_list_t* list = rc_client_create_achievement_list(c, RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	if (!list) return;
	BadgeJob* job = calloc(1, sizeof(BadgeJob));
	job->game_id = game_id;
	int total = 0;
	for (uint32_t b=0; b<list->num_buckets; b++) total += list->buckets[b].num_achievements;
	job->names = calloc(total ? total : 1, sizeof(job->names[0]));
	for (uint32_t b=0; b<list->num_buckets; b++) {
		for (uint32_t i=0; i<list->buckets[b].num_achievements; i++) {
			snprintf(job->names[job->count++], 8, "%s", list->buckets[b].achievements[i]->badge_name);
		}
	}
	rc_client_destroy_achievement_list(list);
	pthread_t thread;
	if (pthread_create(&thread, NULL, badgeThread, job)==0) pthread_detach(thread);
	else {
		free(job->names);
		free(job);
	}
}

static void RC_CCONV clientLog(const char* message, const rc_client_t* c) {
	LOG_info("RetroAchievements: %s\n", message);
}
static void RC_CCONV hashError(const char* message, const struct rc_hash_iterator* iterator) {
	LOG_info("RetroAchievements hash: %s\n", message);
}
static void loadCallback(int result, const char* error_message, rc_client_t* c, void* userdata) {
	load_finished = 1;
	if (result==RC_NO_GAME_LOADED) return; // not in RetroAchievements' database, nothing to show
	if (result!=RC_OK) {
		LOG_info("RetroAchievements: couldn't load the game: %s (%i)\n", error_message ? error_message : "", result);
		// a game that couldn't be identified is left alone, like one without achievements; other trouble is shown
		if (error_message && strstr(error_message, "hash")) {
			if (!hash_retry) hash_retry = 1; // (it can collide with the core reading the same disc as it starts)
			return;
		}
		notify("RetroAchievements", error_message ? error_message : "Couldn't load achievements");
		return;
	}
	const rc_client_game_t* game = rc_client_get_game_info(c);
	game_id = game->id;
	rc_client_user_game_summary_t summary;
	rc_client_get_user_game_summary(c, &summary);
	char detail[160];
	if (summary.num_promoted_achievements==0) { // known, but nothing to unlock yet: no popup
		LOG_info("RetroAchievements: %s has no achievements yet\n", game->title);
		return;
	}
	snprintf(detail, sizeof(detail), "%u of %u unlocked  \xC2\xB7  %u of %u points",
		summary.num_unlocked_achievements, summary.num_promoted_achievements, summary.points_unlocked, summary.points_available);
	notifyFull(hardcore ? "RetroAchievements  \xC2\xB7  Hardcore" : "RetroAchievements", game->title, detail, NULL, RA_STATUS_MS);
	fetchBadges(c);
}

static void loginCallback(int result, const char* error_message, rc_client_t* c, void* userdata) {
	if (result!=RC_OK) {
		load_finished = 1;
		notify("RetroAchievements sign in failed", error_message);
		return;
	}
	const rc_client_user_t* user = rc_client_get_user_info(c);
	if (user && user->token) {
		if (!exactMatch(creds.token, (char*)user->token)) RACache_saveToken(user->username, user->token);
		snprintf(creds.username, sizeof(creds.username), "%s", user->username);
		snprintf(creds.token, sizeof(creds.token), "%s", user->token);
	}
	identify();
}
// from memory when minarch has it there (and it's a cartridge system rcheevos can hash that way):
// a big ROM isn't read from the card a second time. Disc systems hash from the file
static const uint32_t from_memory[] = {
	RC_CONSOLE_GAMEBOY, RC_CONSOLE_GAMEBOY_COLOR, RC_CONSOLE_GAMEBOY_ADVANCE, RC_CONSOLE_NINTENDO, RC_CONSOLE_SUPER_NINTENDO,
	RC_CONSOLE_NINTENDO_64, RC_CONSOLE_NINTENDO_DS, RC_CONSOLE_VIRTUAL_BOY, RC_CONSOLE_POKEMON_MINI, RC_CONSOLE_MEGA_DRIVE,
	RC_CONSOLE_MASTER_SYSTEM, RC_CONSOLE_GAME_GEAR, RC_CONSOLE_SEGA_32X, RC_CONSOLE_SG1000, RC_CONSOLE_PC_ENGINE,
	RC_CONSOLE_NEOGEO_POCKET, RC_CONSOLE_WONDERSWAN, RC_CONSOLE_ATARI_LYNX, RC_CONSOLE_ATARI_2600, RC_CONSOLE_ATARI_7800,
	RC_CONSOLE_COLECOVISION, 0,
};
static void identify(void) {
	const void* data = NULL;
	for (int i=0; from_memory[i] && game_data; i++) if (from_memory[i]==console_id) data = game_data;
	rc_client_begin_identify_and_load_game(client, console_id, game_path, data, data ? game_size : 0, loadCallback, NULL);
}

///////////////////////////////

void RA_init(const char* core_tag, const char* rom_path, const void* rom_data, size_t rom_size, const struct retro_memory_map* mmap, RA_GetMemory get_memory) {
	game_data = rom_data;
	game_size = rom_size;
	if (!RACache_readCreds(&creds)) return;
	console_id = RACache_consoleForTag(core_tag);
	if (!console_id) return;
	if (suffixMatch(".zip", (char*)rom_path)) return; // zips only hash correctly once extracted
	snprintf(game_path, sizeof(game_path), "%s", rom_path);

	core_get_memory = get_memory;
	memory_ready = rc_libretro_memory_init(&memory, mmap, getCoreMemoryInfo, console_id);

	client = rc_client_create(readMemory, serverCall);
	rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_WARN, clientLog); // its warnings and errors, in the log
	rc_client_set_event_handler(client, eventHandler);
	rc_hash_callbacks_t hash_callbacks = {0}; // rcheevos' own readers, plus .chd for disc games
	RA_getDiscReader(&hash_callbacks.cdreader);
	hash_callbacks.error_message = hashError; // why a game couldn't be identified, in the log
	rc_client_set_hash_callbacks(client, &hash_callbacks);
	hardcore = RA_HARDCORE_AVAILABLE && exists(RA_HARDCORE_PATH);
	load_finished = 0;
	hash_retry = 0;
	rc_client_set_hardcore_enabled(client, hardcore); // hardcore games start fresh instead of resuming a state, see minarch

	char clause[96] = "";
	rc_client_get_user_agent_clause(client, clause, sizeof(clause));
	snprintf(user_agent, sizeof(user_agent), "MinUI/%s %s", PLATFORM, clause);

	if (creds.token[0]) rc_client_begin_login_with_token(client, creds.username, creds.token, loginCallback, NULL);
	else rc_client_begin_login_with_password(client, creds.username, creds.password, loginCallback, NULL);
}

void RA_doFrame(void) {
	if (client) rc_client_do_frame(client);
}

static SDL_Surface* loadBadge(const char* name) { // NULL when it isn't in this game's pack yet
	size_t size = 0;
	unsigned char* png = RACache_loadBadge(game_id, name, &size);
	if (!png) return NULL;
	SDL_Surface* badge = IMG_Load_RW(SDL_RWFromConstMem(png, size), 1);
	free(png);
	return badge;
}

// renders text as up to max_lines lines no wider than max_w, breaking between words
// only the last allowed line is cut short (with an ellipsis) when there's still more
#define WRAP_MAX_LINES 3
static int wrapLines(TTF_Font* f, const char* text, int max_w, int max_lines, SDL_Color color, SDL_Surface** out) {
	char rest[512];
	snprintf(rest, sizeof(rest), "%s", text);
	char* s = rest;
	int count = 0;
	while (*s && count<max_lines) {
		while (*s==' ') s++;
		if (!*s) break;
		char line[512];
		if (count==max_lines-1) snprintf(line, sizeof(line), "%s", s); // last line takes the rest
		else {
			// grow word by word until the next one wouldn't fit
			int len = 0;
			char* end = s;
			while (*end) {
				char* next = strchr(end + (*end==' '), ' ');
				int next_len = next ? next - s : (int)strlen(s);
				char test[512];
				snprintf(test, sizeof(test), "%.*s", next_len, s);
				int w;
				TTF_SizeUTF8(f, test, &w, NULL);
				if (w>max_w && len) break;
				len = next_len;
				end = s + len;
				if (!next) break;
			}
			snprintf(line, sizeof(line), "%.*s", len, s);
			s += len;
		}
		char fitted[512];
		GFX_truncateText(f, line, fitted, max_w, 0);
		out[count++] = TTF_RenderUTF8_Blended(f, fitted, color);
		if (count==max_lines) break;
	}
	return count;
}

// draws a notification onto a transparent full screen surface for the platform to show over the game
static void showNotification(Notification* n) {
	SDL_Surface* overlay = SDL_CreateRGBSurfaceWithFormat(0, FIXED_WIDTH, FIXED_HEIGHT, 32, SDL_PIXELFORMAT_RGBA32);
	SDL_FillRect(overlay, NULL, SDL_MapRGBA(overlay->format, 0,0,0,0));

	SDL_Surface* badge = n->badge[0] ? loadBadge(n->badge) : NULL;
	int badge_size = badge ? SCALE1(32) : 0; // badges are 64x64, which is 32 at 1x
	int badge_gap = badge ? SCALE1(BUTTON_PADDING) : 0;
	int pad = SCALE1(BUTTON_PADDING);
	int max_text_w = FIXED_WIDTH - SCALE1(PADDING*2) - pad*2 - badge_size - badge_gap;

	// long names wrap onto more lines so they show in full, the box grows to fit
	SDL_Surface* lines[WRAP_MAX_LINES*3];
	int line_count = 0;
	line_count += wrapLines(font.small, n->title, max_text_w, 2, COLOR_DARK_TEXT, lines + line_count);
	if (n->text[0]) line_count += wrapLines(font.large, n->text, max_text_w, WRAP_MAX_LINES, COLOR_WHITE, lines + line_count);
	if (n->detail[0]) line_count += wrapLines(font.small, n->detail, max_text_w, 2, COLOR_WHITE, lines + line_count);

	int text_w = 0;
	int text_h = 0;
	for (int i=0; i<line_count; i++) {
		text_w = MAX(text_w, lines[i]->w);
		text_h += lines[i]->h;
	}
	int w = pad + badge_size + badge_gap + text_w + pad;
	int h = (MAX(text_h, badge_size)) + pad; // MAX() has no outer parens
	int x = (FIXED_WIDTH - w) / 2;
	int y = SCALE1(PADDING*3); // clear of the top edge and of screen borders (eg. GBA's)

	// rounded black box, slightly see-through
	int r = SCALE1(10);
	uint32_t black = SDL_MapRGBA(overlay->format, 0,0,0,235);
	for (int py=0; py<h; py++) {
		for (int px=0; px<w; px++) {
			int dx = px<r ? r-px : (px>=w-r ? px-(w-r-1) : 0);
			int dy = py<r ? r-py : (py>=h-r ? py-(h-r-1) : 0);
			if (dx*dx + dy*dy > r*r) continue;
			((uint32_t*)overlay->pixels)[(y+py)*overlay->w + x+px] = black;
		}
	}
	if (badge) {
		int bx = x + pad;
		int by = y + (h - badge_size)/2;
		SDL_BlitScaled(badge, NULL, overlay, &(SDL_Rect){ bx, by, badge_size, badge_size });
		SDL_FreeSurface(badge);
		// round the badge's corners into the box
		int br = SCALE1(5);
		for (int py=0; py<badge_size; py++) {
			for (int px=0; px<badge_size; px++) {
				int dx = px<br ? br-px : (px>=badge_size-br ? px-(badge_size-br-1) : 0);
				int dy = py<br ? br-py : (py>=badge_size-br ? py-(badge_size-br-1) : 0);
				if (dx*dx + dy*dy > br*br) ((uint32_t*)overlay->pixels)[(by+py)*overlay->w + bx+px] = black;
			}
		}
	}
	int text_x = x + pad + badge_size + badge_gap;
	int text_y = y + (h - text_h)/2;
	for (int i=0; i<line_count; i++) {
		SDL_BlitSurface(lines[i], NULL, overlay, &(SDL_Rect){ text_x, text_y });
		text_y += lines[i]->h;
		SDL_FreeSurface(lines[i]);
	}

	GFX_setNotification(overlay);
	SDL_FreeSurface(overlay);
}

void RA_update(void) {
	if (!client) return;
	uint32_t now = SDL_GetTicks();
	if (hash_retry==1) { // once, a few seconds after it failed
		hash_retry = 2;
		hash_retry_at = now + 5000;
	}
	if (hash_retry==2 && (int32_t)(now - hash_retry_at)>=0) {
		hash_retry = 3;
		LOG_info("RetroAchievements: identifying the game again\n");
		load_finished = 0;
		identify();
	}

	if (!notify_showing) {
		Notification n;
		int has_next = 0;
		pthread_mutex_lock(&notify_mx);
		if (notify_count) {
			n = notify_queue[notify_head];
			notify_head = (notify_head + 1) % NOTIFY_QUEUE_SIZE;
			notify_count -= 1;
			has_next = 1;
		}
		pthread_mutex_unlock(&notify_mx);
		if (!has_next) return;
		GFX_setNotificationAlpha(0);
		notify_alpha = 0;
		showNotification(&n);
		notify_showing = 1;
		notify_shown_at = now;
		notify_ms = n.ms;
		dismiss_requested = 0;
		dismissed_at = 0;
	}

	// dismissed: a quick fade from wherever it is
	if (dismiss_requested && !dismissed_at) {
		dismissed_at = now;
		dismissed_alpha = notify_alpha<0 ? 255 : notify_alpha;
	}
	if (dismissed_at) {
		int t = now - dismissed_at;
		if (t>=DISMISS_FADE_MS) {
			GFX_setNotification(NULL);
			notify_showing = 0;
			dismiss_requested = 0;
			dismissed_at = 0;
			return;
		}
		int alpha = dismissed_alpha * (DISMISS_FADE_MS - t) / DISMISS_FADE_MS;
		if (alpha!=notify_alpha) {
			GFX_setNotificationAlpha(alpha);
			notify_alpha = alpha;
		}
		return;
	}

	// fade in, hold, fade out, then make way for the next one
	int t = now - notify_shown_at;
	if (t>=notify_ms) {
		GFX_setNotification(NULL);
		notify_showing = 0;
		return;
	}
	int alpha = 255;
	if (t<RA_FADE_IN_MS) alpha = t * 255 / RA_FADE_IN_MS;
	else if (t>notify_ms-RA_FADE_OUT_MS) alpha = (notify_ms - t) * 255 / RA_FADE_OUT_MS;
	if (alpha!=notify_alpha) {
		GFX_setNotificationAlpha(alpha);
		notify_alpha = alpha;
	}
}

int RA_dismissNotification(void) {
	if (!notify_showing || dismiss_requested) return 0;
	pthread_mutex_lock(&notify_mx);
	notify_count = 0; // the ones waiting go too, the list has them all
	pthread_mutex_unlock(&notify_mx);
	dismiss_requested = 1;
	return 1;
}

void RA_quit(void) {
	if (!client) return;
	// give unlocks earned right before quitting a chance to reach the server
	for (int waited=0; waited<RA_QUIT_WAIT_MS; waited+=100) {
		pthread_mutex_lock(&jobs_mx);
		int running = jobs_running;
		pthread_mutex_unlock(&jobs_mx);
		if (!running) break;
		usleep(100000);
	}
	GFX_setNotification(NULL);
	notify_showing = 0;
	rc_client_unload_game(client);
	rc_client_destroy(client);
	client = NULL;
	if (memory_ready) rc_libretro_memory_destroy(&memory);
	memory_ready = 0;
}

///////////////////////////////

int RA_hasAchievements(void) {
	return client && rc_client_get_game_info(client) && rc_client_has_achievements(client);
}
void RA_notify(const char* title, const char* text, const char* detail) {
	if (client) notifyFull(title, text, detail, NULL, RA_NOTIFY_MS);
}
int RA_isLoading(void) {
	return client && !load_finished;
}
int RA_isHardcore(void) {
	return client && hardcore && RA_hasAchievements();
}
void RA_stateLoaded(void) {
	if (!client || !hardcore) return;
	hardcore = 0;
	rc_client_set_hardcore_enabled(client, 0);
	if (RA_hasAchievements()) notifyFull("RetroAchievements", "Hardcore off for this session", "A save state was loaded, unlocks count as softcore", NULL, RA_NOTIFY_MS);
}

typedef struct Row {
	const char* header; // section label, or NULL for an achievement
	const rc_client_achievement_t* achievement;
} Row;

#define LIST_ROWS 4 // leaves room for the summary above and the description below

// X cycles the order of the achievements list, remembered between games
enum {
	SORT_DEFAULT, // rcheevos' own sections: recently unlocked, almost there, locked, unlocked...
	SORT_UNLOCKED_FIRST,
	SORT_POINTS,
	SORT_NAME,
	SORT_COUNT,
};
static char* sort_labels[SORT_COUNT] = { "Default order", "Unlocked first", "Most points", "A to Z" };
#define SORT_PATH RA_CACHE_DIR "/list_sort.txt"

static int byPoints(const void* a, const void* b) {
	const rc_client_achievement_t* x = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* y = *(const rc_client_achievement_t**)b;
	if (x->points!=y->points) return x->points<y->points ? 1 : -1;
	return strcasecmp(x->title, y->title);
}
static int byName(const void* a, const void* b) {
	const rc_client_achievement_t* x = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* y = *(const rc_client_achievement_t**)b;
	return strcasecmp(x->title, y->title);
}

// fills rows (sized for every achievement plus a header per bucket) in the given order, returns how many
static int buildRows(rc_client_achievement_list_t* list, int sort, Row* rows) {
	int n = 0;
	if (sort==SORT_DEFAULT) {
		for (uint32_t b=0; b<list->num_buckets; b++) {
			rows[n++] = (Row){ list->buckets[b].label, NULL };
			for (uint32_t i=0; i<list->buckets[b].num_achievements; i++) rows[n++] = (Row){ NULL, list->buckets[b].achievements[i] };
		}
		return n;
	}

	int total = 0;
	for (uint32_t b=0; b<list->num_buckets; b++) total += list->buckets[b].num_achievements;
	const rc_client_achievement_t** all = calloc(total ? total : 1, sizeof(*all));
	int count = 0;
	for (uint32_t b=0; b<list->num_buckets; b++) {
		for (uint32_t i=0; i<list->buckets[b].num_achievements; i++) all[count++] = list->buckets[b].achievements[i];
	}

	if (sort==SORT_UNLOCKED_FIRST) {
		for (int pass=0; pass<2; pass++) { // unlocked, then locked, each in the set's order
			int want = pass==0;
			int header = n;
			for (int i=0; i<count; i++) {
				if (!!all[i]->unlocked!=want) continue;
				if (n==header) rows[n++] = (Row){ want ? "Unlocked" : "Locked", NULL };
				rows[n++] = (Row){ NULL, all[i] };
			}
		}
	}
	else {
		qsort(all, count, sizeof(*all), sort==SORT_POINTS ? byPoints : byName);
		rows[n++] = (Row){ sort_labels[sort], NULL };
		for (int i=0; i<count; i++) rows[n++] = (Row){ NULL, all[i] };
	}
	free(all);
	return n;
}
static int firstAchievement(Row* rows, int count) {
	for (int i=0; i<count; i++) if (!rows[i].header) return i;
	return 0;
}

void RA_showAchievements(SDL_Surface* screen, RA_SleepCallback before_sleep, RA_SleepCallback after_sleep) {
	if (!RA_hasAchievements()) return;
	rc_client_achievement_list_t* list = rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	if (!list) return;
	
	int capacity = 2; // the most headers a non-default sort adds
	for (uint32_t b=0; b<list->num_buckets; b++) capacity += 1 + list->buckets[b].num_achievements;
	Row* rows = calloc(capacity, sizeof(Row));
	int sort = getInt(SORT_PATH);
	if (sort<0 || sort>=SORT_COUNT) sort = SORT_DEFAULT;
	int count = buildRows(list, sort, rows);
	uint32_t flash_until = 0; // the new order's name, shown briefly after X
	
	rc_client_user_game_summary_t summary;
	rc_client_get_user_game_summary(client, &summary);
	char summary_text[96];
	snprintf(summary_text, sizeof(summary_text), "%u of %u unlocked  \xC2\xB7  %u of %u points",
		summary.num_unlocked_achievements, summary.num_promoted_achievements, summary.points_unlocked, summary.points_available);
	
	SDL_Surface** badges = calloc(capacity, sizeof(SDL_Surface*)); // loaded the first time each row is drawn
	char* badge_tried = calloc(capacity, 1);

	int selected = firstAchievement(rows, count); // row 0 is a header
	int start = 0;
	int dirty = 1;
	int show_setting = 0;
	PAD_reset();
	while (1) {
		GFX_startFrame();
		PAD_poll();
		
		if (PAD_justPressed(BTN_B) || PAD_justPressed(BTN_MENU)) {
			SFX_play(SFX_BACK);
			break;
		}
		if (PAD_justPressed(BTN_X)) {
			SFX_play(SFX_TICK);
			sort = (sort + 1) % SORT_COUNT;
			putInt(SORT_PATH, sort);
			// badges belong to rows, which just moved
			for (int i=0; i<capacity; i++) {
				if (badges[i]) SDL_FreeSurface(badges[i]);
				badges[i] = NULL;
				badge_tried[i] = 0;
			}
			count = buildRows(list, sort, rows);
			selected = firstAchievement(rows, count);
			start = 0;
			flash_until = SDL_GetTicks() + 1000;
			dirty = 1;
		}
		if (flash_until && SDL_GetTicks()>=flash_until) {
			flash_until = 0;
			dirty = 1;
		}
		int step = 0;
		if (PAD_justRepeated(BTN_UP)) step = -1;
		else if (PAD_justRepeated(BTN_DOWN)) step = 1;
		else if (PAD_justRepeated(BTN_LEFT)) step = -LIST_ROWS;
		else if (PAD_justRepeated(BTN_RIGHT)) step = LIST_ROWS;
		if (step) {
			int next = selected + step;
			if (next<0) next = 0;
			if (next>=count) next = count-1;
			// headers aren't selectable, keep moving the same way (or back if at the end)
			while (next>=0 && next<count && rows[next].header) next += step>0 ? 1 : -1;
			if (next<0 || next>=count) next = selected;
			if (next!=selected) {
				selected = next;
				dirty = 1;
				SFX_play(SFX_TICK);
			}
		}
		if (selected<start+1 && selected>0 && rows[selected-1].header) start = selected-1; // show the section label with its first item
		if (selected<start) start = selected;
		if (selected>=start+LIST_ROWS) start = selected-LIST_ROWS+1;
		
		PWR_update(&dirty, &show_setting, before_sleep, after_sleep);
		
		if (dirty) {
			GFX_clear(screen);
			int ow = GFX_blitHardwareGroup(screen, show_setting);
			
			SDL_Surface* text = TTF_RenderUTF8_Blended(font.small, summary_text, COLOR_WHITE);
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ SCALE1(PADDING+BUTTON_PADDING), SCALE1(PADDING) + (SCALE1(PILL_SIZE)-text->h)/2 });
			SDL_FreeSurface(text);
			
			int width = screen->w - SCALE1(PADDING*2);
			for (int i=start, j=0; i<count && j<LIST_ROWS; i++, j++) {
				int y = SCALE1(PADDING + (j+1)*PILL_SIZE);
				Row* row = &rows[i];
				if (row->header) {
					text = TTF_RenderUTF8_Blended(font.small, row->header, COLOR_DARK_TEXT);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ SCALE1(PADDING+BUTTON_PADDING), y + (SCALE1(PILL_SIZE)-text->h)/2 });
					SDL_FreeSurface(text);
					continue;
				}
				const rc_client_achievement_t* a = row->achievement;
				int is_selected = i==selected;
				if (is_selected) GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){ SCALE1(PADDING), y, width, SCALE1(PILL_SIZE) });
				SDL_Color color = is_selected ? COLOR_BLACK : (a->unlocked ? COLOR_WHITE : COLOR_DARK_TEXT);
				
				if (!badge_tried[i]) {
					badge_tried[i] = 1;
					badges[i] = loadBadge(a->badge_name);
					if (badges[i] && !a->unlocked) SDL_SetSurfaceColorMod(badges[i], 90, 90, 90); // locked ones are dimmed
				}
				int badge_size = SCALE1(PILL_SIZE - 6);
				int text_x = SCALE1(PADDING+BUTTON_PADDING);
				if (badges[i]) {
					SDL_BlitScaled(badges[i], NULL, screen, &(SDL_Rect){ SCALE1(PADDING+BUTTON_PADDING)/2 + SCALE1(PADDING)/2, y + SCALE1(3), badge_size, badge_size });
					text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING)/2 + badge_size + SCALE1(BUTTON_PADDING)/2;
				}
				
				char points[16];
				snprintf(points, sizeof(points), "%u", a->points);
				SDL_Surface* points_txt = TTF_RenderUTF8_Blended(font.small, points, color);
				char title[160];
				GFX_truncateText(font.large, (char*)a->title, title, width - (text_x - SCALE1(PADDING)) - points_txt->w - SCALE1(BUTTON_PADDING*2), 0);
				text = TTF_RenderUTF8_Blended(font.large, title, color);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ text_x, y + SCALE1(4) });
				SDL_FreeSurface(text);
				SDL_BlitSurface(points_txt, NULL, screen, &(SDL_Rect){ SCALE1(PADDING) + width - SCALE1(BUTTON_PADDING) - points_txt->w, y + (SCALE1(PILL_SIZE)-points_txt->h)/2 });
				SDL_FreeSurface(points_txt);
			}
			
			// description of the highlighted achievement, two lines at most, with progress when it tracks any
			const rc_client_achievement_t* a = rows[selected].achievement;
			if (a) {
				char description[512+3]; // room for wrapping's "..."
				if (a->measured_progress[0] && !a->unlocked) snprintf(description, sizeof(description)-3, "%s (%s)", a->description, a->measured_progress);
				else snprintf(description, sizeof(description)-3, "%s", a->description);
				GFX_wrapText(font.small, description, width - SCALE1(BUTTON_PADDING*2), 2);
				int y = SCALE1(PADDING + (LIST_ROWS+1)*PILL_SIZE + 2);
				char* line = strtok(description, "\n");
				while (line) {
					text = TTF_RenderUTF8_Blended(font.small, line, COLOR_WHITE);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ SCALE1(PADDING+BUTTON_PADDING), y });
					y += text->h;
					SDL_FreeSurface(text);
					line = strtok(NULL, "\n");
				}
			}
			
			if (flash_until) {
				// the new order's name in a box over the list, like the launcher's sort popup
				text = TTF_RenderUTF8_Blended(font.large, sort_labels[sort], COLOR_WHITE);
				int pw = text->w + SCALE1(BUTTON_PADDING*4);
				int ph = text->h + SCALE1(BUTTON_PADDING*2);
				SDL_Rect box = { (screen->w-pw)/2, (screen->h-ph)/2, pw, ph };
				GFX_blitRect(ASSET_DARK_GRAY_PILL, screen, &box);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ box.x + (pw-text->w)/2, box.y + (ph-text->h)/2 });
				SDL_FreeSurface(text);
			}

			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			else GFX_blitButtonGroup((char*[]){ "X","SORT", NULL }, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){ "B","BACK", NULL }, 0, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}
	
	for (int i=0; i<capacity; i++) if (badges[i]) SDL_FreeSurface(badges[i]);
	free(badges);
	free(badge_tried);
	free(rows);
	rc_client_destroy_achievement_list(list);
	PAD_reset();
}
