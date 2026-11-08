// Makes DraStic (a closed standalone program, not ours and not in releases) act like MinUI+'s
// other systems: minarch's screen effects and game menu, save slots, play time, achievements and
// remappable controls.
// launch.sh loads this with LD_PRELOAD. It points DraStic's calls to SDL and ALSA at the functions
// here by rewriting DraStic's own relocation slots, so this library and the platform code it
// shares with minarch still reach the real SDL.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <pthread.h>

#include <msettings.h>
#include <SDL2/SDL_image.h>

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "bgm.h"
#include "ra.h"
#include "libretro.h"

#define DEVICE_WIDTH FIXED_WIDTH
#define DEVICE_HEIGHT FIXED_HEIGHT

extern SDL_Window* PLAT_getWindow(void);
extern int PLAT_getRotate(void);
extern SDL_Texture* PLAT_getEffectTexture(int scale);
extern void PLAT_drawNotification(void);

///////////////////////////////
// timing, in the log when NDSLAYER_PROFILE is set

static int profile_on;
static uint64_t profile_upload, profile_draw, profile_swap, profile_poll, profile_audio;
static uint64_t profile_started, profile_last_frame, profile_worst_gap;
static int profile_frames, profile_polls;

static uint64_t nowUs(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec*1000000 + now.tv_nsec/1000;
}
static void profileReset(void) {
	profile_upload = profile_draw = profile_swap = profile_poll = profile_audio = 0;
	profile_frames = profile_polls = 0;
	profile_worst_gap = 0;
	profile_started = profile_last_frame = nowUs();
}
static void profileFrame(void) {
	uint64_t now = nowUs();
	if (now-profile_last_frame>profile_worst_gap) profile_worst_gap = now-profile_last_frame;
	profile_last_frame = now;
	if (++profile_frames<300) return;
	double per_frame = 1000.0 * profile_frames;
	fprintf(stderr, "profile: %.1f fps, polls %.1f/s, worst gap %.1f ms, ms a frame: all %.1f upload %.2f draw %.2f swap %.2f poll %.2f audio %.2f\n",
		profile_frames * 1000000.0 / (now-profile_started), profile_polls * 1000000.0 / (now-profile_started), profile_worst_gap/1000.0, (now-profile_started)/per_frame,
		profile_upload/per_frame, profile_draw/per_frame, profile_swap/per_frame, profile_poll/per_frame, profile_audio/per_frame);
	profileReset();
}

///////////////////////////////
// DraStic's clock stops while it's paused: it paces itself by it, and would spend the rest of the
// session skipping frames to catch up on time spent asleep or in the menu

static uint64_t paused_total, pause_started;
static int pause_depth;
static uint64_t fps_started; // 0 to start counting over

static void pauseBegin(void) {
	if (pause_depth++==0) pause_started = nowUs();
}
static void pauseEnd(void) {
	if (--pause_depth>0) return;
	paused_total += nowUs()-pause_started;
	fps_started = 0;
	profileReset();
}
static int h_gettimeofday(struct timeval* tv, void* tz) {
	int result = gettimeofday(tv, tz);
	uint64_t now = (uint64_t)tv->tv_sec*1000000 + tv->tv_usec - paused_total;
	tv->tv_sec = now/1000000;
	tv->tv_usec = now%1000000;
	return result;
}

///////////////////////////////
// DraStic's threads signal each other without holding the lock, so with several cores running a wakeup can be missed
// and two threads then wait on each other forever. Every wait of its is a loop on a flag, so waking now and then
// to look at the flag again is harmless, and gets it going again

static int h_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
	struct timespec until;
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_nsec += 20000000;
	if (until.tv_nsec>=1000000000) {
		until.tv_sec += 1;
		until.tv_nsec -= 1000000000;
	}
	int result = pthread_cond_timedwait(cond, mutex, &until);
	return result==ETIMEDOUT ? 0 : result;
}

///////////////////////////////
// DraStic itself, found in its symbol table rather than at fixed addresses

static void* nds_system;
static int32_t (*nds_save)(void*,uint32_t,uint16_t*,uint16_t*);
static int32_t (*nds_load)(void*,uint32_t,uint16_t*,uint16_t*,uint32_t);
static void (*nds_copy_screen)(void*,int);
static void (*nds_quit)(void*);

#define MENU_SLOT_COUNT 8
#define SCREEN_BYTES (256*192*2)

static char rom_path[512]; // as the launcher passed it
static char rom_file[256]; // with its extension, as minarch names what it keeps per game
static char game_name[256]; // without, as DraStic names its states
static char minui_dir[512]; // where the launcher looks for slots and pictures
static char states_dir[512];

static int canSave(void) {
	return nds_system && nds_save && nds_load && nds_copy_screen;
}
static int slotPath(int slot, char* path, size_t size) {
	return snprintf(path, size, "%s/%s_%i.dss", states_dir, game_name, slot);
}
static int slotExists(int slot) {
	char path[800];
	slotPath(slot, path, sizeof(path));
	return access(path, F_OK)==0;
}
static void saveSlot(int slot) {
	static uint16_t top[SCREEN_BYTES/2], bottom[SCREEN_BYTES/2]; // the snapshot in the state
	nds_copy_screen(top, 0);
	nds_copy_screen(bottom, 1);
	int result = nds_save(nds_system, slot, top, bottom);
	if (result) fprintf(stderr, "layer: saving slot %i returned %i\n", slot, result);
}
static void loadSlot(int slot) {
	nds_load(nds_system, slot, NULL, NULL, 0);
}

///////////////////////////////
// controls

typedef struct Action {
	const char* name; // shown in the menu
	const char* index; // in DraStic's config, CONTROL_INDEX_<index>
	int btn; // BTN_ID_* it's on, BTN_ID_NONE for none
	int sym; // the key DraStic expects, 0 when its config leaves it unbound
	int down;
	int mod; // needs MENU held too
	int default_btn;
	int default_mod;
} Action;

static Action actions[] = {
	{"Up",           "UP",                 BTN_ID_DPAD_UP},
	{"Down",         "DOWN",               BTN_ID_DPAD_DOWN},
	{"Left",         "LEFT",               BTN_ID_DPAD_LEFT},
	{"Right",        "RIGHT",              BTN_ID_DPAD_RIGHT},
	{"A",            "A",                  BTN_ID_A},
	{"B",            "B",                  BTN_ID_B},
	{"X",            "X",                  BTN_ID_X},
	{"Y",            "Y",                  BTN_ID_Y},
	{"L",            "L",                  BTN_ID_L1},
	{"R",            "R",                  BTN_ID_R1},
	{"Start",        "START",              BTN_ID_START},
	{"Select",       "SELECT",             BTN_ID_SELECT},
	{"Stylus Up",    "TOUCH_CURSOR_UP",    BTN_ID_ANALOG_UP},
	{"Stylus Down",  "TOUCH_CURSOR_DOWN",  BTN_ID_ANALOG_DOWN},
	{"Stylus Left",  "TOUCH_CURSOR_LEFT",  BTN_ID_ANALOG_LEFT},
	{"Stylus Right", "TOUCH_CURSOR_RIGHT", BTN_ID_ANALOG_RIGHT},
	{"Stylus Press", "TOUCH_CURSOR_PRESS", BTN_ID_R2},
	{"Next Layout",  NULL,                 BTN_ID_R2, .mod=1},
	{"Previous Layout", NULL,              BTN_ID_NONE},
	{"Next Screen",  NULL,                 BTN_ID_NONE},
	{"Previous Screen", NULL,              BTN_ID_NONE},
	{"Swap Screens", NULL,                 BTN_ID_L2},
	{"Fast Forward", "FAST_FORWARD",       BTN_ID_NONE},
	{"Save State",   NULL,                 BTN_ID_NONE},
	{"Load State",   NULL,                 BTN_ID_NONE},
	{"Game Switcher",NULL,                 BTN_ID_NONE},
};
#define ACTION_COUNT (int)(sizeof(actions)/sizeof(actions[0]))
#define ACTION_STYLUS_UP 12
#define ACTION_STYLUS_PRESS 16
#define ACTION_NEXT_LAYOUT 17 // from here on are shortcuts, and those without an index are ours: DraStic never hears about them
#define ACTION_PREV_LAYOUT 18
#define ACTION_NEXT_SCREEN 19
#define ACTION_PREV_SCREEN 20
#define ACTION_SWAP 21
#define ACTION_SAVE 23
#define ACTION_LOAD 24
#define ACTION_SWITCHER 25

static const struct { int id; const char* name; } buttons[] = {
	{BTN_ID_NONE, "NONE"},
	{BTN_ID_DPAD_UP, "UP"}, {BTN_ID_DPAD_DOWN, "DOWN"}, {BTN_ID_DPAD_LEFT, "LEFT"}, {BTN_ID_DPAD_RIGHT, "RIGHT"},
	{BTN_ID_A, "A"}, {BTN_ID_B, "B"}, {BTN_ID_X, "X"}, {BTN_ID_Y, "Y"},
	{BTN_ID_START, "START"}, {BTN_ID_SELECT, "SELECT"},
	{BTN_ID_L1, "L1"}, {BTN_ID_R1, "R1"}, {BTN_ID_L2, "L2"}, {BTN_ID_R2, "R2"}, {BTN_ID_L3, "L3"},
	{BTN_ID_ANALOG_UP, "STICK UP"}, {BTN_ID_ANALOG_DOWN, "STICK DOWN"}, {BTN_ID_ANALOG_LEFT, "STICK LEFT"}, {BTN_ID_ANALOG_RIGHT, "STICK RIGHT"},
};
#define BUTTON_COUNT (int)(sizeof(buttons)/sizeof(buttons[0]))

static int buttonIndex(int id) {
	for (int i=0; i<BUTTON_COUNT; i++) if (buttons[i].id==id) return i;
	return 0;
}

static int drawn_bottom = 1; // DraStic draws the bottom screen first when its own swap setting is on

static void readDrasticControls(void) { // what key DraStic wants for each action
	char path[512];
	snprintf(path, sizeof(path), "%s/config/drastic.cfg", getenv("HOME"));
	FILE* file = fopen(path, "r");
	if (!file) return;
	char line[256];
	while (fgets(line, sizeof(line), file)) {
		char index[64];
		int value;
		if (sscanf(line, "screen_swap = %i", &value)==1) drawn_bottom = value ? 0 : 1;
		if (sscanf(line, "controls_a[CONTROL_INDEX_%63[^]]] = %i", index, &value)!=2) continue;
		if (value>=65535) continue; // unbound
		for (int i=0; i<ACTION_COUNT; i++) {
			if (!actions[i].index || strcmp(actions[i].index, index)) continue;
			if (i>=ACTION_STYLUS_UP && i<=ACTION_STYLUS_PRESS) continue; // the layer's own, see updateStylus
			actions[i].sym = value<256 ? value : (value-256)|SDLK_SCANCODE_MASK;
		}
	}
	fclose(file);
}

///////////////////////////////
// our own settings

// DraStic's own settings: it reads them from its config as it starts, so that's where they go
static char* off_on[] = {"Off","On",NULL};
static char* skip_types[] = {"Off","Manual","Automatic",NULL};
static char* skip_values[] = {"0","1","2","3","4","5","6","7","8","9",NULL};
static char* languages[] = {"Japanese","English","French","German","Italian","Spanish",NULL};
typedef struct EmuOption {
	const char* name;
	const char* desc;
	const char* key; // in DraStic's config
	char** labels;
	int default_value;
	int value;
} EmuOption;
static EmuOption emu_options[] = {
	{"Frameskip", "Skipping frames keeps the game at full speed.", "frameskip_type", skip_types, 2},
	{"Frames To Skip", "How many in a row, or at most when Automatic.", "frameskip_value", skip_values, 4},
	{"Threaded 3D", "Draws 3D on another core. Much faster.", "threaded_3d", off_on, 1},
	{"Fix Main 2D Screen", "For games with a broken 2D screen.", "fix_main_2d_screen", off_on, 0},
	{"Disable Edge Marking", "Removes outlines from 3D models. A bit faster.", "disable_edge_marking", off_on, 0},
	{"Cheats", "Uses DraStic's cheats folder.", "enable_cheats", off_on, 1},
	{"Sound", NULL, "enable_sound", off_on, 1},
	{"FPS Counter", "Frames drawn each second, in the corner.\nShows right away.", "show_frame_counter", off_on, 0}, // see EMU_FPS_COUNTER
	{"Language", "The language games see in the DS's settings.", "firmware.language", languages, 1},
};
#define EMU_OPTION_COUNT (int)(sizeof(emu_options)/sizeof(emu_options[0]))
#define EMU_FPS_COUNTER 7 // drawFPSCounter shows this, right away: DraStic's own, which only reads this as it starts, stays off so there aren't two

// ours, for the console or for one game, as minarch keeps its own
static int effect_type = EFFECT_NONE;
static int layout_index = 9; // Big + Corner: a DS game needs both screens
static int border_style = 1; // 0 for none, else its folder in DraStic's resources
static int swap_screens = 0; // the bottom screen is the main one
static int prevent_tearing = 1;
static int stick_dpad = 0; // the thumbstick is a second d-pad, not the stylus
// the game menu in minui writes these too. Normal by default: the A30 changes speed by writing the PLL at the voltage it
// booted with, and 1512MHz is past what that holds for long (see minarch's Auto_maxLevel)
enum { CPU_POWERSAVE, CPU_NORMAL, CPU_PERFORMANCE };
static int cpu_speed = CPU_NORMAL;

enum { SETTINGS_NONE, SETTINGS_CONSOLE, SETTINGS_GAME };
static int settings_loaded = SETTINGS_NONE;

static const char* actionKey(int i) {
	if (actions[i].index) return actions[i].index;
	switch (i) {
		case ACTION_NEXT_LAYOUT: return "LAYER_NEXT_LAYOUT";
		case ACTION_PREV_LAYOUT: return "LAYER_PREV_LAYOUT";
		case ACTION_NEXT_SCREEN: return "LAYER_NEXT_SCREEN";
		case ACTION_PREV_SCREEN: return "LAYER_PREV_SCREEN";
		case ACTION_SWAP: return "LAYER_SWAP";
		case ACTION_SAVE: return "LAYER_SAVE";
		case ACTION_LOAD: return "LAYER_LOAD";
	}
	return "LAYER_SWITCHER";
}
static const char* consoleSettingsPath(void) {
	const char* path = getenv("NDSLAYER_CFG");
	return path ? path : "/tmp/ndslayer.cfg";
}
static void gameSettingsPath(char* path, size_t size) { // <console>/<game>.cfg, without its .cfg on the folder
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", consoleSettingsPath());
	char* dot = strrchr(dir, '.');
	if (dot) *dot = '\0';
	mkdir(dir, 0755);
	snprintf(path, size, "%s/%s.cfg", dir, game_name);
}
static int gameCPUSpeed(void) { return CPU_SPEED_POWERSAVE + cpu_speed; }
static void resetSettings(void) {
	effect_type = EFFECT_NONE;
	layout_index = 9;
	border_style = 1;
	swap_screens = 0;
	prevent_tearing = 1;
	stick_dpad = 0;
	cpu_speed = CPU_NORMAL;
	for (int i=0; i<ACTION_COUNT; i++) {
		actions[i].btn = actions[i].default_btn;
		actions[i].mod = actions[i].default_mod;
	}
	for (int i=0; i<EMU_OPTION_COUNT; i++) emu_options[i].value = emu_options[i].default_value;
}
static int readSettings(const char* path) {
	FILE* file = fopen(path, "r");
	if (!file) return 0;
	char line[128];
	while (fgets(line, sizeof(line), file)) {
		char key[64];
		int value, mod = 0;
		if (sscanf(line, "effect = %i", &value)==1) effect_type = value;
		else if (sscanf(line, "layout = %i", &value)==1) layout_index = value;
		else if (sscanf(line, "border = %i", &value)==1) border_style = value;
		else if (sscanf(line, "swap = %i", &value)==1) swap_screens = value;
		else if (sscanf(line, "tearing = %i", &value)==1) prevent_tearing = value;
		else if (sscanf(line, "stick = %i", &value)==1) stick_dpad = value;
		else if (sscanf(line, "cpu_speed = %i", &value)==1) cpu_speed = MAX(0, MIN(value, CPU_PERFORMANCE));
		else if (sscanf(line, "emu %63s = %i", key, &value)==2) {
			for (int i=0; i<EMU_OPTION_COUNT; i++) {
				if (!strcmp(emu_options[i].key, key)) emu_options[i].value = value;
			}
		}
		else if (sscanf(line, "bind %63[^=] = %i %i", key, &value, &mod)>=2) {
			key[strcspn(key, " ")] = '\0';
			for (int i=0; i<ACTION_COUNT; i++) {
				if (strcmp(actionKey(i), key)) continue;
				actions[i].btn = value;
				actions[i].mod = mod;
			}
		}
	}
	fclose(file);
	return 1;
}
static void clampSettings(void);
static void loadSettings(void) {
	char game[800];
	gameSettingsPath(game, sizeof(game));
	resetSettings();
	settings_loaded = SETTINGS_NONE;
	if (readSettings(game)) settings_loaded = SETTINGS_GAME;
	else if (readSettings(consoleSettingsPath())) settings_loaded = SETTINGS_CONSOLE;
	clampSettings();
}
static void writeSettings(int scope, int only) { // only: this scope is what applies from now on
	char path[800];
	if (scope==SETTINGS_GAME) gameSettingsPath(path, sizeof(path));
	else snprintf(path, sizeof(path), "%s", consoleSettingsPath());
	if (only && scope==SETTINGS_CONSOLE) { // saving for the console replaces this game's own
		char game[800];
		gameSettingsPath(game, sizeof(game));
		unlink(game);
	}
	char tmp[900];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* file = fopen(tmp, "w");
	if (!file) return;
	fprintf(file, "effect = %i\nlayout = %i\nborder = %i\nswap = %i\ntearing = %i\nstick = %i\ncpu_speed = %i\n", effect_type, layout_index, border_style, swap_screens, prevent_tearing, stick_dpad, cpu_speed);
	for (int i=0; i<EMU_OPTION_COUNT; i++) fprintf(file, "emu %s = %i\n", emu_options[i].key, emu_options[i].value);
	for (int i=0; i<ACTION_COUNT; i++) fprintf(file, "bind %s = %i %i\n", actionKey(i), actions[i].btn, actions[i].mod);
	fflush(file);
	fsync(fileno(file));
	fclose(file);
	rename(tmp, path);
	settings_loaded = scope;
}
static void saveSettings(void) { // what's changed, to whichever it's using (a layout is too easy to change by hand to ask)
	writeSettings(settings_loaded==SETTINGS_GAME ? SETTINGS_GAME : SETTINGS_CONSOLE, 0);
}
static void screenSettingsChanged(void);
static void restoreSettings(void) {
	char game[800];
	gameSettingsPath(game, sizeof(game));
	if (settings_loaded==SETTINGS_GAME) unlink(game);
	else unlink(consoleSettingsPath());
	loadSettings();
	screenSettingsChanged();
}
static void applyEmuOptions(void) { // into DraStic's config, before it reads it
	char path[512], tmp[600];
	snprintf(path, sizeof(path), "%s/config/drastic.cfg", getenv("HOME"));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* in = fopen(path, "r");
	if (!in) return;
	FILE* out = fopen(tmp, "w");
	if (!out) {
		fclose(in);
		return;
	}
	char line[512];
	int found[EMU_OPTION_COUNT];
	memset(found, 0, sizeof(found));
	while (fgets(line, sizeof(line), in)) {
		int matched = 0;
		for (int i=0; i<EMU_OPTION_COUNT && !matched; i++) {
			size_t size = strlen(emu_options[i].key);
			if (strncmp(line, emu_options[i].key, size) || strncmp(line+size, " = ", 3)) continue;
			fprintf(out, "%s = %i\n", emu_options[i].key, i==EMU_FPS_COUNTER ? 0 : emu_options[i].value);
			found[i] = matched = 1;
		}
		if (!matched) fputs(line, out);
	}
	for (int i=0; i<EMU_OPTION_COUNT; i++) {
		if (!found[i]) fprintf(out, "%s = %i\n", emu_options[i].key, i==EMU_FPS_COUNTER ? 0 : emu_options[i].value);
	}
	fclose(in);
	fflush(out);
	fsync(fileno(out));
	fclose(out);
	rename(tmp, path);
}

///////////////////////////////
// play time, the same file and sums as every other system

static struct {
	char path[512];
	PlayTime saved; // as of launch, with this launch's session already counted
	int session_base;
	uint32_t session_ms;
	uint32_t unsaved_ms;
	uint32_t last_tick;
} play;

static void Play_init(void) {
	snprintf(play.path, sizeof(play.path), "%s", prefixMatch(SDCARD_PATH, rom_path) ? rom_path + strlen(SDCARD_PATH) : rom_path);
	getPlayTime(play.path, &play.saved);
	int now = time(NULL);
	int since = now - play.saved.last_played;
	int continuing = play.saved.sessions>0 && play.saved.last_played>0 && since>=0 && since<PLAYTIME_SESSION_GAP;
	if (continuing) play.session_base = play.saved.last_session;
	else {
		play.saved.sessions += 1;
		play.session_base = 0;
	}
	play.session_ms = 0;
	play.unsaved_ms = 0;
	play.last_tick = SDL_GetTicks();
}
static int Play_getTotal(void) {
	return play.saved.seconds + play.session_ms / 1000;
}
static int Play_getSession(void) {
	return play.session_base + play.session_ms / 1000;
}
static void Play_save(void) {
	if (!play.path[0]) return;
	PlayTime current = play.saved;
	current.seconds = Play_getTotal();
	current.last_played = time(NULL);
	current.last_session = Play_getSession();
	setPlayTime(play.path, &current);
	play.unsaved_ms = 0;
}
static void Play_tick(void) {
	uint32_t now = SDL_GetTicks();
	uint32_t delta = now - play.last_tick;
	play.last_tick = now;
	if (delta<500) { // longer gaps were menus, sleep, or loading
		play.session_ms += delta;
		play.unsaved_ms += delta;
	}
	if (play.unsaved_ms>=60000) Play_save(); // so a dead battery loses at most a minute
}

///////////////////////////////
// test scripting, only in a build for the harness

#ifdef NDSLAYER_TEST
static int test_polls;
static void testPad(void) {
	test_polls++;
	const char* script = getenv("NDSLAYER_PRESS"); // poll:button id,...
	for (const char* s=script; s && *s;) {
		int at, id;
		if (sscanf(s, "%i:%i", &at, &id)!=2) break;
		int held = id>=100 ? 60 : 1; // 100 and up: the button id+100, held down for a while
		id %= 100;
		if (at==test_polls) {
			pad.is_pressed |= 1<<id;
			pad.just_pressed |= 1<<id;
			pad.just_repeated |= 1<<id;
		}
		else if (at<test_polls && test_polls<at+held) pad.is_pressed |= 1<<id;
		else if (at+held==test_polls) {
			pad.is_pressed &= ~(1<<id);
			pad.just_repeated &= ~(1<<id);
			pad.just_released |= 1<<id;
		}
		s = strchr(s, ',');
		if (s) s++;
	}
}
static void testShot(const char* tag, void* pixels, int w, int h, int pitch) {
	static int count;
	char path[128];
	snprintf(path, sizeof(path), "/s/nds/layer_%s_%04i.ppm", tag, count++);
	FILE* file = fopen(path, "wb");
	if (!file) return;
	fprintf(file, "P6 %i %i 255\n", w, h);
	for (int y=0; y<h; y++) {
		uint16_t* row = (uint16_t*)((char*)pixels + y*pitch);
		for (int x=0; x<w; x++) {
			unsigned char rgb[3] = {(row[x]>>11)<<3, ((row[x]>>5)&63)<<2, (row[x]&31)<<3};
			fwrite(rgb, 1, 3, file);
		}
	}
	fclose(file);
}
#else
#define testPad()
#endif

///////////////////////////////
// screen layouts, in landscape coordinates: "main" is the screen Swap Screens puts first

typedef struct Layout {
	const char* name;
	const char* border; // bg_<border>.png
	SDL_Rect main, other; // other.w==0 when only one screen shows
	int book; // both screens turned on their side, for games held like a book
} Layout;

static const Layout layouts[] = {
	{"Side by Side",       "h0",   {64,144,256,192},  {320,144,256,192}},
	{"Side by Side Large", "h1",   {0,120,320,240},   {320,120,320,240}},
	{"Book",               "hh0",  {0,27,320,427},    {320,27,320,427}, 1},
	{"Single",             "s0",   {64,48,512,384},   {0}},
	{"Single Full",        NULL,   {0,0,640,480},     {0}},
	{"Stacked",            "v0",   {192,47,256,192},  {192,240,256,192}},
	{"Stacked Large",      "v1",   {160,0,320,240},   {160,240,320,240}},
	{"Big Over Small",     "vh_c0",{128,0,384,288},   {192,288,256,192}},
	{"Big Beside Small",   "vh_c1",{0,96,384,288},    {384,144,256,192}},
	{"Big + Corner",       "vh_s1",{0,0,384,288},     {384,288,256,192}},
	{"Bigger + Corner",    "vh_s0",{0,0,480,360},     {480,360,160,120}},
	{"Bigger + Below",     "vh_s2",{80,0,480,360},    {240,360,160,120}},
};
#define LAYOUT_COUNT (int)(sizeof(layouts)/sizeof(layouts[0]))

static SDL_Rect screenRect(const Layout* layout, int bottom) { // where a DS screen goes
	return bottom==swap_screens ? layout->main : layout->other;
}

static SDL_Renderer* renderer;
static SDL_Texture* border_texture;
static SDL_Surface* border_flat; // the same, for the menu's picture of the game
static int border_dirty = 1;

static void loadBorder(void) {
	border_dirty = 0;
	if (border_texture) SDL_DestroyTexture(border_texture);
	border_texture = NULL;
	if (border_flat) SDL_FreeSurface(border_flat);
	border_flat = NULL;
	const char* name = layouts[layout_index].border;
	if (!border_style || !name) return;
	char path[600];
	snprintf(path, sizeof(path), "%s/resources/bg_640/%i/bg_%s.png", getenv("HOME"), border_style, name);
	SDL_Surface* image = IMG_Load(path);
	if (!image) return; // DraStic builds without the art still play, just on black
	// flattened on black so it's drawn without blending: the screens and lines go over its holes
	SDL_Surface* flat = SDL_CreateRGBSurface(SDL_SWSURFACE, FIXED_WIDTH, FIXED_HEIGHT, FIXED_DEPTH, RGBA_MASK_565);
	if (flat) {
		SDL_FillRect(flat, NULL, 0);
		SDL_BlitSurface(image, NULL, flat, NULL);
		border_texture = SDL_CreateTextureFromSurface(renderer, flat);
		border_flat = flat;
	}
	SDL_FreeSurface(image);
	if (border_texture) SDL_SetTextureBlendMode(border_texture, SDL_BLENDMODE_NONE);
}

///////////////////////////////
// the game menu: minarch's, drawn the same way

static SDL_Surface* screen;
static int in_menu;


static void screenSettingsChanged(void) { border_dirty = 1; }
static void setCoreOnline(int core, int online) {
	char path[64];
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%i/online", core);
	FILE* file = fopen(path, "w");
	if (!file) return;
	fputs(online ? "1" : "0", file);
	fclose(file);
}

static void composeShot(SDL_Surface* dst) { // the game as it looks, without the screen effect
	if (border_dirty) loadBorder();
	SDL_FillRect(dst, NULL, 0);
	if (border_flat) SDL_BlitSurface(border_flat, NULL, dst, NULL);
	if (!nds_copy_screen) return;
	const Layout* layout = &layouts[layout_index];
	static uint16_t pixels[SCREEN_BYTES/2];
	for (int s=0; s<2; s++) {
		SDL_Rect rect = screenRect(layout, s);
		if (!rect.w) continue;
		nds_copy_screen(pixels, s);
		for (int y=0; y<rect.h; y++) {
			if (rect.y+y<0 || rect.y+y>=dst->h) continue;
			uint16_t* out = (uint16_t*)((char*)dst->pixels + (rect.y+y)*dst->pitch) + rect.x;
			if (layout->book) { // turned on its side: across the panel is down the DS screen
				int sx = (rect.h-1-y)*256/rect.h;
				for (int x=0; x<rect.w; x++) out[x] = pixels[(x*192/rect.w)*256 + sx];
			}
			else {
				uint16_t* in = pixels + (y*192/rect.h)*256;
				for (int x=0; x<rect.w; x++) out[x] = in[x*256/rect.w];
			}
		}
	}
}

static void saveShot(SDL_Surface* shot, const char* path) {
	if (shot->w!=DEVICE_WIDTH || shot->h!=DEVICE_HEIGHT || shot->format->BitsPerPixel!=16) {
		SDL_SaveBMP(shot, path);
		return;
	}
	int w = shot->w / 2, h = shot->h / 2;
	uint16_t* half = malloc(w * h * sizeof(uint16_t));
	if (!half) return;
	for (int y=0; y<h; y++) {
		uint16_t* a = (uint16_t*)((uint8_t*)shot->pixels + (y*2) * shot->pitch);
		uint16_t* b = (uint16_t*)((uint8_t*)shot->pixels + (y*2+1) * shot->pitch);
		uint16_t* out = half + y * w;
		for (int x=0; x<w; x++) { // the average of each 2x2, so grid lines blend instead of flickering in and out
			uint32_t p0 = a[x*2], p1 = a[x*2+1], p2 = b[x*2], p3 = b[x*2+1];
			uint32_t r = ((p0>>11) + (p1>>11) + (p2>>11) + (p3>>11) + 2) >> 2;
			uint32_t g = (((p0>>5)&0x3F) + ((p1>>5)&0x3F) + ((p2>>5)&0x3F) + ((p3>>5)&0x3F) + 2) >> 2;
			uint32_t bl = ((p0&0x1F) + (p1&0x1F) + (p2&0x1F) + (p3&0x1F) + 2) >> 2;
			out[x] = (r << 11) | (g << 5) | bl;
		}
	}
	FILE* file = fopen(path, "wb");
	if (file) {
		uint32_t size = w * h * 2;
		uint8_t header[66] = { 'B','M' };
		#define PUT32(at, v) do { uint32_t _v = (v); header[at] = _v; header[at+1] = _v>>8; header[at+2] = _v>>16; header[at+3] = _v>>24; } while (0)
		PUT32(2, sizeof(header) + size); // file size
		PUT32(10, sizeof(header)); // where the pixels start
		PUT32(14, 40); // BITMAPINFOHEADER
		PUT32(18, w);
		PUT32(22, -h); // top-down
		header[26] = 1; // planes
		header[28] = 16; // bits per pixel
		PUT32(30, 3); // BI_BITFIELDS
		PUT32(34, size);
		PUT32(54, 0xF800); PUT32(58, 0x07E0); PUT32(62, 0x001F); // RGB565
		#undef PUT32
		fwrite(header, 1, sizeof(header), file);
		fwrite(half, 1, size, file);
		fclose(file);
	}
	free(half);
}

static void quitGame(void) {
	if (nds_quit) nds_quit(nds_system);
}
static char* switcherPath(void) {
	return prefixMatch(SDCARD_PATH, rom_path) ? rom_path + strlen(SDCARD_PATH) : rom_path;
}
static void slotBmpPath(int slot, char* path, size_t size) {
	snprintf(path, size, "%s/%s.%d.bmp", minui_dir, rom_file, slot);
}
static SDL_Surface* shot_surface(void) {
	static SDL_Surface* shot;
	if (!shot) shot = SDL_CreateRGBSurface(SDL_SWSURFACE, FIXED_WIDTH, FIXED_HEIGHT, FIXED_DEPTH, RGBA_MASK_565);
	return shot;
}
static int menu_slot;
static void saveToSlot(int slot, SDL_Surface* shot) { // with its picture, and remembered for Resume
	if (!canSave()) return;
	char path[800];
	slotBmpPath(slot, path, sizeof(path));
	if (shot) saveShot(shot, path);
	if (slot!=AUTO_RESUME_SLOT) {
		snprintf(path, sizeof(path), "%s/%s.txt", minui_dir, rom_file);
		putInt(path, slot);
	}
	saveSlot(slot);
	sync();
}
static void loadFromSlot(int slot) {
	if (!canSave() || !slotExists(slot)) return;
	if (slot!=AUTO_RESUME_SLOT) {
		char path[800];
		snprintf(path, sizeof(path), "%s/%s.txt", minui_dir, rom_file);
		putInt(path, slot);
	}
	fprintf(stderr, "layer: loading slot %i\n", slot);
	loadSlot(slot);
	RA_stateLoaded();
}
static void beforeSleep(void) { // sleep can end in the power going off: comes back here when it does
	pauseBegin();
	Play_save();
	composeShot(shot_surface()); // the game switcher shows it, so it has to match the save
	saveToSlot(AUTO_RESUME_SLOT, shot_surface());
	putFile(AUTO_RESUME_PATH, switcherPath());
	PWR_setCPUSpeed(CPU_SPEED_MENU);
	sync();
}
static void afterSleep(void) {
	unlink(AUTO_RESUME_PATH);
	setCoreOnline(2, 1);
	setCoreOnline(3, 1);
	PWR_setCPUSpeed(in_menu ? CPU_SPEED_POWERSAVE : gameCPUSpeed());
	play.last_tick = SDL_GetTicks();
	pauseEnd();
}
static void saveForSwitcher(void) { // a quick save with a picture, then back to the launcher's game switcher
	composeShot(shot_surface());
	saveToSlot(AUTO_RESUME_SLOT, shot_surface());
	Switcher_add(switcherPath()); // so there's a way back
	putFile(SWITCHER_OPEN_PATH, "1");
	quitGame();
}

static void getAlias(char* path, char* alias) {
	// LOG_info("alias path: %s\n", path);
	char* tmp;
	char map_path[800];
	snprintf(map_path, sizeof(map_path), "%s", path);
	tmp = strrchr(map_path, '/');
	if (tmp) {
		tmp += 1;
		strcpy(tmp, "map.txt"); // (shorter than what it replaces, or the name was cut off and it won't be found)
		// LOG_info("map_path: %s\n", map_path);
	}
	char* file_name = strrchr(path,'/');
	if (file_name) file_name += 1;
	// LOG_info("file_name: %s\n", file_name);
	
	if (exists(map_path)) {
		FILE* file = fopen(map_path, "r");
		if (file) {
			char line[256];
			while (fgets(line,256,file)!=NULL) {
				normalizeNewline(line);
				trimTrailingNewlines(line);
				if (strlen(line)==0) continue; // skip empty lines
			
				tmp = strchr(line,'\t');
				if (tmp) {
					tmp[0] = '\0';
					char* key = line;
					char* value = tmp+1;
					if (exactMatch(file_name,key)) {
						strcpy(alias, value);
						break;
					}
				}
			}
			fclose(file);
		}
	}
}

#define OPTION_PADDING 8
#define MENU_ROW_SIZE 25
#define MENU_MESSAGE_FADE_MS 150
#define WINDOW_RADIUS 4
#define PAGINATION_HEIGHT 6
#define SCROLL_WIDTH 24
#define SCROLL_HEIGHT 4
#define STATS_ROWS 4
#define STATS_ROW_SIZE 22

static void Menu_blitPill(uint32_t color, SDL_Surface* dst, SDL_Rect* rect) {
	int h = rect->h;
	int r = h / 2;
	Uint8 cr,cg,cb;
	SDL_GetRGB(color, dst->format, &cr,&cg,&cb);
	SDL_Surface* ends = SDL_CreateRGBSurface(0, h,h, 32, 0x00FF0000,0x0000FF00,0x000000FF,0xFF000000);
	if (!ends) return;
	float c = h / 2.0f;
	for (int y=0; y<h; y++) {
		Uint32* row = (Uint32*)((Uint8*)ends->pixels + y * ends->pitch);
		for (int x=0; x<h; x++) {
			int inside = 0; // 4x4 samples per pixel for smooth edges
			for (int sy=0; sy<4; sy++) {
				for (int sx=0; sx<4; sx++) {
					float dx = x + (sx + 0.5f) / 4 - c;
					float dy = y + (sy + 0.5f) / 4 - c;
					if (dx*dx + dy*dy <= c*c) inside += 1;
				}
			}
			row[x] = SDL_MapRGBA(ends->format, cr,cg,cb, inside * 255 / 16);
		}
	}
	int w = rect->w > h ? rect->w - h : 0; // not MAX(): it has no outer parens
	SDL_BlitSurface(ends, &(SDL_Rect){0,0,r,h}, dst, &(SDL_Rect){rect->x,rect->y});
	if (w>0) SDL_FillRect(dst, &(SDL_Rect){rect->x+r,rect->y,w,h}, color);
	SDL_BlitSurface(ends, &(SDL_Rect){r,0,h-r,h}, dst, &(SDL_Rect){rect->x+r+w,rect->y});
	SDL_FreeSurface(ends);
}

static void pollMenu(int* dirty, int* show_setting) {
	GFX_startFrame();
	PAD_poll();
	testPad();
	if (dirty) PWR_update(dirty, show_setting, beforeSleep, afterSleep);
}
static void flipMenu(void) {
#ifdef NDSLAYER_TEST
	testShot("menu", screen->pixels, screen->w, screen->h, screen->pitch);
#endif
	GFX_flip(screen);
}

typedef struct MenuList MenuList;
typedef struct MenuItem MenuItem;
enum { MENU_CALLBACK_NOP, MENU_CALLBACK_EXIT, MENU_CALLBACK_NEXT_ITEM };
typedef int (*MenuList_callback_t)(MenuList* list, int i);
struct MenuItem {
	char* name;
	char* desc;
	char** values;
	int id;
	int value;
	MenuList_callback_t on_confirm;
	MenuList_callback_t on_change;
};
enum { MENU_LIST, MENU_VAR, MENU_FIXED, MENU_INPUT };
struct MenuList {
	int type;
	int max_width;
	char* title;
	char* desc;
	MenuItem* items;
	MenuList_callback_t on_confirm;
	MenuList_callback_t on_change;
};

static int Menu_message(char* message, char** pairs) {
	int dirty = 1;
	while (1) {
		pollMenu(&dirty, NULL);
		if (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B)) {
			SFX_play(SFX_BACK);
			break;
		}
		if (dirty) {
			GFX_clear(screen);
			GFX_blitMessage(font.medium, message, screen, &(SDL_Rect){0,SCALE1(PADDING),screen->w,screen->h-SCALE1(PILL_SIZE+PADDING)});
			GFX_blitButtonGroup(pairs, 0, screen, 1);
			flipMenu();
			dirty = 0;
		}
		else GFX_sync();
	}
	return MENU_CALLBACK_NOP;
}

static int Menu_options(MenuList* list) {
	MenuItem* items = list->items;
	int type = list->type;

	int dirty = 1;
	int show_options = 1;
	int show_settings = 0;
	int await_input = 0;

	int max_visible_options = (screen->h - ((SCALE1(PADDING + PILL_SIZE) * 2) + SCALE1(BUTTON_SIZE))) / SCALE1(BUTTON_SIZE);

	int count;
	for (count=0; items[count].name; count++);
	int selected = 0;
	int start = 0;
	int end = MIN(count,max_visible_options);
	int visible_rows = end;

	#define NEXT_ITEM() do { \
		selected += 1; \
		if (selected>=count) { selected = 0; start = 0; end = visible_rows; } \
		else if (selected>=end) { start += 1; end += 1; } \
	} while (0)

	int defer_menu = 0;
	while (show_options) {
		if (await_input) {
			defer_menu = 1;
			list->on_confirm(list, selected);
			NEXT_ITEM();
			dirty = 1;
			await_input = 0;
		}

		pollMenu(NULL, NULL);

		if (PAD_justRepeated(BTN_UP)) {
			selected -= 1;
			if (selected<0) {
				selected = count - 1;
				start = MAX(0,count - max_visible_options);
				end = count;
			}
			else if (selected<start) {
				start -= 1;
				end -= 1;
			}
			dirty = 1;
			SFX_play(SFX_TICK);
		}
		else if (PAD_justRepeated(BTN_DOWN)) {
			NEXT_ITEM();
			dirty = 1;
			SFX_play(SFX_TICK);
		}
		else if (type!=MENU_INPUT && items[selected].values) {
			MenuItem* item = &items[selected];
			int change = PAD_justRepeated(BTN_LEFT) ? -1 : PAD_justRepeated(BTN_RIGHT) ? 1 : 0;
			if (change) {
				int total;
				for (total=0; item->values[total]; total++);
				item->value = (item->value+change+total)%total;
				if (item->on_change) item->on_change(list, selected);
				else if (list->on_change) list->on_change(list, selected);
				dirty = 1;
				SFX_play(SFX_TICK);
			}
		}

		if (PAD_justPressed(BTN_B)) {
			show_options = 0;
			SFX_play(SFX_BACK);
		}
		else if (PAD_justPressed(BTN_A)) {
			MenuItem* item = &items[selected];
			SFX_play(SFX_CONFIRM);
			int result = MENU_CALLBACK_NOP;
			if (item->on_confirm) result = item->on_confirm(list, selected);
			else if (type==MENU_INPUT) await_input = 1;
			else if (list->on_confirm) result = list->on_confirm(list, selected);
			if (result==MENU_CALLBACK_EXIT) show_options = 0;
			else {
				if (result==MENU_CALLBACK_NEXT_ITEM) NEXT_ITEM();
				dirty = 1;
			}
		}
		else if (type==MENU_INPUT && PAD_justPressed(BTN_X)) {
			MenuItem* item = &items[selected];
			item->value = 0;
			SFX_play(SFX_BACK); // cleared
			if (list->on_change) list->on_change(list, selected);
			NEXT_ITEM();
			dirty = 1;
		}

		if (!defer_menu) PWR_update(&dirty, &show_settings, beforeSleep, afterSleep);
		if (defer_menu && PAD_justReleased(BTN_MENU)) defer_menu = 0;

		if (dirty) {
			GFX_clear(screen);
			int header_scrolls = GFX_blitHeader(screen, list->title, GFX_blitHardwareGroup(screen, show_settings));

			char* desc = NULL;
			SDL_Surface* text;

			if (type==MENU_LIST) {
				int mw = list->max_width;
				if (!mw) {
					for (int i=0; i<count; i++) {
						int w = 0;
						TTF_SizeUTF8(font.small, items[i].name, &w, NULL);
						w += SCALE1(OPTION_PADDING*2);
						if (w>mw) mw = w;
					}
					list->max_width = mw = MIN(mw, screen->w - SCALE1(PADDING*2));
				}
				int ox = (screen->w - mw) / 2;
				int oy = SCALE1(PADDING + PILL_SIZE);
				int selected_row = selected - start;
				for (int i=start,j=0; i<end; i++,j++) {
					MenuItem* item = &items[i];
					SDL_Color text_color = COLOR_WHITE;
					if (j==selected_row) {
						int w = 0;
						TTF_SizeUTF8(font.small, item->name, &w, NULL);
						w += SCALE1(OPTION_PADDING*2);
						GFX_blitPill(ASSET_BUTTON, screen, &(SDL_Rect){ox, oy+SCALE1(j*BUTTON_SIZE), w, SCALE1(BUTTON_SIZE)});
						text_color = COLOR_BLACK;
						if (item->desc) desc = item->desc;
					}
					text = TTF_RenderUTF8_Blended(font.small, item->name, text_color);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ox+SCALE1(OPTION_PADDING), oy+SCALE1((j*BUTTON_SIZE)+1)});
					SDL_FreeSurface(text);
				}
			}
			else {
				int mw = list->max_width;
				if (type==MENU_FIXED) mw = screen->w - SCALE1(PADDING*2);
				else if (!mw) {
					int mrw = 0; // every value list in an input table is the same
					for (int i=0; i<count; i++) {
						MenuItem* item = &items[i];
						int w = 0, lw = 0, rw = 0;
						TTF_SizeUTF8(font.small, item->name, &lw, NULL);
						if (!mrw || type!=MENU_INPUT) {
							for (int j=0; item->values[j]; j++) {
								TTF_SizeUTF8(font.tiny, item->values[j], &rw, NULL);
								if (lw+rw>w) w = lw+rw;
								if (rw>mrw) mrw = rw;
							}
						}
						else w = lw + mrw;
						w += SCALE1(OPTION_PADDING*4);
						if (w>mw) mw = w;
					}
					list->max_width = mw = MIN(mw, screen->w - SCALE1(PADDING*2));
				}
				int ox = type==MENU_FIXED ? SCALE1(PADDING) : (screen->w - mw) / 2;
				int oy = SCALE1(PADDING + PILL_SIZE);
				int selected_row = selected - start;
				for (int i=start,j=0; i<end; i++,j++) {
					MenuItem* item = &items[i];
					SDL_Color text_color = COLOR_WHITE;
					if (j==selected_row) {
						GFX_blitPill(ASSET_OPTION, screen, &(SDL_Rect){ox, oy+SCALE1(j*BUTTON_SIZE), mw, SCALE1(BUTTON_SIZE)});
						int w = 0;
						TTF_SizeUTF8(font.small, item->name, &w, NULL);
						w += SCALE1(OPTION_PADDING*2);
						GFX_blitPill(ASSET_BUTTON, screen, &(SDL_Rect){ox, oy+SCALE1(j*BUTTON_SIZE), w, SCALE1(BUTTON_SIZE)});
						text_color = COLOR_BLACK;
						if (item->desc) desc = item->desc;
					}
					text = TTF_RenderUTF8_Blended(font.small, item->name, text_color);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ox+SCALE1(OPTION_PADDING), oy+SCALE1((j*BUTTON_SIZE)+1)});
					SDL_FreeSurface(text);
					if (!(await_input && j==selected_row) && item->value>=0) {
						text = TTF_RenderUTF8_Blended(font.tiny, item->values[item->value], COLOR_WHITE); // always white
						SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ox + mw - text->w - SCALE1(OPTION_PADDING), oy+SCALE1((j*BUTTON_SIZE)+3)});
						SDL_FreeSurface(text);
					}
				}
			}

			if (count>max_visible_options) {
				int ox = (screen->w - SCALE1(SCROLL_WIDTH))/2;
				int oy = SCALE1((PILL_SIZE - SCROLL_HEIGHT) / 2);
				if (start>0) GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING) + oy});
				if (end<count) GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, screen->h - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE) + oy});
			}

			if (!desc && list->desc) desc = list->desc;
			if (desc) {
				int w,h;
				GFX_sizeText(font.tiny, desc, SCALE1(12), &w,&h);
				int y = screen->h - SCALE1(PADDING) - h;
				int below_arrow = screen->h - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE) + SCALE1((PILL_SIZE - SCROLL_HEIGHT) / 2 + SCROLL_HEIGHT);
				if (y<below_arrow) y = below_arrow;
				if (y+h>screen->h) y = screen->h - h;
				GFX_blitText(font.tiny, desc, SCALE1(12), COLOR_WHITE, screen, &(SDL_Rect){(screen->w - w) / 2, y, w, h});
			}

			flipMenu();
			dirty = header_scrolls; // (a long title keeps scrolling)
		}
		else GFX_sync();
	}
	return MENU_CALLBACK_NOP;
}

///////////////////////////////
// its options

#define BORDER_STYLES 4 // border_labels, without Off
// the settings files can be damaged or edited, and these index tables
static void clampSettings(void) {
	layout_index = MAX(0, MIN(layout_index, LAYOUT_COUNT-1));
	border_style = MAX(0, MIN(border_style, BORDER_STYLES));
	effect_type = MAX(EFFECT_NONE, MIN(effect_type, EFFECT_GRID));
	swap_screens = !!swap_screens;
	prevent_tearing = !!prevent_tearing;
	stick_dpad = !!stick_dpad;
	for (int i=0; i<EMU_OPTION_COUNT; i++) {
		int count = 0;
		while (emu_options[i].labels[count]) count += 1;
		if (emu_options[i].value<0 || emu_options[i].value>=count) emu_options[i].value = emu_options[i].default_value;
	}
}

static char* layout_labels[LAYOUT_COUNT+1];
static char* border_labels[] = {"Off","Style 1","Style 2","Style 3","Style 4",NULL};
static char* screen_labels[] = {"Top","Bottom",NULL};
static char* effect_labels[] = {"None","Line","Grid",NULL};
static char* stick_labels[] = {"Stylus","D-Pad",NULL};
static char* button_labels[BUTTON_COUNT*2]; // none, then each button, then MENU+ each

static char* cpu_labels[] = {"Powersave","Normal","Performance",NULL};
static int OptionFrontend_onChange(MenuList* list, int i) {
	layout_index = list->items[0].value;
	border_style = list->items[1].value;
	swap_screens = list->items[2].value;
	effect_type = list->items[3].value;
	prevent_tearing = list->items[4].value;
	stick_dpad = list->items[5].value;
	cpu_speed = list->items[6].value; // (applies as the menu closes)
	border_dirty = 1;
	saveSettings();
	return MENU_CALLBACK_NOP;
}
static MenuItem frontend_items[] = {
	{"Screen Layout", "Has its own keys under Shortcuts.", layout_labels},
	{"Screen Bezel", "The bezel around the screens.\nSome layouts don't have one.", border_labels},
	{"Main Screen", "The screen a layout shows biggest.\nHas its own keys under Shortcuts.", screen_labels},
	{"Screen Effect", "Grid draws the DS's pixel grid, on larger screens.\nLine simulates CRT scanlines.", effect_labels},
	{"Prevent Tearing", "Waits for the screen before showing a frame.\nTurn off if a game runs slower with it.", off_on},
	{"Thumbstick", "Moves the stylus, or works as a second d-pad.\nSave Changes can keep it for just this game.", stick_labels},
	{"CPU Speed", "Normal is what the A30 holds for long.\nPerformance is faster, but can freeze games.", cpu_labels},
	{NULL},
};
static MenuList OptionFrontend_menu = {.type=MENU_VAR, .title="Frontend Options", .items=frontend_items, .on_change=OptionFrontend_onChange};
static int OptionFrontend_openMenu(MenuList* list, int i) {
	frontend_items[0].value = layout_index;
	frontend_items[1].value = border_style;
	frontend_items[2].value = swap_screens;
	frontend_items[3].value = effect_type;
	frontend_items[4].value = prevent_tearing;
	frontend_items[5].value = stick_dpad;
	frontend_items[6].value = cpu_speed;
	Menu_options(&OptionFrontend_menu);
	return MENU_CALLBACK_NOP;
}

static MenuItem* emulator_items;
static int OptionEmulator_onChange(MenuList* list, int i) {
	emu_options[i].value = list->items[i].value;
	saveSettings();
	return MENU_CALLBACK_NOP;
}
static MenuList OptionEmulator_menu = {.type=MENU_FIXED, .title="Emulator Options", .desc="DraStic applies these when the game next starts.", .on_change=OptionEmulator_onChange};
static int OptionEmulator_openMenu(MenuList* list, int i) {
	if (!emulator_items) {
		emulator_items = calloc(EMU_OPTION_COUNT+1, sizeof(MenuItem));
		for (int j=0; j<EMU_OPTION_COUNT; j++) {
			emulator_items[j].name = (char*)emu_options[j].name;
			emulator_items[j].desc = (char*)emu_options[j].desc;
			emulator_items[j].values = (char**)emu_options[j].labels;
		}
		OptionEmulator_menu.items = emulator_items;
	}
	for (int j=0; j<EMU_OPTION_COUNT; j++) emulator_items[j].value = emu_options[j].value;
	Menu_options(&OptionEmulator_menu);
	return MENU_CALLBACK_NOP;
}

static int bindingValue(Action* action) {
	if (action->btn==BTN_ID_NONE) return 0;
	return buttonIndex(action->btn) + (action->mod ? BUTTON_COUNT-1 : 0);
}
static int OptionBinding_onChange(MenuList* list, int i) { // X cleared it
	Action* action = &actions[list->items[i].id];
	action->btn = BTN_ID_NONE;
	action->mod = 0;
	saveSettings();
	return MENU_CALLBACK_NOP;
}
static int OptionBinding_bind(MenuList* list, int i) { // waits for a button
	MenuItem* item = &list->items[i];
	Action* action = &actions[item->id];
	int bound = 0;
	while (!bound) {
		pollMenu(NULL, NULL);
		for (int b=1; b<BUTTON_COUNT; b++) {
			if (!PAD_justPressed(1<<buttons[b].id)) continue;
			action->btn = buttons[b].id;
			action->mod = PAD_isPressed(BTN_MENU) ? 1 : 0;
			item->value = bindingValue(action);
			bound = 1;
			break;
		}
		GFX_sync();
	}
	saveSettings();
	return MENU_CALLBACK_NEXT_ITEM;
}
#define BIND_DESC "Press A to set and X to clear.\nSupports single button and MENU+button."
static MenuList OptionControls_menu = {.type=MENU_INPUT, .title="Controls", .desc=BIND_DESC, .on_confirm=OptionBinding_bind, .on_change=OptionBinding_onChange};
static MenuList OptionShortcuts_menu = {.type=MENU_INPUT, .title="Shortcuts", .desc=BIND_DESC, .on_confirm=OptionBinding_bind, .on_change=OptionBinding_onChange};
static int openBindings(MenuList* menu, int from, int to) {
	if (!menu->items) {
		menu->items = calloc(to-from+1, sizeof(MenuItem));
		for (int j=from; j<to; j++) {
			MenuItem* item = &menu->items[j-from];
			item->name = (char*)actions[j].name;
			item->id = j;
			item->values = button_labels;
		}
	}
	for (int j=from; j<to; j++) menu->items[j-from].value = bindingValue(&actions[j]);
	Menu_options(menu);
	return MENU_CALLBACK_NOP;
}
static int OptionControls_openMenu(MenuList* list, int i) { return openBindings(&OptionControls_menu, 0, ACTION_NEXT_LAYOUT); }
static int OptionShortcuts_openMenu(MenuList* list, int i) { return openBindings(&OptionShortcuts_menu, ACTION_NEXT_LAYOUT, ACTION_COUNT); }

static char* getSaveDesc(void) {
	switch (settings_loaded) {
		case SETTINGS_NONE: return "Using defaults.";
		case SETTINGS_CONSOLE: return "Using console config.";
		case SETTINGS_GAME: return "Using game config.";
	}
	return NULL;
}
static int OptionSaveChanges_onConfirm(MenuList* list, int i) {
	char* message;
	switch (i) {
		case 0:
			writeSettings(SETTINGS_CONSOLE, 1);
			message = "Saved for console.";
			break;
		case 1:
			writeSettings(SETTINGS_GAME, 0);
			message = "Saved for game.";
			break;
		default:
			restoreSettings();
			message = settings_loaded ? "Restored console defaults." : "Restored defaults.";
			break;
	}
	Menu_message(message, (char*[]){"A","OKAY",NULL});
	return MENU_CALLBACK_EXIT;
}
static MenuList OptionSaveChanges_menu = {
	.type = MENU_LIST,
	.title = "Save Changes",
	.on_confirm = OptionSaveChanges_onConfirm,
	.items = (MenuItem[]){{"Save for console"},{"Save for game"},{"Restore defaults"},{NULL}},
};
static int OptionSaveChanges_openMenu(MenuList* list, int i) {
	OptionSaveChanges_menu.desc = getSaveDesc();
	Menu_options(&OptionSaveChanges_menu);
	return MENU_CALLBACK_NOP;
}
static MenuList options_menu = {
	.type = MENU_LIST,
	.title = "Options",
	.items = (MenuItem[]){
		{"Frontend", "MinUI (DraStic layer)", .on_confirm=OptionFrontend_openMenu},
		{"Emulator", .on_confirm=OptionEmulator_openMenu},
		{"Controls", .on_confirm=OptionControls_openMenu},
		{"Shortcuts", .on_confirm=OptionShortcuts_openMenu},
		{"Save Changes", .on_confirm=OptionSaveChanges_openMenu},
		{NULL},
	},
};
static void initOptions(void) {
	for (int i=0; i<LAYOUT_COUNT; i++) layout_labels[i] = (char*)layouts[i].name;
	button_labels[0] = "NONE";
	for (int b=1; b<BUTTON_COUNT; b++) {
		char name[32];
		snprintf(name, sizeof(name), "MENU+%s", buttons[b].name);
		button_labels[b] = (char*)buttons[b].name;
		button_labels[BUTTON_COUNT-1+b] = strdup(name);
	}
}

///////////////////////////////
// the menu itself

enum { ITEM_CONT, ITEM_SAVE, ITEM_LOAD, ITEM_ACHIEVEMENTS, ITEM_OPTS, ITEM_QUIT, MENU_ITEM_COUNT };
static char* menu_items[MENU_ITEM_COUNT] = {"Continue","Save","Load","Achievements","Options","Quit"};

static void runMenu(void) {
	GFX_clearSettingOverGame(); // the menu draws its own
	char* menu_message = NULL;
	uint32_t menu_message_until = 0;
	int in_switcher = -1;

#ifdef NDSLAYER_TEST
	fprintf(stderr, "menu opens at poll %i\n", test_polls);
#endif
	pauseBegin();
	in_menu = 1;
	Play_save();

	SDL_Surface* backing = shot_surface();
	composeShot(backing);
	SDL_Surface* overlay = SDL_CreateRGBSurface(SDL_SWSURFACE, FIXED_WIDTH, FIXED_HEIGHT, FIXED_DEPTH, RGBA_MASK_565);
	SDL_SetSurfaceBlendMode(overlay, SDL_BLENDMODE_BLEND);
	SDL_SetSurfaceAlphaMod(overlay, 0xB8);
	SDL_FillRect(overlay, NULL, 0);
	SDL_Surface* preview = SDL_CreateRGBSurface(SDL_SWSURFACE, DEVICE_WIDTH/2, DEVICE_HEIGHT/2, FIXED_DEPTH, RGBA_MASK_565);

	SDL_GL_SetSwapInterval(1);
	BGM_start();
	PWR_warn(0);
	if (!HAS_POWER_BUTTON) PWR_enableSleep();
	PWR_setCPUSpeedAsync(CPU_SPEED_POWERSAVE);
	GFX_setVsync(VSYNC_STRICT);
	GFX_setEffect(EFFECT_NONE);
	PWR_enableAutosleep();
	PAD_reset();

	char rom_name[256];
	getDisplayName(rom_file, rom_name);
	getAlias(rom_path, rom_name);

	int selected = 0;
	int hidden_item = RA_hasAchievements() ? -1 : ITEM_ACHIEVEMENTS;
	int item_count = MENU_ITEM_COUNT - (hidden_item!=-1);
	char slot_path[800];
	snprintf(slot_path, sizeof(slot_path), "%s/%s.txt", minui_dir, rom_file);
	if (exists(slot_path)) menu_slot = getInt(slot_path);
	if (menu_slot<0 || menu_slot>=MENU_SLOT_COUNT) menu_slot = 0;

	int save_exists = 0, preview_exists = 0;
	char bmp_path[800] = "";
	char preview_shown[800] = ""; // the picture already in preview, so redraws don't read it from the card again
	int show_setting = 0;
	int dirty = 1;
	int title_scrolls = 0, title_width = 0;
	int show_menu = 1;

	while (show_menu) {
		GFX_startFrame();
		uint32_t now = SDL_GetTicks();

		PAD_poll();
		testPad();
		if (menu_message_until && now>=menu_message_until) {
			menu_message_until = 0;
			dirty = 1;
		}
		else if (menu_message_until && menu_message_until-now<MENU_MESSAGE_FADE_MS) dirty = 1;

		if (PAD_justPressed(BTN_UP)) {
			selected -= 1;
			if (selected<0) selected += MENU_ITEM_COUNT;
			if (selected==hidden_item) selected -= 1;
			dirty = 1;
			SFX_play(SFX_TICK);
		}
		else if (PAD_justPressed(BTN_DOWN)) {
			selected += 1;
			if (selected==hidden_item) selected += 1;
			if (selected>=MENU_ITEM_COUNT) selected -= MENU_ITEM_COUNT;
			dirty = 1;
			SFX_play(SFX_TICK);
		}
		else if (PAD_justPressed(BTN_LEFT) && (selected==ITEM_SAVE || selected==ITEM_LOAD)) {
			menu_slot -= 1;
			if (menu_slot<0) menu_slot += MENU_SLOT_COUNT;
			dirty = 1;
			SFX_play(SFX_TICK);
		}
		else if (PAD_justPressed(BTN_RIGHT) && (selected==ITEM_SAVE || selected==ITEM_LOAD)) {
			menu_slot += 1;
			if (menu_slot>=MENU_SLOT_COUNT) menu_slot -= MENU_SLOT_COUNT;
			dirty = 1;
			SFX_play(SFX_TICK);
		}

		if (dirty && (selected==ITEM_SAVE || selected==ITEM_LOAD)) {
			slotBmpPath(menu_slot, bmp_path, sizeof(bmp_path));
			save_exists = slotExists(menu_slot);
			preview_exists = save_exists && exists(bmp_path);
		}

		if (PAD_justPressed(BTN_B) || (BTN_WAKE!=BTN_MENU && PAD_tappedMenu(now))) {
			show_menu = 0;
			SFX_play(SFX_BACK);
		}
		else if (PAD_justPressed(BTN_X)) {
			SFX_play(SFX_CONFIRM);
			char* path = switcherPath();
			if (Switcher_has(path)) {
				Switcher_remove(path);
				menu_message = "Unpinned from game switcher";
			}
			else if (Switcher_add(path)==-1) menu_message = "Game switcher is full (10)";
			else menu_message = "Pinned to game switcher";
			menu_message_until = now + 1500;
			in_switcher = -1;
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_A) && selected==ITEM_LOAD && !save_exists) {
			// an empty slot: nothing to load, so the menu stays (it says "Empty Slot")
		}
		else if (PAD_justPressed(BTN_A)) {
#ifdef NDSLAYER_TEST
			fprintf(stderr, "menu A on %i at poll %i\n", selected, test_polls);
#endif
			SFX_play(SFX_CONFIRM);
			switch (selected) {
				case ITEM_CONT:
					show_menu = 0;
					break;
				case ITEM_SAVE:
					saveToSlot(menu_slot, backing);
					show_menu = 0;
					break;
				case ITEM_LOAD:
					loadFromSlot(menu_slot);
					show_menu = 0;
					break;
				case ITEM_ACHIEVEMENTS:
					RA_showAchievements(screen, beforeSleep, afterSleep);
					dirty = 1;
					break;
				case ITEM_OPTS:
					Menu_options(&options_menu);
					composeShot(backing); // the layout may have changed
					dirty = 1;
					break;
				case ITEM_QUIT:
					show_menu = 0;
					Play_save();
					quitGame();
					break;
			}
			if (!show_menu) break;
		}

		PWR_update(&dirty, &show_setting, beforeSleep, afterSleep);

		if (dirty) {
			GFX_clear(screen);
			SDL_BlitSurface(backing, NULL, screen, NULL);
			SDL_BlitSurface(overlay, NULL, screen, NULL);

			int ox, oy;
			int ow = GFX_blitHardwareGroup(screen, show_setting);
			int max_width = screen->w - SCALE1(PADDING * 2 + BUTTON_MARGIN) - ow; // a gap before the status pill

			title_scrolls = GFX_blitTitle(screen, rom_name, max_width);
			title_width = max_width;
			SDL_Surface* text;

			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			else {
				if (in_switcher==-1) in_switcher = Switcher_has(switcherPath());
				GFX_blitButtonGroup((char*[]){BTN_SLEEP==BTN_POWER?"POWER":"MENU","SLEEP", "X",in_switcher?"UNPIN":"PIN", NULL}, 0, screen, 0);
			}
			GFX_blitButtonGroup((char*[]){"B","BACK", "A","OKAY", NULL}, 1, screen, 1);

			int text_oy = (SCALE1(MENU_ROW_SIZE) - TTF_FontHeight(font.medium)) / 2;
			oy = (((DEVICE_HEIGHT / FIXED_SCALE) - PADDING * 2) - (item_count * MENU_ROW_SIZE)) / 2;
			for (int i=0, r=0; i<MENU_ITEM_COUNT; i++) {
				if (i==hidden_item) continue;
				char* item = menu_items[i];
				SDL_Color text_color = COLOR_WHITE;
				if (i==selected) {
					TTF_SizeUTF8(font.medium, item, &ow, NULL);
					ow += SCALE1(BUTTON_PADDING*2);
					Menu_blitPill(RGB_WHITE, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(oy + PADDING + (r * MENU_ROW_SIZE)), ow, SCALE1(MENU_ROW_SIZE)});
					text_color = COLOR_BLACK;
				}
				else {
					text = TTF_RenderUTF8_Blended(font.medium, item, COLOR_BLACK);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){SCALE1(2 + PADDING + BUTTON_PADDING), SCALE1(1 + PADDING + oy + (r * MENU_ROW_SIZE)) + text_oy});
					SDL_FreeSurface(text);
				}
				text = TTF_RenderUTF8_Blended(font.medium, item, text_color);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), SCALE1(oy + PADDING + (r * MENU_ROW_SIZE)) + text_oy});
				SDL_FreeSurface(text);
				r += 1;
			}

			if (selected==ITEM_SAVE || selected==ITEM_LOAD) {
				int hw = DEVICE_WIDTH / 2;
				int hh = DEVICE_HEIGHT / 2;
				int pw = hw + SCALE1(WINDOW_RADIUS*2);
				int ph = hh + SCALE1(WINDOW_RADIUS*2 + PAGINATION_HEIGHT + WINDOW_RADIUS);
				ox = DEVICE_WIDTH - pw - SCALE1(PADDING);
				oy = (DEVICE_HEIGHT - ph) / 2;

				GFX_blitRect(ASSET_STATE_BG, screen, &(SDL_Rect){ox,oy,pw,ph});
				ox += SCALE1(WINDOW_RADIUS);
				oy += SCALE1(WINDOW_RADIUS);

				if (preview_exists && strcmp(preview_shown, bmp_path)) {
					SDL_Surface* bmp = IMG_Load(bmp_path);
					if (bmp) {
						SDL_Surface* raw = SDL_ConvertSurface(bmp, screen->format, SDL_SWSURFACE);
						SDL_FillRect(preview, NULL, 0);
						if (raw) {
							SDL_BlitScaled(raw, NULL, preview, NULL);
							SDL_FreeSurface(raw);
						}
						SDL_FreeSurface(bmp);
						snprintf(preview_shown, sizeof(preview_shown), "%s", bmp_path);
					}
				}
				if (preview_exists && !strcmp(preview_shown, bmp_path)) SDL_BlitSurface(preview, NULL, screen, &(SDL_Rect){ox,oy});
				else {
					SDL_Rect preview_rect = {ox,oy,hw,hh};
					SDL_FillRect(screen, &preview_rect, 0);
					GFX_blitMessage(font.large, save_exists ? "No Preview" : "Empty Slot", screen, &preview_rect);
				}

				ox += (pw-SCALE1(15*MENU_SLOT_COUNT))/2;
				oy += hh+SCALE1(WINDOW_RADIUS);
				for (int i=0; i<MENU_SLOT_COUNT; i++) {
					if (i==menu_slot) GFX_blitAsset(ASSET_PAGE, NULL, screen, &(SDL_Rect){ox+SCALE1(i*15),oy});
					else GFX_blitAsset(ASSET_DOT, NULL, screen, &(SDL_Rect){ox+SCALE1(i*15)+4,oy+SCALE1(2)});
				}
			}
			else {
				PlayTime current = play.saved;
				current.seconds = Play_getTotal();
				char total[32], session[32], sessions[16], average[32];
				formatPlayTime(current.seconds, total);
				formatPlayTime(Play_getSession(), session);
				sprintf(sessions, "%i", current.sessions);
				formatPlayTime(getAveragePlayTime(&current), average);
				char* labels[] = {"Play time", "This session", "Sessions", "Average"};
				char* values[] = {total, session, sessions, average};

				int label_w = 0, value_w = 0;
				for (int i=0; i<STATS_ROWS; i++) {
					int w;
					TTF_SizeUTF8(font.small, labels[i], &w, NULL);
					label_w = MAX(label_w, w);
					TTF_SizeUTF8(font.small, values[i], &w, NULL);
					value_w = MAX(value_w, w);
				}
				int pw = label_w + value_w + SCALE1(BUTTON_PADDING*3);
				int ph = SCALE1(STATS_ROW_SIZE * STATS_ROWS + BUTTON_PADDING*2);
				ox = DEVICE_WIDTH - pw - SCALE1(PADDING);
				oy = (DEVICE_HEIGHT - ph) / 2;
				GFX_blitRect(ASSET_BLACK_PILL, screen, &(SDL_Rect){ox,oy,pw,ph});

				int stats_oy = (SCALE1(STATS_ROW_SIZE) - TTF_FontHeight(font.small)) / 2;
				for (int i=0; i<STATS_ROWS; i++) {
					int ty = oy + SCALE1(BUTTON_PADDING + i * STATS_ROW_SIZE) + stats_oy;
					text = TTF_RenderUTF8_Blended(font.small, labels[i], COLOR_DARK_TEXT);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ox + SCALE1(BUTTON_PADDING), ty});
					SDL_FreeSurface(text);
					text = TTF_RenderUTF8_Blended(font.small, values[i], COLOR_WHITE);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ox + pw - SCALE1(BUTTON_PADDING) - text->w, ty});
					SDL_FreeSurface(text);
				}
			}

			if (menu_message_until) {
				text = TTF_RenderUTF8_Blended(font.large, menu_message, COLOR_WHITE);
				int pw = text->w + SCALE1(BUTTON_PADDING*4);
				int ph = text->h + SCALE1(BUTTON_PADDING*2);
				SDL_Rect box = {(screen->w-pw)/2, (screen->h-ph)/2, pw, ph};
				int left = (int32_t)(menu_message_until - SDL_GetTicks());
				SDL_Surface* under = left<MENU_MESSAGE_FADE_MS ? SDL_CreateRGBSurfaceWithFormat(0, pw, ph, screen->format->BitsPerPixel, screen->format->format) : NULL;
				if (under) SDL_BlitSurface(screen, &box, under, NULL);
				GFX_blitRect(ASSET_DARK_GRAY_PILL, screen, &box);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){box.x + (pw-text->w)/2, box.y + (ph-text->h)/2});
				SDL_FreeSurface(text);
				if (under) {
					SDL_SetSurfaceBlendMode(under, SDL_BLENDMODE_BLEND);
					SDL_SetSurfaceAlphaMod(under, 255 - 255 * MAX(left, 0) / MENU_MESSAGE_FADE_MS);
					SDL_BlitSurface(under, NULL, screen, &(SDL_Rect){box.x, box.y});
					SDL_FreeSurface(under);
				}
			}

			flipMenu();
			dirty = 0;
		}
		else if (title_scrolls) { // just its strip: the rest of the menu, and any preview, stay as drawn
			SDL_Rect strip = { SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE) };
			SDL_BlitSurface(backing, &strip, screen, &(SDL_Rect){ strip.x, strip.y });
			SDL_BlitSurface(overlay, &strip, screen, &(SDL_Rect){ strip.x, strip.y });
			GFX_blitTitle(screen, rom_name, title_width);
			flipMenu();
		}
		else GFX_sync();
	}

#ifdef NDSLAYER_TEST
	fprintf(stderr, "menu closes at poll %i\n", test_polls);
#endif
	BGM_pause();
	SDL_FreeSurface(preview);
	SDL_FreeSurface(overlay);
	PAD_reset();
	GFX_clearAll();
	PWR_warn(1);
	SDL_GL_SetSwapInterval(prevent_tearing ? 1 : 0);
	GFX_setVsync(VSYNC_OFF);
	GFX_setEffect(effect_type);
	PWR_setCPUSpeedAsync(gameCPUSpeed());
	PWR_disableAutosleep(); // nobody touches the buttons through a cutscene
	if (!HAS_POWER_BUTTON) PWR_disableSleep();
	in_menu = 0;
	play.last_tick = SDL_GetTicks();
	pauseEnd();
}

///////////////////////////////
// what DraStic calls instead of SDL

typedef struct LayerTexture {
	SDL_Texture* real;
	int w, h;
} LayerTexture;

typedef struct Drawn {
	SDL_Texture* real;
	SDL_Rect src, dst;
} Drawn;

static int logical_w = FIXED_WIDTH, logical_h = FIXED_HEIGHT;
static int rotate;
static Drawn drawn_screens[2]; // this frame's, in the order DraStic drew them: top, then bottom
static int screens_drawn;

// the stylus: DraStic's own cursor is a fixed speed and its keys don't move it up or down here. This one follows the
// stick, reaches all of the bottom screen, and touches it the way a mouse does
#define STYLUS_SIZE 15
#define STYLUS_SPEED 5.0f // DS pixels a poll, with the stick all the way over
#define STYLUS_SHOWN_MS 3000
static float stylus_x = 128, stylus_y = 96;
static int stylus_pressed;
static uint32_t stylus_shown_until;
static SDL_Texture* stylus_texture;
static SDL_Texture* stylusTexture(void) {
	if (stylus_texture) return stylus_texture;
	static uint32_t pixels[STYLUS_SIZE*STYLUS_SIZE];
	int mid = STYLUS_SIZE/2;
	for (int y=0; y<STYLUS_SIZE; y++) {
		for (int x=0; x<STYLUS_SIZE; x++) {
			int dx = abs(x-mid), dy = abs(y-mid);
			int on = dx==0 || dy==0;
			int near = dx<=1 || dy<=1;
			pixels[y*STYLUS_SIZE+x] = on ? 0xFFFFFFFF : near ? 0xFF000000 : 0;
		}
	}
	stylus_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, STYLUS_SIZE, STYLUS_SIZE);
	SDL_SetTextureBlendMode(stylus_texture, SDL_BLENDMODE_BLEND);
	SDL_UpdateTexture(stylus_texture, NULL, pixels, STYLUS_SIZE*4);
	return stylus_texture;
}
static SDL_Texture* hidden_screen;

static int h_SDL_Init(Uint32 flags) { return 0; }
static SDL_bool h_SDL_SetHint(const char* name, const char* value) { return SDL_TRUE; }
static int h_SDL_GetCurrentDisplayMode(int index, SDL_DisplayMode* mode) {
	memset(mode, 0, sizeof(*mode));
	mode->format = SDL_PIXELFORMAT_RGB565;
	mode->w = FIXED_WIDTH;
	mode->h = FIXED_HEIGHT;
	mode->refresh_rate = 60;
	return 0;
}
static Uint32 screen_format = SDL_PIXELFORMAT_RGB888;
static SDL_BlendMode screen_blend = SDL_BLENDMODE_NONE;
static void chooseScreenFormat(void) {
	// DraStic's screens are 32-bit with a junk alpha byte (opaque every 4th pixel), and this display shows alpha
	// in the output as black stripes. A format without alpha is right if SDL takes it as is. If not it'd convert
	// every frame, so use its native format, with a blend that keeps what the screen's alpha already is
	SDL_RendererInfo info;
	if (SDL_GetRendererInfo(renderer, &info)==0) {
		fprintf(stderr, "layer: renderer %s, formats:", info.name);
		int native = 0;
		for (Uint32 i=0; i<info.num_texture_formats; i++) {
			fprintf(stderr, " %s", SDL_GetPixelFormatName(info.texture_formats[i]));
			if (info.texture_formats[i]==SDL_PIXELFORMAT_RGB888) native = 1;
		}
		fprintf(stderr, "\n");
		if (native) return;
	}
	screen_format = SDL_PIXELFORMAT_ARGB8888;
	screen_blend = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ZERO, SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
}
static void* watchdog(void* arg);
static void* h_SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) {
	screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	PWR_warn(1);
	PWR_disableAutosleep();
	if (!HAS_POWER_BUTTON) PWR_disableSleep();
	InitSettings();
	PWR_setCPUSpeed(gameCPUSpeed());
	GFX_setVsync(VSYNC_OFF);
	GFX_setEffect(effect_type);
	renderer = SDL_GetRenderer(PLAT_getWindow());
	rotate = PLAT_getRotate();
	chooseScreenFormat();
	setCoreOnline(2, 1); // DraStic's 3D has threads for more than the two cores the rest of MinUI+ keeps on
	setCoreOnline(3, 1);
	int interval = SDL_GL_SetSwapInterval(prevent_tearing ? 1 : 0);
	profile_on = getenv("NDSLAYER_PROFILE")!=NULL;
	profileReset();
	initOptions();
	Play_init();
	if (profile_on) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, watchdog, NULL)==0) pthread_detach(thread);
	}
	fprintf(stderr, "layer: screens in %s, blend %i, swap interval %s\n", SDL_GetPixelFormatName(screen_format), screen_blend!=SDL_BLENDMODE_NONE, interval==0 ? (prevent_tearing ? "on" : "off") : "unchanged");
	return (void*)1;
}
static int h_SDL_SetWindowFullscreen(void* window, Uint32 flags) { return 0; }
static void* h_SDL_CreateRenderer(void* window, int index, Uint32 flags) { return (void*)1; }
static int h_SDL_RenderSetLogicalSize(void* r, int w, int h) {
	logical_w = w;
	logical_h = h;
	return 0;
}
static void h_SDL_RenderGetLogicalSize(void* r, int* w, int* h) {
	if (w) *w = logical_w;
	if (h) *h = logical_h;
}
static void* h_SDL_CreateTexture(void* r, Uint32 format, int access, int w, int h) {
	LayerTexture* texture = calloc(1, sizeof(LayerTexture));
	int is_screen = !format; // it leaves the screens' format to SDL
	texture->real = SDL_CreateTexture(renderer, is_screen ? screen_format : format, access, w, h);
	if (is_screen && screen_blend!=SDL_BLENDMODE_NONE && SDL_SetTextureBlendMode(texture->real, screen_blend)<0) { // converting beats stripes
		SDL_DestroyTexture(texture->real);
		screen_format = SDL_PIXELFORMAT_RGB888;
		screen_blend = SDL_BLENDMODE_NONE;
		texture->real = SDL_CreateTexture(renderer, screen_format, access, w, h);
	}
	texture->w = w;
	texture->h = h;
	return texture;
}
static void h_SDL_DestroyTexture(void* t) {
	LayerTexture* texture = t;
	if (texture->real) SDL_DestroyTexture(texture->real);
	free(texture);
}
static int h_SDL_SetTextureBlendMode(void* t, SDL_BlendMode mode) {
	return SDL_SetTextureBlendMode(((LayerTexture*)t)->real, mode);
}
static int h_SDL_UpdateTexture(void* t, const SDL_Rect* rect, const void* pixels, int pitch) {
	if (((LayerTexture*)t)->real==hidden_screen) return 0; // the screen this layout doesn't show
	uint64_t started = profile_on ? nowUs() : 0;
	int result = SDL_UpdateTexture(((LayerTexture*)t)->real, rect, pixels, pitch);
	if (profile_on) profile_upload += nowUs()-started;
	return result;
}
static int h_SDL_SetRenderDrawBlendMode(void* r, SDL_BlendMode mode) { return SDL_SetRenderDrawBlendMode(renderer, mode); }
static int h_SDL_SetRenderDrawColor(void* r, Uint8 a, Uint8 b, Uint8 c, Uint8 d) { return SDL_SetRenderDrawColor(renderer, a, b, c, d); }
static int h_SDL_RenderClear(void* r) { return SDL_RenderClear(renderer); }
static int h_SDL_RenderCopy(void* r, void* t, const SDL_Rect* src, const SDL_Rect* dst) { // DraStic's own placement only tells us which is which
	LayerTexture* texture = t;
	Drawn* drawn = NULL;
	if (texture->w>=256 && texture->h>=192) {
		if (screens_drawn<2) drawn = &drawn_screens[screens_drawn++];
	}
	if (!drawn) return 0;
	drawn->real = texture->real;
	drawn->src = src ? *src : (SDL_Rect){0,0,texture->w,texture->h};
	drawn->dst = dst ? *dst : (SDL_Rect){0,0,logical_w,logical_h};
	return 0;
}
// a DS's LCD up close: inside each of its 256x192 pixels a red, a green and a blue stripe that fade into each other, and
// a soft gap between the rows. The same idea as cgwg's LCD grid shader (written fresh here, without its curves or passes):
// the pattern only depends on where a panel pixel falls in a DS pixel, so it's worked out once, into a texture that
// multiplies whatever's under it, which the A30's GPU does for almost nothing. Too small a screen has no room for it
#define DS_LCD_MIN_WIDTH 410 // about 1.6x
#define DS_LCD_STRIPE 1.5f // how far a stripe reaches, in thirds of a DS pixel: further and the colors just mix
#define DS_LCD_CELL 0.8f // how far a pixel's light reaches from its middle, in DS pixels: past 0.5 they meet in a soft gap
#define DS_LCD_TINT 0.2f // how much of the stripes' color to keep: at 2.5x they're finer than the panel, and full strength beats into bands
#define DS_LCD_STRIPE_MEAN 0.533f // what a stripe averages over a DS pixel
static float dsLcdBump(float distance, float reach) { // 1 in the middle, easing to 0
	float x = distance/reach;
	if (x<=-1 || x>=1) return 0;
	x = 1 - x*x;
	return x*x;
}
// how much a stripe at center (a share of a DS pixel) lights the panel pixel from..to (in DS pixels), wherever it repeats
static float dsLcdCover(float from, float to, float center, float reach, float scale) {
	float sum = 0;
	for (int i=0; i<8; i++) {
		float at = from + (to-from)*(i+0.5f)/8;
		float place = at - floorf(at);
		for (int k=-1; k<=1; k++) sum += dsLcdBump((place-center-k)*scale, reach);
	}
	return sum/8;
}
static uint8_t dsLcdByte(float light) { // as the display wants it
	if (light>1) light = 1;
	return powf(light, 1/2.2f) * 255;
}
static SDL_Texture* dsLcdTexture(int slot, int w, int h) {
	static SDL_Texture* textures[2];
	static int sizes[2][2];
	if (w<DS_LCD_MIN_WIDTH) return NULL;
	if (textures[slot] && sizes[slot][0]==w && sizes[slot][1]==h) return textures[slot];
	if (textures[slot]) SDL_DestroyTexture(textures[slot]);
	textures[slot] = NULL;
	uint32_t* pixels = malloc(w*h*sizeof(uint32_t));
	float (*across)[3] = malloc(w*sizeof(*across));
	float* down = malloc(h*sizeof(float));
	SDL_Texture* texture = pixels && across && down ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h) : NULL;
	if (texture) {
		for (int x=0; x<w; x++) {
			float cell = dsLcdCover(x*256.0f/w, (x+1)*256.0f/w, 0.5f, DS_LCD_CELL, 1);
			for (int c=0; c<3; c++) {
				float stripe = dsLcdCover(x*256.0f/w, (x+1)*256.0f/w, (c+0.5f)/3, DS_LCD_STRIPE, 3) / DS_LCD_STRIPE_MEAN;
				across[x][c] = cell * (1 - DS_LCD_TINT + DS_LCD_TINT*stripe);
			}
		}
		for (int y=0; y<h; y++) down[y] = dsLcdCover(y*192.0f/h, (y+1)*192.0f/h, 0.5f, DS_LCD_CELL, 1);
		for (int y=0; y<h; y++) {
			for (int x=0; x<w; x++) {
				pixels[y*w+x] = 0xFF000000 | dsLcdByte(across[x][0]*down[y])<<16 | dsLcdByte(across[x][1]*down[y])<<8 | dsLcdByte(across[x][2]*down[y]);
			}
		}
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_MOD);
		SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
		SDL_UpdateTexture(texture, NULL, pixels, w*sizeof(uint32_t));
	}
	free(pixels);
	free(across);
	free(down);
	textures[slot] = texture;
	sizes[slot][0] = w;
	sizes[slot][1] = h;
	return texture;
}
static void screenSharp(int i, SDL_Texture* screen, int sharp) { // the pattern only fits pixels drawn with hard edges
	static SDL_Texture* seen[2];
	static int was[2];
	if (seen[i]!=screen) {
		seen[i] = screen;
		was[i] = 0;
	}
	if (was[i]==sharp) return;
	SDL_SetTextureScaleMode(screen, sharp ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
	was[i] = sharp;
}
static void drawTexture(SDL_Texture* real, const SDL_Rect* src, SDL_Rect rect, int book) {
	// centre and size in landscape; this panel is on its side, so its centre turns with it
	int cx = rect.x + rect.w/2;
	int cy = rect.y + rect.h/2;
	int w = book ? rect.h : rect.w;
	int h = book ? rect.w : rect.h;
	int angle = book ? 270 : 0;
	if (rotate) {
		int turned = cx;
		cx = cy;
		cy = FIXED_WIDTH - turned;
		angle += rotate*90;
	}
	SDL_Rect out = {cx-w/2, cy-h/2, w, h};
	SDL_RenderCopyEx(renderer, real, src, &out, angle%360, NULL, SDL_FLIP_NONE);
}
static void drawFPSCounter(void) {
	static SDL_Texture* texture;
	static int frames, fps = -1, shown = -1, w, h;
	if (!emu_options[EMU_FPS_COUNTER].value) {
		fps_started = 0;
		return;
	}
	uint64_t now = nowUs();
	if (!fps_started) {
		fps_started = now;
		frames = 0;
	}
	frames += 1;
	if (now-fps_started>=1000000) {
		fps = (frames*1000000ULL + (now-fps_started)/2) / (now-fps_started);
		fps_started = now;
		frames = 0;
	}
	if (fps<0) return;
	if (fps!=shown) { // only once a second at most, so it hardly costs a thing
		char label[16];
		snprintf(label, sizeof(label), "%i FPS", fps);
		SDL_Surface* text = TTF_RenderUTF8_Blended(font.small, label, COLOR_WHITE);
		if (!text) return;
		w = text->w + SCALE1(4);
		h = text->h;
		SDL_Surface* back = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
		if (back) {
			SDL_FillRect(back, NULL, SDL_MapRGBA(back->format, 0, 0, 0, 160));
			SDL_BlitSurface(text, NULL, back, &(SDL_Rect){SCALE1(2), 0});
			if (texture) SDL_DestroyTexture(texture);
			texture = SDL_CreateTextureFromSurface(renderer, back);
			if (texture) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
			SDL_FreeSurface(back);
			shown = fps;
		}
		SDL_FreeSurface(text);
	}
	if (texture) drawTexture(texture, NULL, (SDL_Rect){FIXED_WIDTH-w-SCALE1(2), SCALE1(2), w, h}, 0);
}
static void h_SDL_RenderPresent(void* r) {
	uint64_t started = profile_on ? nowUs() : 0;
	if (border_dirty) loadBorder();
	const Layout* layout = &layouts[layout_index];
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	if (border_texture) drawTexture(border_texture, NULL, (SDL_Rect){0,0,FIXED_WIDTH,FIXED_HEIGHT}, 0);
	hidden_screen = NULL;
	for (int i=0; i<screens_drawn; i++) {
		SDL_Rect rect = screenRect(layout, i==drawn_bottom);
		if (rect.w) {
			int lcd = effect_type==EFFECT_GRID && (layout->book ? rect.h : rect.w)>=DS_LCD_MIN_WIDTH;
			screenSharp(i, drawn_screens[i].real, lcd);
			drawTexture(drawn_screens[i].real, &drawn_screens[i].src, rect, layout->book);
		}
		else hidden_screen = drawn_screens[i].real;
	}
	if (SDL_TICKS_PASSED(stylus_shown_until, SDL_GetTicks()) || stylus_pressed) { // wherever the bottom screen is, if it's showing
		SDL_Rect to = screenRect(layout, 1);
		if (to.w) {
			int x = stylus_x, y = stylus_y;
			SDL_Rect rect = {0, 0, STYLUS_SIZE, STYLUS_SIZE};
			if (layout->book) { // down the DS screen is across the panel
				rect.x = to.x + y*to.w/192 - STYLUS_SIZE/2;
				rect.y = to.y + to.h - x*to.h/256 - STYLUS_SIZE/2;
			}
			else {
				rect.x = to.x + x*to.w/256 - STYLUS_SIZE/2;
				rect.y = to.y + y*to.h/192 - STYLUS_SIZE/2;
			}
			drawTexture(stylusTexture(), NULL, rect, layout->book);
		}
	}
	if (effect_type==EFFECT_GRID) {
		for (int i=0; i<2; i++) {
			SDL_Rect rect = i ? layout->other : layout->main;
			if (!rect.w) continue;
			SDL_Texture* grid = dsLcdTexture(i, layout->book ? rect.h : rect.w, layout->book ? rect.w : rect.h);
			if (grid) drawTexture(grid, NULL, rect, layout->book);
		}
	}
	int scale = layout->main.w/256;
	SDL_Texture* effect = effect_type==EFFECT_GRID ? NULL : PLAT_getEffectTexture(scale<2 ? 2 : scale);
	if (effect) { // on the screens only, lined up with each one's corner
		SDL_Rect rect = layout->main;
		drawTexture(effect, &(SDL_Rect){0,0,rect.w,rect.h}, rect, 0);
		rect = layout->other;
		if (rect.w) drawTexture(effect, &(SDL_Rect){0,0,rect.w,rect.h}, rect, 0);
	}
	drawFPSCounter();
	PLAT_drawNotification();
	screens_drawn = 0;
#ifdef NDSLAYER_TEST
	static int presents, last_view = -1, shot_in;
	const char* every = getenv("NDSLAYER_SHOTS");
	int view = layout_index*2 + swap_screens;
	if (view!=last_view) { last_view = view; shot_in = 2; }
	if (every && (++presents%atoi(every)==0 || (shot_in && !--shot_in))) {
		static uint16_t pixels[FIXED_WIDTH*FIXED_WIDTH];
		SDL_Rect size;
		SDL_RenderGetViewport(renderer, &size);
		SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGB565, pixels, size.w*2);
		testShot("game", pixels, size.w, size.h, size.w*2);
	}
#endif
	if (!profile_on) {
		SDL_RenderPresent(renderer);
		return;
	}
	uint64_t drawn = nowUs();
	SDL_RenderPresent(renderer);
	uint64_t done = nowUs();
	profile_draw += drawn-started;
	profile_swap += done-drawn;
	profileFrame();
}
static void h_SDL_Delay(Uint32 ms) { SDL_Delay(ms); }
static const char* h_SDL_GetKeyName(SDL_Keycode key) { return "Key"; }
static SDL_Keymod h_SDL_GetModState(void) { return KMOD_NONE; }
static int h_SDL_NumJoysticks(void) { return 0; }
static void* h_SDL_JoystickOpen(int index) { return NULL; }
static const char* h_SDL_JoystickName(void* joystick) { return "None"; }
static int h_SDL_JoystickEventState(int state) { return 0; }
static void h_SDL_Quit(void) {
	Play_save();
	RA_quit();
	BGM_quit();
	SFX_quit();
	SND_quit();
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	SDL_Quit();
}

static SDL_Event events[ACTION_COUNT*2+4];
static int event_count, event_next;

static void queueStylus(int type) { // in DraStic's own idea of where the bottom screen is
	const SDL_Rect* bottom = &drawn_screens[drawn_bottom].dst;
	SDL_Event* event = &events[event_count++];
	memset(event, 0, sizeof(*event));
	event->type = type;
	if (type==SDL_MOUSEMOTION) {
		event->motion.state = stylus_pressed ? SDL_BUTTON_LMASK : 0;
		event->motion.x = bottom->x + (int)stylus_x;
		event->motion.y = bottom->y + (int)stylus_y;
	}
	else {
		event->button.button = SDL_BUTTON_LEFT;
		event->button.state = type==SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
		event->button.x = bottom->x + (int)stylus_x;
		event->button.y = bottom->y + (int)stylus_y;
	}
}
static float stylusStep(Action* action, int axis) { // the stick moves it by how far over it is, a button at full speed
	if (!action->down) return 0;
	if (action->btn<BTN_ID_ANALOG_UP || action->btn>BTN_ID_ANALOG_RIGHT) return STYLUS_SPEED;
	float tilt = abs(axis) / 32767.0f;
	if (tilt>1) tilt = 1;
	float step = tilt*tilt*STYLUS_SPEED;
	return step<0.5f ? 0.5f : step; // close to the middle is for aiming
}
static void updateStylus(void) {
	Action* stylus = &actions[ACTION_STYLUS_UP];
	float x = stylus_x + stylusStep(&stylus[3], pad.laxis.x) - stylusStep(&stylus[2], pad.laxis.x);
	float y = stylus_y + stylusStep(&stylus[1], pad.laxis.y) - stylusStep(&stylus[0], pad.laxis.y);
	if (x<0) x = 0; else if (x>255) x = 255;
	if (y<0) y = 0; else if (y>191) y = 191;
	int moved = (int)x!=(int)stylus_x || (int)y!=(int)stylus_y;
	stylus_x = x;
	stylus_y = y;
	if (moved) {
		stylus_shown_until = SDL_GetTicks()+STYLUS_SHOWN_MS;
		queueStylus(SDL_MOUSEMOTION);
	}
	if (stylus[4].down!=stylus_pressed) {
		stylus_pressed = stylus[4].down;
		stylus_shown_until = SDL_GetTicks()+STYLUS_SHOWN_MS;
		queueStylus(stylus_pressed ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP);
	}
}
static void queueKey(Action* action, int down) {
	SDL_Event* event = &events[event_count++];
	memset(event, 0, sizeof(*event));
	event->type = down ? SDL_KEYDOWN : SDL_KEYUP;
	event->key.state = down ? SDL_PRESSED : SDL_RELEASED;
	event->key.keysym.sym = action->sym;
	event->key.keysym.scancode = SDL_GetScancodeFromKey(action->sym);
}
// RetroAchievements reads the DS's main memory, which DraStic keeps in a mapping of its own. It's the one with the
// copy of the game's header that a DS keeps near the end of its 4 MB
#define DS_RAM_SIZE 0x400000
#define DS_HEADER_COPY 0x3FFE00
static uint8_t* ds_ram;
static void findMainRam(void) {
	char title[16];
	int fd = open(rom_path, O_RDONLY);
	if (fd<0) return;
	int size = read(fd, title, sizeof(title));
	close(fd);
	if (size!=sizeof(title)) return;
	FILE* maps = fopen("/proc/self/maps", "r");
	if (!maps) return;
	char line[640];
	uintptr_t run_start = 0, run_end = 0;
	while (!ds_ram) {
		char* got = fgets(line, sizeof(line), maps);
		unsigned long from = 0, to = 0;
		char perms[8] = "", name[512] = "";
		if (got) sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &from, &to, perms, name);
		if (got && perms[0]=='r' && from==run_end && strncmp(name, "/dev/", 5)!=0 && name[0]!='[') { // a mapping can be in pieces
			run_end = to;
			continue;
		}
		for (uintptr_t at=run_start; run_end-run_start>=DS_RAM_SIZE && at+DS_RAM_SIZE<=run_end; at+=4096) {
			if (memcmp((char*)at+DS_HEADER_COPY, title, sizeof(title))) continue;
			ds_ram = (uint8_t*)at;
			break;
		}
		if (!got) break;
		int usable = perms[0]=='r' && (strncmp(name, "/dev/", 5)!=0 || strncmp(name, "/dev/shm/", 9)==0) && name[0]!='[';
		run_start = usable ? from : 0;
		run_end = usable ? to : 0;
	}
	fclose(maps);
}
static void getDsMemory(unsigned id, uint8_t** data, size_t* size) {
	*data = NULL;
	*size = 0;
	if (id==RETRO_MEMORY_SYSTEM_RAM) {
		*data = ds_ram;
		*size = DS_RAM_SIZE;
	}
}
static void gameTick(void) { // once per emulated frame
	Play_tick();
	static int ticks;
	ticks += 1;
	if (ticks==1 && exists(RESUME_SLOT_PATH)) { // the launcher's Resume
		int slot = getInt(RESUME_SLOT_PATH);
		unlink(RESUME_SLOT_PATH);
		loadFromSlot(slot);
	}
	if (!ds_ram && ticks%60==1 && ticks<60*600) {
		findMainRam();
		if (ds_ram) {
			fprintf(stderr, "layer: DS memory at %p\n", ds_ram);
			RA_init("NDS", rom_path, NULL, 0, NULL, getDsMemory);
		}
		else if (ticks==60*300+1) fprintf(stderr, "layer: DS memory not found, no achievements\n");
	}
	if (!ds_ram) return;
	RA_doFrame();
	RA_update();
}
static uint64_t last_poll;
static void* watchdog(void* arg) { // says so in the log when the game stops, and which threads are waiting on what
	int reported = 0;
	while (1) {
		sleep(1);
		uint64_t gap = nowUs()-last_poll;
		if (pause_depth || !last_poll || gap<5000000) {
			reported = 0;
			continue;
		}
		if (reported) continue;
		reported = 1;
		fprintf(stderr, "layer: game stopped, no input poll for %.1f s\n", gap/1000000.0);
		fflush(stderr);
	}
	return NULL;
}
static int h_SDL_PollEvent(SDL_Event* event) {
	if (event_next<event_count) {
		*event = events[event_next++];
		return 1;
	}
	if (event_count) { // drained: it stops polling until the next frame
		event_count = event_next = 0;
		return 0;
	}
	uint64_t started = profile_on ? nowUs() : 0;
	last_poll = nowUs();
	PAD_poll();
	testPad();
	static int show_setting = 0; // the volume or brightness bar over the game, which also clicks each step
	PWR_update(NULL, &show_setting, beforeSleep, afterSleep);
	GFX_settingOverGame(show_setting);
	if (profile_on) {
		profile_poll += nowUs()-started;
		profile_polls += 1;
	}
	gameTick();
	// Menu alone opens the menu when it's let go; held, it changes the screens instead
	static int menu_used;
	static uint32_t menu_down_at;
	static int switcher_ready;
	if (PAD_justPressed(BTN_MENU)) {
		menu_used = RA_dismissNotification(); // a popup's up: MENU clears it instead
		switcher_ready = 0;
		menu_down_at = SDL_GetTicks();
	}
	if (PAD_isPressed(BTN_MENU)) {
		if (!PAD_justPressed(BTN_MENU) && PAD_justPressed(BTN_ANY)) menu_used = 1; // a combo, not a menu press
		if (!menu_used && !switcher_ready && SDL_GetTicks()-menu_down_at>=SWITCHER_HOLD_MS) { // the cue that it's ready
			switcher_ready = 1;
			VIB_pulse(32768, 60);
		}
		if (PAD_isPressed(BTN_PLUS) || PAD_isPressed(BTN_MINUS)) menu_used = 1; // brightness
	}
	if (PAD_justReleased(BTN_MENU) && !menu_used && SDL_GetTicks()-menu_down_at>=SWITCHER_HOLD_MS) { // held, then let go
		menu_used = 1;
		saveForSwitcher();
	}
	else if (PAD_justReleased(BTN_MENU) && !menu_used) {
		for (int i=0; i<ACTION_COUNT; i++) {
			if (actions[i].down && actions[i].sym) queueKey(&actions[i], 0); // nothing stays held
			actions[i].down = 0;
		}
		if (stylus_pressed) {
			stylus_pressed = 0;
			queueStylus(SDL_MOUSEBUTTONUP);
		}
		runMenu();
	}
	else {
		for (int i=0; i<ACTION_COUNT; i++) {
			Action* action = &actions[i];
			int down = action->btn!=BTN_ID_NONE && PAD_isPressed(1<<action->btn) && (action->mod ? PAD_isPressed(BTN_MENU) : !PAD_isPressed(BTN_MENU));
			if (stick_dpad) {
				if (action->btn>=BTN_ID_ANALOG_UP && action->btn<=BTN_ID_ANALOG_RIGHT) down = 0;
				if (i<=3 && PAD_isPressed(1<<(BTN_ID_ANALOG_UP+i)) && !PAD_isPressed(BTN_MENU)) down = 1; // up, down, left, right
			}
			if (down==action->down) continue;
			action->down = down;
			if (down && action->mod) menu_used = 1;
			if (action->index) {
				if (action->sym) queueKey(action, down);
				continue;
			}
			if (!down) continue;
			switch (i) {
				case ACTION_NEXT_LAYOUT:
				case ACTION_PREV_LAYOUT:
					layout_index = (layout_index+(i==ACTION_NEXT_LAYOUT ? 1 : LAYOUT_COUNT-1))%LAYOUT_COUNT;
					border_dirty = 1;
					saveSettings();
					break;
				case ACTION_NEXT_SCREEN:
				case ACTION_PREV_SCREEN: // there are only two
				case ACTION_SWAP:
					swap_screens = !swap_screens;
					saveSettings();
					break;
				case ACTION_SAVE: {
					char path[800];
					snprintf(path, sizeof(path), "%s/%s.txt", minui_dir, rom_file);
					if (exists(path)) menu_slot = getInt(path);
					composeShot(shot_surface());
					saveToSlot(menu_slot, shot_surface());
					break;
				}
				case ACTION_LOAD: {
					char path[800];
					snprintf(path, sizeof(path), "%s/%s.txt", minui_dir, rom_file);
					if (exists(path)) menu_slot = getInt(path);
					loadFromSlot(menu_slot);
					break;
				}
				case ACTION_SWITCHER:
					saveForSwitcher();
					break;
			}
		}
		updateStylus();
	}
	if (!event_count) return 0;
	*event = events[event_next++];
	return 1;
}

///////////////////////////////
// what DraStic calls instead of ALSA: its sound goes where every other system's does

#define PCM_PLAYBACK ((void*)1)
#define PCM_CAPTURE ((void*)2)

static int sound_started;
static unsigned sound_rate = 44100;
static unsigned long sound_buffer = 8192; // the size it asks for

// DraStic times itself and its frameskip by how full the sound buffer is, so it has to look like a real one:
// what's written, less what would have played by now
static uint64_t sound_written, sound_clock;
static unsigned long soundQueued(void) {
	if (!sound_clock) return 0;
	uint64_t played = (nowUs()-sound_clock) * sound_rate / 1000000;
	if (played>=sound_written) { // ran dry, eg. in the menu: starts again from here
		sound_clock = 0;
		sound_written = 0;
		return 0;
	}
	return sound_written-played;
}

static int h_snd_pcm_open(void** pcm, const char* name, int stream, int mode) {
	*pcm = stream ? PCM_CAPTURE : PCM_PLAYBACK;
	return 0;
}
static int h_snd_pcm_ok() { return 0; }
static int h_snd_pcm_hw_params_malloc(void** params) { *params = calloc(1, 64); return 0; }
static void h_snd_pcm_params_free(void* params) { free(params); }
static int h_snd_pcm_hw_params_set_rate_near(void* pcm, void* params, unsigned* rate, int* dir) {
	sound_rate = *rate;
	return 0;
}
static int h_snd_pcm_hw_params_set_buffer_size_near(void* pcm, void* params, unsigned long* size) {
	sound_buffer = *size;
	return 0;
}
static long h_snd_pcm_avail(void* pcm) {
	if (pcm==PCM_CAPTURE) return sound_buffer;
	return sound_buffer-soundQueued();
}
static long h_snd_pcm_writei(void* pcm, const void* frames, unsigned long count) {
	if (!sound_started) { // once there's somewhere for it to go
		SND_init(sound_rate, 60.0);
		sound_started = 1;
	}
	uint64_t started = profile_on ? nowUs() : 0;
	while (sound_buffer-soundQueued()<count) SDL_Delay(1); // blocks until there's room, like ALSA
	if (!sound_clock) sound_clock = nowUs();
	sound_written += count;
	SND_batchSamples(frames, count);
	if (profile_on) profile_audio += nowUs()-started;
	return count;
}
static long h_snd_pcm_readi(void* pcm, void* frames, unsigned long count) { // no microphone
	memset(frames, 0, count*2);
	return count;
}
static const char* h_snd_strerror(int error) { return "none"; }

///////////////////////////////
// hooking up

#define HOOK(name) {#name, h_##name}
#define SND_OK(name) {#name, h_snd_pcm_ok}
static const struct { const char* name; void* function; } hooks[] = {
	HOOK(gettimeofday), HOOK(pthread_cond_wait), HOOK(SDL_Init), HOOK(SDL_Quit), HOOK(SDL_SetHint), HOOK(SDL_GetCurrentDisplayMode), HOOK(SDL_CreateWindow),
	HOOK(SDL_SetWindowFullscreen), HOOK(SDL_CreateRenderer), HOOK(SDL_RenderSetLogicalSize), HOOK(SDL_RenderGetLogicalSize),
	HOOK(SDL_CreateTexture), HOOK(SDL_DestroyTexture), HOOK(SDL_SetTextureBlendMode), HOOK(SDL_UpdateTexture),
	HOOK(SDL_SetRenderDrawBlendMode), HOOK(SDL_SetRenderDrawColor), HOOK(SDL_RenderClear), HOOK(SDL_RenderCopy),
	HOOK(SDL_RenderPresent), HOOK(SDL_Delay), HOOK(SDL_GetKeyName), HOOK(SDL_GetModState), HOOK(SDL_NumJoysticks),
	HOOK(SDL_JoystickOpen), HOOK(SDL_JoystickName), HOOK(SDL_JoystickEventState), HOOK(SDL_PollEvent),
	HOOK(snd_pcm_open), HOOK(snd_pcm_avail), HOOK(snd_pcm_writei), HOOK(snd_pcm_readi), HOOK(snd_strerror),
	HOOK(snd_pcm_hw_params_malloc), HOOK(snd_pcm_hw_params_set_rate_near),
	{"snd_pcm_sw_params_malloc", h_snd_pcm_hw_params_malloc},
	{"snd_pcm_hw_params_free", h_snd_pcm_params_free}, {"snd_pcm_sw_params_free", h_snd_pcm_params_free},
	SND_OK(snd_pcm_close), SND_OK(snd_pcm_hw_params), SND_OK(snd_pcm_hw_params_any), SND_OK(snd_pcm_hw_params_set_access),
	HOOK(snd_pcm_hw_params_set_buffer_size_near), SND_OK(snd_pcm_hw_params_set_channels), SND_OK(snd_pcm_hw_params_set_format),
	SND_OK(snd_pcm_hw_params_set_period_size_near), SND_OK(snd_pcm_prepare), SND_OK(snd_pcm_recover), SND_OK(snd_pcm_start),
	SND_OK(snd_pcm_sw_params), SND_OK(snd_pcm_sw_params_current),
};
#define HOOK_COUNT (int)(sizeof(hooks)/sizeof(hooks[0]))

static void* hookFor(const char* name) {
	for (int i=0; i<HOOK_COUNT; i++) if (!strcmp(hooks[i].name, name)) return hooks[i].function;
	return NULL;
}
static void writeSlot(uintptr_t address, void* value) {
	long page = sysconf(_SC_PAGESIZE);
	mprotect((void*)(address & ~(page-1)), page*2, PROT_READ|PROT_WRITE);
	*(void**)address = value;
}
static void hookDrastic(void) {
	int fd = open("/proc/self/exe", O_RDONLY);
	if (fd<0) return;
	struct stat info;
	fstat(fd, &info);
	char* file = mmap(NULL, info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (file==MAP_FAILED) return;

	Elf32_Ehdr* elf = (Elf32_Ehdr*)file;
	Elf32_Shdr* sections = (Elf32_Shdr*)(file + elf->e_shoff);
	for (int i=0; i<elf->e_shnum; i++) {
		Elf32_Shdr* section = &sections[i];
		if (section->sh_type==SHT_REL) { // its calls out
			Elf32_Shdr* symbols = &sections[section->sh_link];
			if (symbols->sh_type!=SHT_DYNSYM) continue;
			const char* names = file + sections[symbols->sh_link].sh_offset;
			Elf32_Rel* rel = (Elf32_Rel*)(file + section->sh_offset);
			int count = section->sh_size / sizeof(Elf32_Rel);
			for (int r=0; r<count; r++) {
				int index = ELF32_R_SYM(rel[r].r_info);
				if (!index) continue;
				Elf32_Sym* symbol = (Elf32_Sym*)(file + symbols->sh_offset) + index;
				void* hook = hookFor(names + symbol->st_name);
				if (hook) writeSlot(rel[r].r_offset, hook);
			}
		}
		else if (section->sh_type==SHT_SYMTAB) { // its own functions, when it wasn't stripped
			const char* names = file + sections[section->sh_link].sh_offset;
			Elf32_Sym* symbol = (Elf32_Sym*)(file + section->sh_offset);
			int count = section->sh_size / sizeof(Elf32_Sym);
			for (int s=0; s<count; s++) {
				const char* name = names + symbol[s].st_name;
				void* value = (void*)symbol[s].st_value;
				if (!strcmp(name, "nds_system")) nds_system = value;
				else if (!strcmp(name, "save_state_index")) nds_save = value;
				else if (!strcmp(name, "load_state_index")) nds_load = value;
				else if (!strcmp(name, "screen_copy16")) nds_copy_screen = value;
				else if (!strcmp(name, "quit")) nds_quit = value;
			}
		}
	}
	munmap(file, info.st_size);
}

static void readGameName(void) { // from the command line, as DraStic names its states
	int fd = open("/proc/self/cmdline", O_RDONLY);
	if (fd<0) return;
	char line[1024] = {0};
	int size = read(fd, line, sizeof(line)-1);
	close(fd);
	if (size<=0) return;
	char* rom = line + strlen(line) + 1;
	if (rom>=line+size) return;
	snprintf(rom_path, sizeof(rom_path), "%s", rom);
	char* slash = strrchr(rom, '/');
	snprintf(rom_file, sizeof(rom_file), "%s", slash ? slash+1 : rom);
	snprintf(game_name, sizeof(game_name), "%s", rom_file);
	char* dot = strrchr(game_name, '.');
	if (dot) *dot = '\0';
	char emu[256];
	getEmuName(rom_path, emu);
	snprintf(minui_dir, sizeof(minui_dir), "%s/.minui/%s", SHARED_USERDATA_PATH, emu);
	mkdir(SHARED_USERDATA_PATH "/.minui", 0755);
	mkdir(minui_dir, 0755);
}

static void adoptOldSaves(void) { // saves made before the launcher could see them: Resume goes to the newest
	char path[800];
	snprintf(path, sizeof(path), "%s/%s.txt", minui_dir, rom_file);
	if (exists(path)) return;
	int newest = -1;
	time_t newest_time = 0;
	for (int slot=0; slot<MENU_SLOT_COUNT; slot++) {
		char state[800];
		struct stat info;
		slotPath(slot, state, sizeof(state));
		if (stat(state, &info)!=0 || (newest!=-1 && info.st_mtime<newest_time)) continue;
		newest = slot;
		newest_time = info.st_mtime;
	}
	if (newest!=-1) putInt(path, newest);
}

__attribute__((constructor)) static void layerInit(void) {
	char exe[512] = {0};
	if (readlink("/proc/self/exe", exe, sizeof(exe)-1)<=0) return;
	char* name = strrchr(exe, '/');
	if (!name || strcmp(name+1, "drastic")) return; // only DraStic, not anything it runs
	unsetenv("LD_PRELOAD");
	snprintf(states_dir, sizeof(states_dir), "%s/savestates", getenv("HOME"));
	readGameName();
	adoptOldSaves();
	readDrasticControls();
	for (int i=0; i<ACTION_COUNT; i++) {
		actions[i].default_btn = actions[i].btn;
		actions[i].default_mod = actions[i].mod;
	}
	loadSettings();
	applyEmuOptions();
	hookDrastic();
}
