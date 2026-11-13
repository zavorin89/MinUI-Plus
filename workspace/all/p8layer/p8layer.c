// Makes the official PICO-8 (a closed program each owner brings, never in releases) act like MinUI+'s other
// systems: the A30's buttons, the launcher's volume, MENU for a game menu (screen effect, scaling, border, buttons,
// quit), the power button's sleep, play time, and the notifications for volume and brightness.
//
// PICO-8 opens its own window and reads the keyboard and joysticks through SDL. On the A30 neither works well: the
// panel is on its side, the buttons are a keyboard PICO-8 doesn't expect, and opening sound puts the volume back to
// full. launch.sh loads this with LD_PRELOAD; it points PICO-8's own calls to SDL at the functions here by rewriting
// pico8_dyn's relocation slots (as ndslayer does for DraStic), so this library and MinUI's platform code still reach
// the real SDL. PICO-8 then draws its 128x128 screen into a texture, and this puts that on the panel itself.

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

#include <msettings.h>
#include <SDL2/SDL_image.h>

#include "defines.h"
#include "api.h"
#include "utils.h"

extern SDL_Window* PLAT_getWindow(void);
extern int PLAT_getRotate(void);
extern void PLAT_drawNotification(void);

#define P8_SIZE 128 // PICO-8's screen
#define POLL_MS 5
// what SDL_Init isn't asked for: sensors aren't in the A30's SDL2 (asking fails SDL_Init outright), and the buttons
// come from MinUI, so no joysticks, game controllers or rumble either
#define SDL_INIT_SENSOR_FLAG 0x00008000u
#define SDL_INIT_UNWANTED (SDL_INIT_SENSOR_FLAG | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC)

static char rom_path[512];
static char rom_name[256];
static char settings_path[512];

static SDL_Renderer* renderer;
static SDL_Window* window;
static SDL_Surface* screen;
static int rotate;
static int video_ready;
static int settings_ready;
static int in_menu;
static int quit_requested;
static uint32_t quit_requested_at;

///////////////////////////////
// settings, in PICO-8's own home (Saves/PICO8)

enum { LOOK_NONE, LOOK_LINE, LOOK_GRID, LOOK_COUNT }; // (the order the other systems list them in)
enum { SCALING_INTEGER, SCALING_FULL, SCALING_COUNT };
static int effect = LOOK_GRID;
static int scaling = SCALING_FULL;
static int border_on = 1;
static int swap_ab = 0; // A is O (Z) and B is X, unless swapped

static char* scaling_labels[] = {"Integer","Fullscreen",NULL};
static char* effect_labels[] = {"None","Line","Grid",NULL};
static char* off_on[] = {"Off","On",NULL};
static char* layout_labels[] = {"A is O, B is X","A is X, B is O",NULL};
// minuiplus.txt is "key = value" like minarch's .cfg files, so the launcher's game menu (MENU + Y) can show and
// change the same settings; the values are the menus' labels
static int settingValue(const char* line, const char* key, char** labels, int count, int* value) {
	size_t len = strlen(key);
	if (strncmp(line, key, len) || strncmp(line+len, " = ", 3)) return 0;
	const char* text = line + len + 3;
	for (int i=0; i<count; i++) {
		size_t n = strlen(labels[i]);
		if (!strncmp(text, labels[i], n) && (text[n]=='\n' || text[n]=='\r' || text[n]=='\0')) { *value = i; return 1; }
	}
	return 0;
}
static void loadSettings(void) {
	FILE* file = fopen(settings_path, "r");
	if (!file) return;
	char line[128];
	while (fgets(line, sizeof(line), file)) {
		int value;
		if (settingValue(line, "p8_scaling", scaling_labels, SCALING_COUNT, &value)) scaling = value;
		else if (settingValue(line, "p8_effect", effect_labels, LOOK_COUNT, &value)) effect = value;
		else if (settingValue(line, "p8_border", off_on, 2, &value)) border_on = value;
		else if (settingValue(line, "p8_buttons", layout_labels, 2, &value)) swap_ab = value;
	}
	fclose(file);
}
static void saveSettings(void) { // swapped in whole, so a power cut can't leave them half written
	char tmp[sizeof(settings_path)+8];
	snprintf(tmp, sizeof(tmp), "%s.tmp", settings_path);
	FILE* file = fopen(tmp, "w");
	if (!file) return;
	fprintf(file, "p8_scaling = %s\np8_effect = %s\np8_border = %s\np8_buttons = %s\n",
		scaling_labels[scaling], effect_labels[effect], off_on[border_on], layout_labels[swap_ab]);
	int ok = fflush(file)==0 && !ferror(file) && fsync(fileno(file))==0;
	fclose(file);
	if (ok) rename(tmp, settings_path);
	else unlink(tmp);
}
static void ensureSettings(void) { // the volume can be wanted before the window
	if (settings_ready) return;
	InitSettings();
	settings_ready = 1;
}
static void applyVolume(void) { // opening the sound puts the A30 back at full volume
	ensureSettings();
	SetVolume(GetVolume());
}

///////////////////////////////
// play time, the same file and sums as every other system

static struct {
	char path[512];
	PlayTime saved;
	int session_base;
	uint32_t session_ms;
	uint32_t unsaved_ms;
	uint32_t last_tick;
} play;

static void Play_init(void) {
	if (!rom_path[0]) return;
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
	play.last_tick = SDL_GetTicks();
}
static int Play_getTotal(void) { return play.saved.seconds + play.session_ms / 1000; }
static int Play_getSession(void) { return play.session_base + play.session_ms / 1000; }
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
	if (delta<500) { // longer gaps were the menu or sleep
		play.session_ms += delta;
		play.unsaved_ms += delta;
	}
	if (play.unsaved_ms>=60000) Play_save(); // so a dead battery loses at most a minute
}

///////////////////////////////
// sound: PICO-8 keeps its own, through SDL, kept at the launcher's volume. Its callback is wrapped so the menu sounds
// and the volume clicks mix in; the device keeps running in the menu (silent but for them) and stops for sleep

static int audio_open;
static int audio_running; // what PICO-8 last asked for
static int audio_asleep;
static SDL_AudioCallback game_callback;
static void* game_userdata;
static void soundCallback(void* userdata, Uint8* stream, int len) {
	if (in_menu || !audio_running || !game_callback) memset(stream, 0, len);
	else game_callback(game_userdata, stream, len);
	SFX_mixInto((int16_t*)stream, len / (int)sizeof(int16_t) / MAX(1, SFX_deviceChannels()));
#ifdef P8LAYER_TEST
	if (in_menu) { // a menu sound made it into PICO-8's output
		int16_t* samples = (int16_t*)stream;
		for (int i=0; i<len/2; i++) if (samples[i]) { fprintf(stderr, "p8layer: menu sound in the output\n"); break; }
	}
#endif
}
static void pauseSound(int paused) { // for sleep
	audio_asleep = paused;
	if (audio_open) SDL_PauseAudio(paused);
}
static int h_SDL_OpenAudio(SDL_AudioSpec* desired, SDL_AudioSpec* obtained) {
	game_callback = desired->callback;
	game_userdata = desired->userdata;
	if (game_callback) {
		desired->callback = soundCallback;
		desired->userdata = NULL;
	}
	int result = SDL_OpenAudio(desired, obtained);
	if (game_callback) {
		desired->callback = game_callback;
		desired->userdata = game_userdata;
	}
	if (result==0) {
		audio_open = 1;
		SDL_AudioSpec* spec = obtained ? obtained : desired; // (without obtained, SDL converts to what was asked for)
		if (game_callback && spec->format==AUDIO_S16SYS) SFX_useDevice(spec->freq, spec->channels);
		applyVolume();
	}
	return result;
}
static void h_SDL_PauseAudio(int paused) {
	audio_running = !paused;
	if (audio_open && !audio_asleep) SDL_PauseAudio(0); // (silent while PICO-8 has it paused, see soundCallback)
}

static Uint8 key_state[SDL_NUM_SCANCODES]; // what PICO-8 reads as the keyboard (see the buttons, below)

///////////////////////////////
// test scripting, only in a build for the harness

#ifdef P8LAYER_TEST
static int test_polls;
static int test_counts[8]; // updates, presents, keyboard state reads, poll calls
#define TEST_COUNT(i) (test_counts[i]++)
static void testPad(void) {
	test_polls++;
	if (test_polls%500==0) fprintf(stderr, "p8layer: state z %i x %i right %i\n", key_state[29], key_state[27], key_state[79]);
	if (test_polls%100==0) fprintf(stderr, "p8layer: poll %i: %i surface updates, %i presents, %i keyboard reads, %i poll calls, %i texture copies\n", test_polls, test_counts[0], test_counts[1], test_counts[2], test_counts[3], test_counts[4]);
	const char* script = getenv("P8LAYER_PRESS"); // poll:button id,... (100 and up: held 30 polls, 200 and up 600)
	for (const char* s=script; s && *s;) {
		int at, id;
		if (sscanf(s, "%i:%i", &at, &id)!=2) break;
		int held = id>=200 ? 3000 : id>=100 ? 30 : 1; // 200 and up: 3000 polls (PICO-8 polls unevenly under qemu)
		id %= 100; // (both)
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
static void testRawKeys(void) { // P8LAYER_RAW=poll:scancode,...: that key alone, held 400 polls
	const char* script = getenv("P8LAYER_RAW");
	for (const char* s=script; s && *s;) {
		int at, code;
		if (sscanf(s, "%i:%i", &at, &code)!=2) break;
		if (code>0 && code<512) {
			if (test_polls==at) { key_state[code] = 1; fprintf(stderr, "p8layer: raw %i down\n", code); }
			if (test_polls==at+400) key_state[code] = 0;
		}
		s = strchr(s, ',');
		if (s) s++;
	}
}
static void testShot(const char* tag, void* pixels, int w, int h, int pitch) {
	static int count;
	char path[128];
	snprintf(path, sizeof(path), "/s/p8/shot_%s_%04i.ppm", tag, count++);
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
static void testPresent(void) {
	static int presents;
	const char* every = getenv("P8LAYER_SHOTS");
	if (!every || ++presents%atoi(every)) return;
	static uint16_t pixels[FIXED_WIDTH*FIXED_WIDTH];
	SDL_Rect size;
	SDL_RenderGetViewport(renderer, &size);
	SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGB565, pixels, size.w*2);
	testShot("game", pixels, size.w, size.h, size.w*2);
}
#else
#define TEST_COUNT(i)
#define testPad()
#define testPresent()
#define testShot(tag, pixels, w, h, pitch)
#endif

///////////////////////////////
// the picture: PICO-8's screen texture, placed and dressed here

static SDL_Texture* game_texture; // the last one it drew with
static SDL_Rect game_src = {0, 0, P8_SIZE, P8_SIZE};
static SDL_Texture* game_tracked; // the 128x128 texture, whose pixels are kept for the menu's picture
static Uint32 game_format;
static int game_bpp;
static uint8_t game_pixels[P8_SIZE*P8_SIZE*4];
static int game_pixels_ready;

static SDL_Texture* border_texture;
static SDL_Surface* border_flat; // the same, for the menu
static int border_dirty = 1;
static void loadBorder(void) {
	border_dirty = 0;
	if (border_texture) SDL_DestroyTexture(border_texture);
	if (border_flat) SDL_FreeSurface(border_flat);
	border_texture = NULL;
	border_flat = NULL;
	if (!border_on) return;
	// a cart's own (Bezels/P8/<cart>.png) before the system's, as minarch looks for them
	const char* tags[] = {"PICO8", "P8"};
	char path[800] = "";
	for (int i=0; i<2 && !path[0]; i++) {
		snprintf(path, sizeof(path), "%s/%s/%s.png", BEZELS_PATH, tags[i], rom_name);
		if (!exists(path)) snprintf(path, sizeof(path), "%s/%s.png", BEZELS_PATH, tags[i]);
		if (!exists(path)) path[0] = '\0';
	}
	if (!path[0]) return;
	SDL_Surface* image = IMG_Load(path);
	if (!image) return;
	// flattened on black so it's drawn without blending: the game goes over its hole
	SDL_Surface* flat = SDL_CreateRGBSurface(SDL_SWSURFACE, FIXED_WIDTH, FIXED_HEIGHT, FIXED_DEPTH, RGBA_MASK_565);
	if (flat) {
		SDL_FillRect(flat, NULL, 0);
		SDL_BlitScaled(image, NULL, flat, NULL);
		border_texture = SDL_CreateTextureFromSurface(renderer, flat);
		if (border_texture) SDL_SetTextureBlendMode(border_texture, SDL_BLENDMODE_NONE);
		border_flat = flat;
	}
	SDL_FreeSurface(image);
}

static SDL_Rect gameRect(void) { // in landscape
	int size = scaling==SCALING_FULL ? FIXED_HEIGHT : (FIXED_HEIGHT / P8_SIZE) * P8_SIZE;
	return (SDL_Rect){(FIXED_WIDTH-size)/2, (FIXED_HEIGHT-size)/2, size, size};
}

static void drawTexture(SDL_Texture* texture, const SDL_Rect* src, SDL_Rect rect) {
	// the panel is on its side, so the centre turns with it
	int cx = rect.x + rect.w/2;
	int cy = rect.y + rect.h/2;
	int angle = 0;
	if (rotate) {
		int turned = cx;
		cx = cy;
		cy = FIXED_WIDTH - turned;
		angle = rotate*90;
	}
	SDL_Rect out = {cx-rect.w/2, cy-rect.h/2, rect.w, rect.h};
	SDL_RenderCopyEx(renderer, texture, src, &out, angle%360, NULL, SDL_FLIP_NONE);
}

// an LCD up close, like ndslayer's for the DS: worked out once for the size it's drawn at, into a texture that
// multiplies the game under it. Grid: a soft gap around every PICO-8 pixel, with a faint red, green and blue stripe
// inside it. Line: the gap between rows only
#define LCD_STRIPE 1.5f
#define LCD_CELL 0.8f
#define LCD_TINT 0.2f
#define LCD_STRIPE_MEAN 0.533f
static float lcdBump(float distance, float reach) {
	float x = distance/reach;
	if (x<=-1 || x>=1) return 0;
	x = 1 - x*x;
	return x*x;
}
static float lcdCover(float from, float to, float center, float reach, float scale) {
	float sum = 0;
	for (int i=0; i<8; i++) {
		float at = from + (to-from)*(i+0.5f)/8;
		float place = at - floorf(at);
		for (int k=-1; k<=1; k++) sum += lcdBump((place-center-k)*scale, reach);
	}
	return sum/8;
}
static uint8_t lcdByte(float light) {
	if (light>1) light = 1;
	return powf(light, 1/2.2f) * 255;
}
static SDL_Texture* effectTexture(int kind, int size) {
	static SDL_Texture* texture;
	static int made_kind = -1, made_size;
	if (kind==LOOK_NONE) return NULL;
	if (texture && made_kind==kind && made_size==size) return texture;
	if (texture) SDL_DestroyTexture(texture);
	texture = NULL;
	made_kind = kind;
	made_size = size;
	uint32_t* pixels = malloc(size*size*sizeof(uint32_t));
	float (*across)[3] = malloc(size*sizeof(*across));
	float* down = malloc(size*sizeof(float));
	if (pixels && across && down) texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, size, size);
	if (texture) {
		float step = (float)P8_SIZE / size;
		for (int i=0; i<size; i++) {
			float from = i*step, to = (i+1)*step;
			down[i] = lcdCover(from, to, 0.5f, LCD_CELL, 1);
			float cell = kind==LOOK_GRID ? lcdCover(from, to, 0.5f, LCD_CELL, 1) : 1;
			for (int c=0; c<3; c++) {
				float stripe = kind==LOOK_GRID ? lcdCover(from, to, (c+0.5f)/3, LCD_STRIPE, 3) / LCD_STRIPE_MEAN : 1;
				across[i][c] = cell * (1 - LCD_TINT + LCD_TINT*stripe);
				if (kind!=LOOK_GRID) across[i][c] = 1;
			}
		}
		for (int y=0; y<size; y++) {
			for (int x=0; x<size; x++) {
				pixels[y*size+x] = 0xFF000000 | lcdByte(across[x][0]*down[y])<<16 | lcdByte(across[x][1]*down[y])<<8 | lcdByte(across[x][2]*down[y]);
			}
		}
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_MOD);
		SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
		SDL_UpdateTexture(texture, NULL, pixels, size*sizeof(uint32_t));
	}
	free(pixels);
	free(across);
	free(down);
	return texture;
}

static void presentGame(void) {
	TEST_COUNT(1);
	if (border_dirty) loadBorder();
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	if (border_texture) drawTexture(border_texture, NULL, (SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});
	SDL_Rect rect = gameRect();
	if (game_texture) {
		SDL_SetTextureScaleMode(game_texture, SDL_ScaleModeNearest);
		SDL_SetTextureBlendMode(game_texture, SDL_BLENDMODE_NONE);
		drawTexture(game_texture, &game_src, rect);
		SDL_Texture* lcd = effectTexture(effect, rect.w);
		if (lcd) drawTexture(lcd, NULL, rect);
	}
	PLAT_drawNotification();
	testPresent();
	SDL_RenderPresent(renderer);
}

///////////////////////////////
// the game menu, drawn the way the other systems' are

#define MENU_ROW_SIZE 25
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
			int inside = 0;
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
	int w = rect->w > h ? rect->w - h : 0;
	SDL_BlitSurface(ends, &(SDL_Rect){0,0,r,h}, dst, &(SDL_Rect){rect->x,rect->y});
	if (w>0) SDL_FillRect(dst, &(SDL_Rect){rect->x+r,rect->y,w,h}, color);
	SDL_BlitSurface(ends, &(SDL_Rect){r,0,h-r,h}, dst, &(SDL_Rect){rect->x+r+w,rect->y});
	SDL_FreeSurface(ends);
}

static void composeBackdrop(SDL_Surface* dst) { // the game as it looks, without the effect
	if (!dst) return;
	if (border_dirty) loadBorder();
	SDL_FillRect(dst, NULL, 0);
	if (border_flat) SDL_BlitSurface(border_flat, NULL, dst, NULL);
	if (!game_pixels_ready) return;
	SDL_Surface* frame = SDL_CreateRGBSurfaceWithFormatFrom(game_pixels, P8_SIZE, P8_SIZE, game_bpp*8, P8_SIZE*game_bpp, game_format);
	if (!frame) return;
	SDL_Rect rect = gameRect();
	SDL_BlitScaled(frame, NULL, dst, &rect);
	SDL_FreeSurface(frame);
}

// the game switcher (MENU held, then let go): PICO-8 has no save states, so it gets the picture alone, where the
// switcher shows it, and picking it there starts the cart (or Splore) over
static void saveShot(SDL_Surface* shot, const char* path) {
	if (shot->w!=FIXED_WIDTH || shot->h!=FIXED_HEIGHT || shot->format->BitsPerPixel!=16) {
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
static void releaseAll(void);
static void queueQuit(void);
static void saveForSwitcher(void) {
	const char* rel = prefixMatch(SDCARD_PATH, rom_path) ? rom_path + strlen(SDCARD_PATH) : rom_path;
	char folder[512]; // the system's tag, from "Roms/Name (TAG)/cart", as the launcher looks it up
	snprintf(folder, sizeof(folder), "%s", rom_path);
	char* slash = strrchr(folder, '/');
	if (slash) *slash = '\0';
	char* open = strrchr(folder, '(');
	char* close = open ? strchr(open, ')') : NULL;
	if (rom_path[0] && open && close) {
		*close = '\0';
		char path[1024];
		snprintf(path, sizeof(path), SHARED_USERDATA_PATH "/.minui/%s", open+1);
		mkdir(path, 0755);
		snprintf(path + strlen(path), sizeof(path) - strlen(path), "/%s.%d.bmp", strrchr(rom_path, '/') + 1, AUTO_RESUME_SLOT);
		SDL_Surface* shot = SDL_CreateRGBSurface(SDL_SWSURFACE, FIXED_WIDTH, FIXED_HEIGHT, FIXED_DEPTH, RGBA_MASK_565);
		if (shot) {
			composeBackdrop(shot);
			saveShot(shot, path);
			SDL_FreeSurface(shot);
		}
		Switcher_add((char*)rel); // so there's a way back
		putFile(SWITCHER_OPEN_PATH, "1");
	}
	releaseAll();
	Play_save();
	sync();
	quit_requested = 1;
	quit_requested_at = SDL_GetTicks();
	queueQuit();
}

static void beforeSleep(void) {
	pauseSound(1);
	Play_save();
	sync();
}
static void afterSleep(void) {
	applyVolume();
	pauseSound(0);
	play.last_tick = SDL_GetTicks();
}

// Options, laid out like every other system's: lists of settings with their values on the right
#define OPTION_PADDING 8
#define SCROLL_WIDTH 24
#define SCROLL_HEIGHT 4
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
enum { MENU_LIST, MENU_VAR };
struct MenuList {
	int type;
	int max_width;
	char* title;
	char* desc;
	MenuItem* items;
	MenuList_callback_t on_confirm;
	MenuList_callback_t on_change;
};

static int Menu_options(MenuList* list) { // ndslayer's (and so minarch's), for the two kinds of list used here
	MenuItem* items = list->items;
	int type = list->type;
	int dirty = 1;
	int show_options = 1;
	int show_settings = 0;
	int max_visible_options = (screen->h - ((SCALE1(PADDING + PILL_SIZE) * 2) + SCALE1(BUTTON_SIZE))) / SCALE1(BUTTON_SIZE);
	int count;
	for (count=0; items[count].name; count++);
	int selected = 0;
	int start = 0;
	int end = MIN(count,max_visible_options);
	int visible_rows = end;

	while (show_options) {
		GFX_startFrame();
		PAD_poll();
		testPad();

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
		}
		else if (PAD_justRepeated(BTN_DOWN)) {
			selected += 1;
			if (selected>=count) { selected = 0; start = 0; end = visible_rows; }
			else if (selected>=end) { start += 1; end += 1; }
			dirty = 1;
		}
		else if (items[selected].values) {
			MenuItem* item = &items[selected];
			int change = PAD_justRepeated(BTN_LEFT) ? -1 : PAD_justRepeated(BTN_RIGHT) ? 1 : 0;
			if (change) {
				int total;
				for (total=0; item->values[total]; total++);
				item->value = (item->value+change+total)%total;
				if (item->on_change) item->on_change(list, selected);
				else if (list->on_change) list->on_change(list, selected);
				dirty = 1;
			}
		}

		if (PAD_justPressed(BTN_B)) show_options = 0;
		else if (PAD_justPressed(BTN_A)) {
			MenuItem* item = &items[selected];
			int result = MENU_CALLBACK_NOP;
			if (item->on_confirm) result = item->on_confirm(list, selected);
			else if (list->on_confirm) result = list->on_confirm(list, selected);
			if (result==MENU_CALLBACK_EXIT) show_options = 0;
			else dirty = 1;
		}

		PWR_update(&dirty, &show_settings, beforeSleep, afterSleep);

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
				for (int i=start,j=0; i<end; i++,j++) {
					MenuItem* item = &items[i];
					SDL_Color text_color = COLOR_WHITE;
					if (i==selected) {
						int w = 0;
						TTF_SizeUTF8(font.small, item->name, &w, NULL);
						w += SCALE1(OPTION_PADDING*2);
						GFX_blitPill(ASSET_BUTTON, screen, &(SDL_Rect){ox, oy+SCALE1(j*BUTTON_SIZE), w, SCALE1(BUTTON_SIZE)});
						text_color = COLOR_BLACK;
						if (item->desc) desc = item->desc;
					}
					text = TTF_RenderUTF8_Blended(font.small, item->name, text_color);
					if (text) {
						SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ox+SCALE1(OPTION_PADDING), oy+SCALE1((j*BUTTON_SIZE)+1)});
						SDL_FreeSurface(text);
					}
				}
			}
			else {
				int mw = list->max_width;
				if (!mw) {
					for (int i=0; i<count; i++) {
						MenuItem* item = &items[i];
						int w = 0, lw = 0, rw = 0;
						TTF_SizeUTF8(font.small, item->name, &lw, NULL);
						for (int v=0; item->values[v]; v++) {
							TTF_SizeUTF8(font.tiny, item->values[v], &rw, NULL);
							if (lw+rw>w) w = lw+rw;
						}
						w += SCALE1(OPTION_PADDING*4);
						if (w>mw) mw = w;
					}
					list->max_width = mw = MIN(mw, screen->w - SCALE1(PADDING*2));
				}
				int ox = (screen->w - mw) / 2;
				int oy = SCALE1(PADDING + PILL_SIZE);
				for (int i=start,j=0; i<end; i++,j++) {
					MenuItem* item = &items[i];
					SDL_Color text_color = COLOR_WHITE;
					if (i==selected) {
						GFX_blitPill(ASSET_OPTION, screen, &(SDL_Rect){ox, oy+SCALE1(j*BUTTON_SIZE), mw, SCALE1(BUTTON_SIZE)});
						int w = 0;
						TTF_SizeUTF8(font.small, item->name, &w, NULL);
						w += SCALE1(OPTION_PADDING*2);
						GFX_blitPill(ASSET_BUTTON, screen, &(SDL_Rect){ox, oy+SCALE1(j*BUTTON_SIZE), w, SCALE1(BUTTON_SIZE)});
						text_color = COLOR_BLACK;
						if (item->desc) desc = item->desc;
					}
					text = TTF_RenderUTF8_Blended(font.small, item->name, text_color);
					if (text) {
						SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ox+SCALE1(OPTION_PADDING), oy+SCALE1((j*BUTTON_SIZE)+1)});
						SDL_FreeSurface(text);
					}
					if (item->value>=0) {
						text = TTF_RenderUTF8_Blended(font.tiny, item->values[item->value], COLOR_WHITE);
						if (text) {
							SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ox + mw - text->w - SCALE1(OPTION_PADDING), oy+SCALE1((j*BUTTON_SIZE)+3)});
							SDL_FreeSurface(text);
						}
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
				GFX_blitText(font.tiny, desc, SCALE1(12), COLOR_WHITE, screen, &(SDL_Rect){(screen->w - w) / 2, y, w, h});
			}
			testShot("options", screen->pixels, screen->w, screen->h, screen->pitch);
			GFX_flip(screen);
			dirty = header_scrolls; // (a long title keeps scrolling)
		}
		else GFX_sync();
	}
	return MENU_CALLBACK_NOP;
}


static int OptionFrontend_onChange(MenuList* list, int i) {
	scaling = list->items[0].value;
	effect = list->items[1].value;
	if (border_on!=list->items[2].value) border_dirty = 1;
	border_on = list->items[2].value;
	saveSettings();
	return MENU_CALLBACK_NOP;
}
static MenuItem frontend_items[] = {
	{"Screen Scaling", "Integer keeps every pixel the same size (3x).\nFullscreen fills the height, a little uneven.", scaling_labels},
	{"Screen Effect", "Grid draws an LCD's pixel grid.\nLine simulates CRT scanlines.", effect_labels},
	{"Screen Bezel", "The bezel from Bezels around the screen:\nP8.png, or one named after the cart.", off_on},
	{NULL},
};
static MenuList OptionFrontend_menu = {.type=MENU_VAR, .title="Frontend Options", .desc="Kept for every cart.", .items=frontend_items, .on_change=OptionFrontend_onChange};
static int OptionFrontend_openMenu(MenuList* list, int i) {
	frontend_items[0].value = scaling;
	frontend_items[1].value = effect;
	frontend_items[2].value = border_on;
	Menu_options(&OptionFrontend_menu);
	return MENU_CALLBACK_NOP;
}

static int OptionControls_onChange(MenuList* list, int i) {
	swap_ab = list->items[0].value;
	saveSettings();
	return MENU_CALLBACK_NOP;
}
static MenuItem controls_items[] = {
	{"Button Layout", "X works as A, and Y as B. START is PICO-8's\npause menu; the d-pad and stick both move.", layout_labels},
	{NULL},
};
static MenuList OptionControls_menu = {.type=MENU_VAR, .title="Controls", .items=controls_items, .on_change=OptionControls_onChange};
static int OptionControls_openMenu(MenuList* list, int i) {
	controls_items[0].value = swap_ab;
	Menu_options(&OptionControls_menu);
	return MENU_CALLBACK_NOP;
}

static MenuList options_menu = {
	.type = MENU_LIST,
	.title = "Options",
	.items = (MenuItem[]){
		{"Frontend", "MinUI (PICO-8 layer)", .on_confirm=OptionFrontend_openMenu},
		{"Controls", .on_confirm=OptionControls_openMenu},
		{NULL},
	},
};

enum { ITEM_CONTINUE, ITEM_OPTIONS, ITEM_QUIT, ITEM_COUNT }; // (no Save or Load: PICO-8 has no save states)
static char* menu_items[ITEM_COUNT] = {"Continue", "Options", "Quit"};

static void runMenu(void) {
	GFX_clearSettingOverGame(); // the menu draws its own
	in_menu = 1; // (the game goes quiet, see soundCallback)
	SFX_forInput(1); // the menu clicks when moving, choosing and going back, as every other system's does
	Play_save();

	SDL_Surface* backing = SDL_CreateRGBSurface(SDL_SWSURFACE, FIXED_WIDTH, FIXED_HEIGHT, FIXED_DEPTH, RGBA_MASK_565);
	SDL_Surface* overlay = SDL_CreateRGBSurface(SDL_SWSURFACE, FIXED_WIDTH, FIXED_HEIGHT, FIXED_DEPTH, RGBA_MASK_565);
	SDL_SetSurfaceBlendMode(overlay, SDL_BLENDMODE_BLEND);
	SDL_SetSurfaceAlphaMod(overlay, 0xB8);
	SDL_FillRect(overlay, NULL, 0);
	composeBackdrop(backing);

	GFX_setVsync(VSYNC_STRICT);
	PWR_warn(0);
	PWR_enableAutosleep();
	PAD_reset();

	int selected = 0;
	int dirty = 1, show_setting = 0, show_menu = 1;
	int title_scrolls = 0, title_width = 0;
	// X pins the cart (or Splore) to the game switcher, as on every other system
	#define P8_MESSAGE_FADE_MS 150
	char* pin_path = rom_path[0] ? (prefixMatch(SDCARD_PATH, rom_path) ? rom_path + strlen(SDCARD_PATH) : rom_path) : NULL;
	int pinned = pin_path && Switcher_has(pin_path);
	char* menu_message = NULL;
	uint32_t menu_message_until = 0;
	while (show_menu) {
		GFX_startFrame();
		uint32_t now = SDL_GetTicks();
		PAD_poll();
		testPad();
		if (menu_message_until && now>=menu_message_until) {
			menu_message_until = 0;
			dirty = 1;
		}
		else if (menu_message_until && menu_message_until-now<P8_MESSAGE_FADE_MS) dirty = 1; // fading out

		if (PAD_justPressed(BTN_UP)) { // a press at a time, like minarch's menu
			selected = (selected + ITEM_COUNT - 1) % ITEM_COUNT;
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_DOWN)) {
			selected = (selected + 1) % ITEM_COUNT;
			dirty = 1;
		}

		if (PAD_justPressed(BTN_B) || PAD_tappedMenu(now)) show_menu = 0;
		else if (pin_path && PAD_justPressed(BTN_X)) {
			SFX_play(SFX_CONFIRM);
			if (pinned) {
				Switcher_remove(pin_path);
				menu_message = "Unpinned from game switcher";
			}
			else if (Switcher_add(pin_path)==-1) menu_message = "Game switcher is full (10)";
			else menu_message = "Pinned to game switcher";
			pinned = Switcher_has(pin_path);
			menu_message_until = now + 1500;
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_A)) {
			switch (selected) {
				case ITEM_CONTINUE:
					show_menu = 0;
					break;
				case ITEM_OPTIONS:
					Menu_options(&options_menu);
					composeBackdrop(backing); // the scaling or bezel may have changed
					dirty = 1;
					break;
				case ITEM_QUIT:
					show_menu = 0;
					quit_requested = 1;
					quit_requested_at = now;
					break;
			}
		}
		if (!show_menu) break;

		PWR_update(&dirty, &show_setting, beforeSleep, afterSleep);

		if (dirty) {
			GFX_clear(screen);
			SDL_BlitSurface(backing, NULL, screen, NULL);
			SDL_BlitSurface(overlay, NULL, screen, NULL);

			int ow = GFX_blitHardwareGroup(screen, show_setting);
			int max_width = screen->w - SCALE1(PADDING * 2 + BUTTON_MARGIN) - ow;
			title_scrolls = GFX_blitTitle(screen, rom_name, max_width);
			title_width = max_width;

			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			else GFX_blitButtonGroup((char*[]){BTN_SLEEP==BTN_POWER?"POWER":"MENU","SLEEP", pin_path ? "X" : NULL, pinned ? "UNPIN" : "PIN", NULL}, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){"B","BACK", "A","OKAY", NULL}, 1, screen, 1);

			char** items = menu_items;

			int text_oy = (SCALE1(MENU_ROW_SIZE) - TTF_FontHeight(font.medium)) / 2;
			int oy = (((FIXED_HEIGHT / FIXED_SCALE) - PADDING * 2) - (ITEM_COUNT * MENU_ROW_SIZE)) / 2;
			SDL_Surface* text;
			for (int i=0; i<ITEM_COUNT; i++) {
				SDL_Color text_color = COLOR_WHITE;
				if (i==selected) {
					int tw;
					TTF_SizeUTF8(font.medium, items[i], &tw, NULL);
					tw += SCALE1(BUTTON_PADDING*2);
					Menu_blitPill(RGB_WHITE, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(oy + PADDING + (i * MENU_ROW_SIZE)), tw, SCALE1(MENU_ROW_SIZE)});
					text_color = COLOR_BLACK;
				}
				else {
					text = TTF_RenderUTF8_Blended(font.medium, items[i], COLOR_BLACK);
					if (text) {
						SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){SCALE1(2 + PADDING + BUTTON_PADDING), SCALE1(1 + PADDING + oy + (i * MENU_ROW_SIZE)) + text_oy});
						SDL_FreeSurface(text);
					}
				}
				text = TTF_RenderUTF8_Blended(font.medium, items[i], text_color);
				if (text) {
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), SCALE1(oy + PADDING + (i * MENU_ROW_SIZE)) + text_oy});
					SDL_FreeSurface(text);
				}
			}

			if (play.path[0]) { // the play time, as the other systems show it
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
				int ox = FIXED_WIDTH - pw - SCALE1(PADDING);
				int py = (FIXED_HEIGHT - ph) / 2;
				GFX_blitRect(ASSET_BLACK_PILL, screen, &(SDL_Rect){ox,py,pw,ph});
				int stats_oy = (SCALE1(STATS_ROW_SIZE) - TTF_FontHeight(font.small)) / 2;
				for (int i=0; i<STATS_ROWS; i++) {
					int ty = py + SCALE1(BUTTON_PADDING + i * STATS_ROW_SIZE) + stats_oy;
					text = TTF_RenderUTF8_Blended(font.small, labels[i], COLOR_DARK_TEXT);
					if (text) {
						SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ox + SCALE1(BUTTON_PADDING), ty});
						SDL_FreeSurface(text);
					}
					text = TTF_RenderUTF8_Blended(font.small, values[i], COLOR_WHITE);
					if (text) {
						SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ox + pw - SCALE1(BUTTON_PADDING) - text->w, ty});
						SDL_FreeSurface(text);
					}
				}
			}
			if (menu_message_until && (text = TTF_RenderUTF8_Blended(font.large, menu_message, COLOR_WHITE))) { // the pin confirmation, drawn and faded as minarch's is
				int pw = text->w + SCALE1(BUTTON_PADDING*4);
				int ph = text->h + SCALE1(BUTTON_PADDING*2);
				SDL_Rect box = { (screen->w-pw)/2, (screen->h-ph)/2, pw, ph };
				int left = (int32_t)(menu_message_until - SDL_GetTicks());
				SDL_Surface* under = left<P8_MESSAGE_FADE_MS ? SDL_CreateRGBSurfaceWithFormat(0, pw, ph, screen->format->BitsPerPixel, screen->format->format) : NULL;
				if (under) SDL_BlitSurface(screen, &box, under, NULL);
				GFX_blitRect(ASSET_DARK_GRAY_PILL, screen, &box);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ box.x + (pw-text->w)/2, box.y + (ph-text->h)/2 });
				SDL_FreeSurface(text);
				if (under) {
					SDL_SetSurfaceBlendMode(under, SDL_BLENDMODE_BLEND);
					SDL_SetSurfaceAlphaMod(under, 255 - 255 * MAX(left, 0) / P8_MESSAGE_FADE_MS);
					SDL_BlitSurface(under, NULL, screen, &(SDL_Rect){ box.x, box.y });
					SDL_FreeSurface(under);
				}
			}
			testShot("menu", screen->pixels, screen->w, screen->h, screen->pitch);
			GFX_flip(screen);
			dirty = 0;
		}
		else if (title_scrolls) {
			SDL_Rect strip = { SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE) };
			SDL_BlitSurface(backing, &strip, screen, &(SDL_Rect){ strip.x, strip.y });
			SDL_BlitSurface(overlay, &strip, screen, &(SDL_Rect){ strip.x, strip.y });
			GFX_blitTitle(screen, rom_name, title_width);
			GFX_flip(screen);
		}
		else GFX_sync();
	}

	SDL_FreeSurface(overlay);
	SDL_FreeSurface(backing);
	PAD_reset();
	GFX_clearAll();
	PWR_warn(1);
	GFX_setVsync(VSYNC_OFF);
	SDL_GL_SetSwapInterval(1); // PICO-8 keeps its own time, and runs at 30 or 60: whole frames, without tearing
	PWR_disableAutosleep();
	SFX_forInput(0); // (not in the game: PICO-8's own buttons don't click)
	in_menu = 0;
	play.last_tick = SDL_GetTicks();
}

///////////////////////////////
// what PICO-8 calls instead of SDL's video

static int h_SDL_Init(Uint32 flags) { return SDL_Init(flags & ~SDL_INIT_UNWANTED); }
static int h_SDL_InitSubSystem(Uint32 flags) { return SDL_InitSubSystem(flags & ~SDL_INIT_UNWANTED); }

static void startVideo(void) {
	if (video_ready) return;
	video_ready = 1;
	ensureSettings();
	screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	PWR_warn(1);
	PWR_disableAutosleep();
	GFX_setVsync(VSYNC_OFF);
	window = PLAT_getWindow();
	renderer = SDL_GetRenderer(window);
	rotate = PLAT_getRotate();
	if (!renderer) fprintf(stderr, "p8layer: no renderer: %s\n", SDL_GetError());
	SDL_GL_SetSwapInterval(1);
	Play_init();
	fprintf(stderr, "p8layer: window ready, rotate %i\n", rotate);
}
static SDL_Window* h_SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) {
	startVideo();
	return window;
}
static void h_SDL_DestroyWindow(SDL_Window* w) {} // it's MinUI's, closed when PICO-8 exits
static void h_SDL_VideoQuit(void) {}
static SDL_Renderer* h_SDL_CreateRenderer(SDL_Window* w, int index, Uint32 flags) {
	startVideo();
	return renderer;
}
static void h_SDL_DestroyRenderer(SDL_Renderer* r) {}
static SDL_GLContext h_SDL_GL_CreateContext(SDL_Window* w) {
	fprintf(stderr, "p8layer: PICO-8 asked for OpenGL; it gets SDL's renderer instead (opengl_blit 0)\n");
	SDL_SetError("no OpenGL here");
	return NULL;
}
static void h_SDL_GL_DeleteContext(SDL_GLContext context) {}

// the screen it's told it has: the A30's panel the right way round
static void fillMode(SDL_DisplayMode* mode) {
	memset(mode, 0, sizeof(*mode));
	mode->format = SDL_PIXELFORMAT_RGB888;
	mode->w = FIXED_WIDTH;
	mode->h = FIXED_HEIGHT;
	mode->refresh_rate = 60;
}
static int h_SDL_GetCurrentDisplayMode(int index, SDL_DisplayMode* mode) { fillMode(mode); return 0; }
static int h_SDL_GetDesktopDisplayMode(int index, SDL_DisplayMode* mode) { fillMode(mode); return 0; }
static int h_SDL_GetDisplayMode(int index, int mode_index, SDL_DisplayMode* mode) { fillMode(mode); return 0; }
static int h_SDL_GetNumDisplayModes(int index) { return 1; }
static int h_SDL_GetDisplayBounds(int index, SDL_Rect* rect) {
	*rect = (SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT};
	return 0;
}
static void h_SDL_GetWindowSize(SDL_Window* w, int* width, int* height) {
	// exactly its screen, so it draws each of its pixels once and the layer does the scaling, effect and border
	if (width) *width = P8_SIZE;
	if (height) *height = P8_SIZE;
}
static void h_SDL_GetWindowPosition(SDL_Window* w, int* x, int* y) {
	if (x) *x = 0;
	if (y) *y = 0;
}

// how it draws: its picture into the window's surface, which comes here instead
static SDL_Surface* window_surface;
static SDL_Texture* window_texture;
static SDL_Surface* h_SDL_GetWindowSurface(SDL_Window* w) {
	startVideo();
	if (!window_surface) window_surface = SDL_CreateRGBSurfaceWithFormat(0, P8_SIZE, P8_SIZE, 32, SDL_PIXELFORMAT_ARGB8888);
	return window_surface;
}
static void presentGame(void);
static int h_SDL_UpdateWindowSurface(SDL_Window* w) {
	TEST_COUNT(0);
	if (!window_surface) return -1;
	if (!window_texture) {
		window_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, P8_SIZE, P8_SIZE);
		if (!window_texture) {
			static int told;
			if (!told++) fprintf(stderr, "p8layer: no texture for its picture: %s\n", SDL_GetError());
			return -1;
		}
	}
	SDL_UpdateTexture(window_texture, NULL, window_surface->pixels, window_surface->pitch);
	for (int y=0; y<P8_SIZE; y++) memcpy(game_pixels + y*P8_SIZE*4, (uint8_t*)window_surface->pixels + y*window_surface->pitch, P8_SIZE*4);
	game_format = SDL_PIXELFORMAT_ARGB8888;
	game_bpp = 4;
	game_pixels_ready = 1;
	game_texture = window_texture;
	game_src = (SDL_Rect){0, 0, P8_SIZE, P8_SIZE};
	presentGame();
	return 0;
}

static SDL_Texture* h_SDL_CreateTexture(SDL_Renderer* r, Uint32 format, int access, int w, int h) {
	SDL_Texture* texture = SDL_CreateTexture(renderer, format, access, w, h);
	if (texture && w==P8_SIZE && h==P8_SIZE && SDL_BYTESPERPIXEL(format)>=2 && SDL_BYTESPERPIXEL(format)<=4) {
		game_tracked = texture;
		game_format = format;
		game_bpp = SDL_BYTESPERPIXEL(format);
		game_pixels_ready = 0;
	}
	fprintf(stderr, "p8layer: texture %ix%i %s\n", w, h, SDL_GetPixelFormatName(format));
	return texture;
}
static void h_SDL_DestroyTexture(SDL_Texture* texture) {
	if (texture==game_tracked) game_tracked = NULL;
	if (texture==game_texture) game_texture = NULL;
	SDL_DestroyTexture(texture);
}
static int h_SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rect, const void* pixels, int pitch) {
#ifdef P8LAYER_TEST
	static int told;
	if (told++<3) fprintf(stderr, "p8layer: update %p (game %p) rect %i,%i %ix%i pitch %i\n", (void*)texture, (void*)game_tracked, rect ? rect->x : -1, rect ? rect->y : -1, rect ? rect->w : -1, rect ? rect->h : -1, pitch);
#endif
	if (texture==game_tracked && (!rect || (rect->x==0 && rect->y==0 && rect->w==P8_SIZE && rect->h==P8_SIZE)) && pixels) { // kept for the menu's picture of the game
		for (int y=0; y<P8_SIZE; y++) memcpy(game_pixels + y*P8_SIZE*game_bpp, (const uint8_t*)pixels + y*pitch, P8_SIZE*game_bpp);
		game_pixels_ready = 1;
	}
	return SDL_UpdateTexture(texture, rect, pixels, pitch);
}
static int h_SDL_RenderClear(SDL_Renderer* r) { return 0; }
static int h_SDL_SetRenderDrawColor(SDL_Renderer* r, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) { return 0; }
static int h_SDL_RenderCopy(SDL_Renderer* r, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst) {
	TEST_COUNT(4);
	// where PICO-8 puts it doesn't matter: the screen goes where the settings say
	int w, h;
	if (SDL_QueryTexture(texture, NULL, NULL, &w, &h)!=0) return 0;
	if (game_tracked && texture!=game_tracked && w<P8_SIZE) return 0; // something small over it: not the screen
	game_texture = texture;
	game_src = src ? *src : (SDL_Rect){0, 0, w, h};
	return 0;
}
static void h_SDL_RenderPresent(SDL_Renderer* r) {
	presentGame();
}

///////////////////////////////
// the buttons: read by MinUI, handed to PICO-8 as its keyboard

typedef struct Key {
	int btn; // BTN_ID_*
	SDL_Scancode code;
	int down;
} Key;
enum { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_A, KEY_B, KEY_X, KEY_Y, KEY_START, KEY_COUNT };
static Key keys[KEY_COUNT] = {
	{BTN_ID_DPAD_UP, SDL_SCANCODE_UP},
	{BTN_ID_DPAD_DOWN, SDL_SCANCODE_DOWN},
	{BTN_ID_DPAD_LEFT, SDL_SCANCODE_LEFT},
	{BTN_ID_DPAD_RIGHT, SDL_SCANCODE_RIGHT},
	{BTN_ID_A, SDL_SCANCODE_Z}, // O
	{BTN_ID_B, SDL_SCANCODE_X}, // X
	{BTN_ID_X, SDL_SCANCODE_C}, // O as well: A and X are one side of the face buttons, B and Y the other
	{BTN_ID_Y, SDL_SCANCODE_V}, // X as well
	{BTN_ID_START, SDL_SCANCODE_RETURN}, // PICO-8's pause menu
};
static SDL_Event events[KEY_COUNT*2+4];
static int event_count, event_next;

static SDL_Scancode keyCode(int i) {
	if (!swap_ab) return keys[i].code;
	switch (i) { // O for X, X for O
		case KEY_A: return SDL_SCANCODE_X;
		case KEY_B: return SDL_SCANCODE_Z;
		case KEY_X: return SDL_SCANCODE_V;
		case KEY_Y: return SDL_SCANCODE_C;
	}
	return keys[i].code;
}
static void queueKey(SDL_Scancode code, int down) {
#ifdef P8LAYER_TEST
	fprintf(stderr, "p8layer: key %i %s at poll %i\n", code, down ? "down" : "up", test_polls);
#endif
	key_state[code] = down ? 1 : 0;
#ifdef P8LAYER_TEST
	if (getenv("P8LAYER_NOEVENTS")) return;
#endif
	if (event_count>=(int)(sizeof(events)/sizeof(events[0]))) return;
	SDL_Event* event = &events[event_count++];
	memset(event, 0, sizeof(*event));
	event->type = down ? SDL_KEYDOWN : SDL_KEYUP;
	event->key.timestamp = SDL_GetTicks();
	event->key.windowID = window ? SDL_GetWindowID(window) : 0;
	event->key.state = down ? SDL_PRESSED : SDL_RELEASED;
	event->key.keysym.scancode = code;
	event->key.keysym.sym = SDL_GetKeyFromScancode(code);
}
static void releaseAll(void) {
	for (int i=0; i<KEY_COUNT; i++) {
		if (keys[i].down) queueKey(keyCode(i), 0);
		keys[i].down = 0;
	}
}
static const Uint8* h_SDL_GetKeyboardState(int* count) {
	TEST_COUNT(2);
	if (count) *count = SDL_NUM_SCANCODES;
	return key_state;
}
static SDL_Keymod h_SDL_GetModState(void) { return KMOD_NONE; }
static int h_SDL_NumJoysticks(void) { return 0; } // the buttons arrive as keys
static SDL_Window* h_SDL_GetKeyboardFocus(void) { return window; }

static void queueWindowEvent(int what) { // SDL would have sent these when its window opened
	if (event_count>=(int)(sizeof(events)/sizeof(events[0]))) return;
	SDL_Event* event = &events[event_count++];
	memset(event, 0, sizeof(*event));
	event->type = SDL_WINDOWEVENT;
	event->window.timestamp = SDL_GetTicks();
	event->window.windowID = window ? SDL_GetWindowID(window) : 0;
	event->window.event = what;
}
static void queueQuit(void) {
	if (event_count>=(int)(sizeof(events)/sizeof(events[0]))) return;
	SDL_Event* event = &events[event_count++];
	memset(event, 0, sizeof(*event));
	event->type = SDL_QUIT;
	event->quit.timestamp = SDL_GetTicks();
}

static int h_SDL_PollEvent(SDL_Event* event) {
	TEST_COUNT(3);
	if (event_next<event_count) {
		if (event) *event = events[event_next++];
		else return 1;
		return 1;
	}
	if (event_count) { // drained: nothing more until its next frame
		event_count = event_next = 0;
		return 0;
	}
	if (!video_ready) return 0;
	// it asks a dozen times or more a frame: the buttons are read at most every few milliseconds
	static uint32_t polled_at;
	uint32_t now = SDL_GetTicks();
#ifndef P8LAYER_TEST // (the harness counts polls)
	if (polled_at && now-polled_at<POLL_MS && !quit_requested) return 0;
#endif
	polled_at = now;
	PAD_poll(); // the buttons, straight from the A30's input devices
	testPad();
#ifdef P8LAYER_TEST
	testRawKeys();
#endif
	SDL_PumpEvents(); // SDL's own copies of them, and the rest, only pile up: PICO-8 gets what's queued here instead
	SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
	static int show_setting = 0; // the volume or brightness bar over the game, which also clicks each step
	PWR_update(NULL, &show_setting, beforeSleep, afterSleep);
	GFX_settingOverGame(show_setting);
	Play_tick();

	static int shown;
	if (!shown) { // (the real ones never reach it: everything SDL queues is dropped)
		shown = 1;
		queueWindowEvent(SDL_WINDOWEVENT_SHOWN);
		queueWindowEvent(SDL_WINDOWEVENT_EXPOSED);
		queueWindowEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
	}
	if (quit_requested) {
		if (SDL_GetTicks()-quit_requested_at>4000) { // it should be gone by now
			fprintf(stderr, "p8layer: PICO-8 didn't quit, leaving anyway\n");
			Play_save();
			sync();
			_exit(0);
		}
		queueQuit();
	}
	else {
		static int menu_used, switcher_ready;
		static uint32_t menu_down_at;
		if (PAD_justPressed(BTN_MENU)) { menu_used = 0; switcher_ready = 0; menu_down_at = SDL_GetTicks(); }
		if (PAD_isPressed(BTN_MENU) && (PAD_isPressed(BTN_PLUS) || PAD_isPressed(BTN_MINUS))) menu_used = 1; // brightness
		if (PAD_isPressed(BTN_MENU) && !menu_used && !switcher_ready && SDL_GetTicks()-menu_down_at>=SWITCHER_HOLD_MS) {
			switcher_ready = 1; // the cue that it's ready
			VIB_pulse(32768, 60);
		}
		if (PAD_justReleased(BTN_MENU) && !menu_used && SDL_GetTicks()-menu_down_at>=SWITCHER_HOLD_MS) {
			menu_used = 1;
			saveForSwitcher();
		}
		else if (PAD_justReleased(BTN_MENU) && !menu_used) {
			releaseAll();
			runMenu();
			if (quit_requested) {
				Play_save();
				queueQuit();
			}
		}
		else if (!PAD_isPressed(BTN_MENU)) {
			for (int i=0; i<KEY_COUNT; i++) {
				int down = PAD_isPressed(1<<keys[i].btn)!=0; // exactly 1: PICO-8 takes nothing else as held
				if (i<=KEY_RIGHT) down = down || PAD_isPressed(1<<(BTN_ID_ANALOG_UP+i)); // the stick moves too
				if (down==keys[i].down) continue;
				keys[i].down = down;
				queueKey(keyCode(i), down);
			}
		}
	}
	if (!event_count) return 0;
	if (!event) return 1;
	*event = events[event_next++];
	return 1;
}

///////////////////////////////
// pointing pico8_dyn's calls here

typedef struct Hook {
	const char* name;
	void* function;
} Hook;
#define HOOK(name) { #name, (void*)h_##name }
static Hook hooks[] = {
	HOOK(SDL_Init), HOOK(SDL_InitSubSystem),
	HOOK(SDL_OpenAudio), HOOK(SDL_PauseAudio),
	HOOK(SDL_CreateWindow), HOOK(SDL_DestroyWindow), HOOK(SDL_VideoQuit),
	HOOK(SDL_CreateRenderer), HOOK(SDL_DestroyRenderer),
	HOOK(SDL_GL_CreateContext), HOOK(SDL_GL_DeleteContext),
	HOOK(SDL_GetCurrentDisplayMode), HOOK(SDL_GetDesktopDisplayMode), HOOK(SDL_GetDisplayMode),
	HOOK(SDL_GetNumDisplayModes), HOOK(SDL_GetDisplayBounds),
	HOOK(SDL_GetWindowSize), HOOK(SDL_GetWindowPosition),
	HOOK(SDL_GetWindowSurface), HOOK(SDL_UpdateWindowSurface),
	HOOK(SDL_CreateTexture), HOOK(SDL_DestroyTexture), HOOK(SDL_UpdateTexture),
	HOOK(SDL_RenderClear), HOOK(SDL_SetRenderDrawColor), HOOK(SDL_RenderCopy), HOOK(SDL_RenderPresent),
	HOOK(SDL_GetKeyboardState), HOOK(SDL_GetModState), HOOK(SDL_NumJoysticks), HOOK(SDL_GetKeyboardFocus),
	HOOK(SDL_PollEvent),
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
static int hookPico8(void) {
	int fd = open("/proc/self/exe", O_RDONLY);
	if (fd<0) return 0;
	struct stat info;
	fstat(fd, &info);
	char* file = mmap(NULL, info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (file==MAP_FAILED) return 0;
	int hooked = 0;
	Elf32_Ehdr* elf = (Elf32_Ehdr*)file;
	Elf32_Shdr* sections = (Elf32_Shdr*)(file + elf->e_shoff);
	for (int i=0; i<elf->e_shnum; i++) {
		Elf32_Shdr* section = &sections[i];
		if (section->sh_type!=SHT_REL) continue;
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
			if (hook) {
				writeSlot(rel[r].r_offset, hook);
				hooked += 1;
			}
		}
	}
	munmap(file, info.st_size);
	return hooked;
}

__attribute__((constructor)) static void layerInit(void) {
	char exe[512] = {0};
	if (readlink("/proc/self/exe", exe, sizeof(exe)-1)<=0) return;
	char* name = strrchr(exe, '/');
	if (!name || strncmp(name+1, "pico8", 5)) return; // only PICO-8, not the wget it runs for Splore
	unsetenv("LD_PRELOAD");
	const char* rom = getenv("P8LAYER_ROM");
	if (rom) {
		snprintf(rom_path, sizeof(rom_path), "%s", rom);
		const char* slash = strrchr(rom, '/');
		char file[256];
		snprintf(file, sizeof(file), "%s", slash ? slash+1 : rom);
		getDisplayName(file, rom_name);
	}
	if (!rom_name[0]) snprintf(rom_name, sizeof(rom_name), "PICO-8");
	const char* home = getenv("HOME");
	snprintf(settings_path, sizeof(settings_path), "%s/minuiplus.txt", home ? home : ".");
	loadSettings();
	int hooked = hookPico8();
	fprintf(stderr, "p8layer: %i calls hooked for %s\n", hooked, rom_name);
}
