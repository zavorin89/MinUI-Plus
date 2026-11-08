#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"

///////////////////////////////

void LOG_note(int level, const char* fmt, ...) {
	char buf[1024] = {0};
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	switch(level) {
#ifdef DEBUG
	case LOG_DEBUG:
		printf("[DEBUG] %s", buf);
		break;
#endif
	case LOG_INFO:
		printf("[INFO] %s", buf);
		break;
	case LOG_WARN:
		fprintf(stderr, "[WARN] %s", buf);
		break;
	case LOG_ERROR:
		fprintf(stderr, "[ERROR] %s", buf);
		break;
	default:
		break;
	}
	fflush(stdout);
}

///////////////////////////////

uint32_t RGB_WHITE;
uint32_t RGB_BLACK;
uint32_t RGB_LIGHT_GRAY;
uint32_t RGB_GRAY;
uint32_t RGB_DARK_GRAY;

static struct GFX_Context {
	SDL_Surface* screen;
	SDL_Surface* assets;
	SDL_Surface* assets_base; // untinted, so the theme can change while running
	int theme;

	int mode;
	int vsync;
} gfx;

///////////////////////////////

static uint32_t asset_rgbs[ASSET_COLORS];

typedef struct Theme {
	const char* name;
	uint8_t r,g,b; // what white becomes
} Theme;
static const Theme themes[] = { // spread around the color wheel so no two look alike
	{ "Vanilla",		0xff,0xff,0xff }, // the classic look
	{ "Strawberry",		0xff,0x8f,0xa3 }, // red-pink
	{ "Tangerine",		0xff,0xaa,0x5c }, // orange
	{ "Cinnamon",		0xd9,0xa0,0x7a }, // muted warm brown
	{ "Lemon",			0xff,0xe8,0x66 }, // yellow
	{ "Lime",			0xc4,0xf0,0x6a }, // yellow-green
	{ "Mint",			0x7f,0xf0,0xcc }, // aqua green
	{ "Blue Raspberry",	0x7c,0xcf,0xff }, // sky blue
	{ "Blueberry",		0x9d,0xae,0xff }, // periwinkle
	{ "Grape",			0xc6,0x9c,0xff }, // purple
	{ "Bubblegum",		0xff,0x96,0xe2 }, // magenta-pink
};
#define THEME_COUNT (int)(sizeof(themes)/sizeof(themes[0]))
ThemeColors theme_colors = {
	{TRIAD_WHITE}, {TRIAD_GRAY}, {TRIAD_LIGHT_TEXT}, {TRIAD_DARK_TEXT},
};

// a neutral grey of brightness v tinted by the theme, mixed toward white by keep/256 (for text that has to stay readable)
static SDL_Color themeTint(int v, int keep) {
	const Theme* t = &themes[gfx.theme];
	int r = v * t->r / 255, g = v * t->g / 255, b = v * t->b / 255;
	return (SDL_Color){ (r*(256-keep) + v*keep) >> 8, (g*(256-keep) + v*keep) >> 8, (b*(256-keep) + v*keep) >> 8 };
}
static uint32_t themeRGB(int v) {
	SDL_Color c = themeTint(v, 0);
	return SDL_MapRGB(gfx.screen->format, c.r, c.g, c.b);
}
// recolors the white and grey parts of the asset sheet, leaving colored ones (eg. the low battery red) alone
static void tintAssets(void) {
	if (!gfx.assets_base) return;
	if (gfx.assets) SDL_FreeSurface(gfx.assets);
	gfx.assets = SDL_ConvertSurface(gfx.assets_base, gfx.assets_base->format, 0);
	SDLX_SetAlpha(gfx.assets, SDL_SRCALPHA, 0); // blended like the loaded sheet
	if (!gfx.theme || gfx.assets->format->BytesPerPixel!=4) return; // vanilla is the sheet as drawn
	SDL_LockSurface(gfx.assets);
	SDL_PixelFormat* format = gfx.assets->format;
	for (int y=0; y<gfx.assets->h; y++) {
		uint32_t* row = (uint32_t*)((uint8_t*)gfx.assets->pixels + y * gfx.assets->pitch);
		for (int x=0; x<gfx.assets->w; x++) {
			uint8_t r,g,b,a;
			SDL_GetRGBA(row[x], format, &r,&g,&b,&a);
			if (!a) continue;
			int lo = MIN(r, MIN(g, b));
			int hi = MAX(r, MAX(g, b));
			if (hi-lo>24) continue; // has its own color
			SDL_Color c = themeTint((r+g+b)/3, 0);
			row[x] = SDL_MapRGBA(format, c.r, c.g, c.b, a);
		}
	}
	SDL_UnlockSurface(gfx.assets);
}
static void applyTheme(void) {
	theme_colors.white = themeTint(0xff, 128); // text: half way, still clearly light
	theme_colors.gray = themeTint(0x99, 128);
	theme_colors.light_text = themeTint(0xcc, 128);
	theme_colors.dark_text = themeTint(0x66, 64);

	RGB_WHITE		= themeRGB(0xff);
	RGB_BLACK		= SDL_MapRGB(gfx.screen->format, TRIAD_BLACK);
	RGB_LIGHT_GRAY	= themeRGB(0x7f);
	RGB_GRAY		= themeRGB(0x99);
	RGB_DARK_GRAY	= themeRGB(0x26);

	asset_rgbs[ASSET_WHITE_PILL]	= RGB_WHITE;
	asset_rgbs[ASSET_BLACK_PILL]	= RGB_BLACK;
	asset_rgbs[ASSET_DARK_GRAY_PILL]= RGB_DARK_GRAY;
	asset_rgbs[ASSET_OPTION]		= RGB_DARK_GRAY;
	asset_rgbs[ASSET_BUTTON]		= RGB_WHITE;
	asset_rgbs[ASSET_PAGE_BG]		= RGB_WHITE;
	asset_rgbs[ASSET_STATE_BG]		= RGB_WHITE;
	asset_rgbs[ASSET_PAGE]			= RGB_BLACK;
	asset_rgbs[ASSET_BAR]			= RGB_WHITE;
	asset_rgbs[ASSET_BAR_BG]		= RGB_BLACK;
	asset_rgbs[ASSET_BAR_BG_MENU]	= RGB_DARK_GRAY;
	asset_rgbs[ASSET_UNDERLINE]		= RGB_GRAY;
	asset_rgbs[ASSET_DOT]			= RGB_LIGHT_GRAY;
	asset_rgbs[ASSET_HOLE]			= RGB_BLACK;

	tintAssets();
}
int GFX_getThemeCount(void) {
	return THEME_COUNT;
}
const char* GFX_getThemeName(int index) {
	return themes[index].name;
}
int GFX_getTheme(void) {
	return gfx.theme;
}
void GFX_setTheme(int index) {
	if (index<0 || index>=THEME_COUNT) index = 0;
	gfx.theme = index;
	applyTheme();
	putFile(THEME_PATH, (char*)themes[index].name);
}

// button labels and hints are a few dozen strings drawn on every redraw, so each is rendered once and kept
#define HINT_CACHE_SIZE 48
static struct { TTF_Font* font; uint32_t color; char text[128]; SDL_Surface* surface; uint32_t used; } hint_cache[HINT_CACHE_SIZE];
static uint32_t hint_clock = 0;
static SDL_Surface* renderHint(TTF_Font* font, char* text, SDL_Color color) { // owned by the cache, don't free it
	uint32_t key = (color.r<<24) | (color.g<<16) | (color.b<<8) | color.a;
	int oldest = 0;
	for (int i=0; i<HINT_CACHE_SIZE; i++) {
		if (hint_cache[i].surface && hint_cache[i].font==font && hint_cache[i].color==key && !strcmp(hint_cache[i].text, text)) {
			hint_cache[i].used = ++hint_clock;
			return hint_cache[i].surface;
		}
		if (!hint_cache[i].surface || (hint_cache[oldest].surface && hint_cache[i].used<hint_cache[oldest].used)) oldest = i;
	}
	SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text, color);
	if (!surface || strlen(text)>=sizeof(hint_cache[0].text)) return surface; // too long to keep (rare): the caller can't free it, so it leaks once
	if (hint_cache[oldest].surface) SDL_FreeSurface(hint_cache[oldest].surface);
	hint_cache[oldest].font = font;
	hint_cache[oldest].color = key;
	strcpy(hint_cache[oldest].text, text);
	hint_cache[oldest].surface = surface;
	hint_cache[oldest].used = ++hint_clock;
	return surface;
}

static SDL_Rect asset_rects[ASSET_COUNT];
GFX_Fonts font;

///////////////////////////////

static struct PWR_Context {
	int initialized;
	
	int can_sleep;
	int can_poweroff;
	int can_autosleep;
	int requested_sleep;
	int requested_wake;
	
	pthread_t battery_pt;
	int is_charging;
	int charge;
	int should_warn;

	int timers[PWR_TIMER_COUNT]; // index into each timer's choices
	int timers_loaded;

	SDL_Surface* overlay;
} pwr = {0};

///////////////////////////////

static int _;
static int refreshClock(void);

SDL_Surface* GFX_init(int mode) {
	refreshClock(); // initialize local time before an emulator loads RTC data
	// TODO: this doesn't really belong here...
	// tried adding to PWR_init() but that was no good (not sure why)
	PLAT_initLid();
	
	gfx.screen = PLAT_initVideo();
	gfx.vsync = VSYNC_STRICT;
	gfx.mode = mode;
	
	asset_rects[ASSET_WHITE_PILL]		= (SDL_Rect){SCALE4( 1, 1,30,30)};
	asset_rects[ASSET_BLACK_PILL]		= (SDL_Rect){SCALE4(33, 1,30,30)};
	asset_rects[ASSET_DARK_GRAY_PILL]	= (SDL_Rect){SCALE4(65, 1,30,30)};
	asset_rects[ASSET_OPTION]			= (SDL_Rect){SCALE4(97, 1,20,20)};
	asset_rects[ASSET_BUTTON]			= (SDL_Rect){SCALE4( 1,33,20,20)};
	asset_rects[ASSET_PAGE_BG]			= (SDL_Rect){SCALE4(64,33,15,15)};
	asset_rects[ASSET_STATE_BG]			= (SDL_Rect){SCALE4(23,54, 8, 8)};
	asset_rects[ASSET_PAGE]				= (SDL_Rect){SCALE4(39,54, 6, 6)};
	asset_rects[ASSET_BAR]				= (SDL_Rect){SCALE4(33,58, 4, 4)};
	asset_rects[ASSET_BAR_BG]			= (SDL_Rect){SCALE4(15,55, 4, 4)};
	asset_rects[ASSET_BAR_BG_MENU]		= (SDL_Rect){SCALE4(85,56, 4, 4)};
	asset_rects[ASSET_UNDERLINE]		= (SDL_Rect){SCALE4(85,51, 3, 3)};
	asset_rects[ASSET_DOT]				= (SDL_Rect){SCALE4(33,54, 2, 2)};
	asset_rects[ASSET_BRIGHTNESS]		= (SDL_Rect){SCALE4(23,33,19,19)};
	asset_rects[ASSET_VOLUME_MUTE]		= (SDL_Rect){SCALE4(44,33,10,16)};
	asset_rects[ASSET_VOLUME]			= (SDL_Rect){SCALE4(44,33,18,16)};
	asset_rects[ASSET_BATTERY]			= (SDL_Rect){SCALE4(47,51,17,10)};
	asset_rects[ASSET_BATTERY_LOW]		= (SDL_Rect){SCALE4(66,51,17,10)};
	asset_rects[ASSET_BATTERY_FILL]		= (SDL_Rect){SCALE4(81,33,12, 6)};
	asset_rects[ASSET_BATTERY_FILL_LOW]	= (SDL_Rect){SCALE4( 1,55,12, 6)};
	asset_rects[ASSET_BATTERY_BOLT]		= (SDL_Rect){SCALE4(81,41,12, 6)};
	asset_rects[ASSET_SCROLL_UP]		= (SDL_Rect){SCALE4(97,23,24, 6)};
	asset_rects[ASSET_SCROLL_DOWN]		= (SDL_Rect){SCALE4(97,31,24, 6)};
	asset_rects[ASSET_WIFI]				= (SDL_Rect){SCALE4(95,39,14,10)};
	asset_rects[ASSET_HOLE]				= (SDL_Rect){SCALE4( 1,63,20,20)};
	
	char asset_path[MAX_PATH];
	sprintf(asset_path, RES_PATH "/assets@%ix.png", FIXED_SCALE);
	if (!exists(asset_path)) LOG_info("missing assets, you're about to segfault dummy!\n");
	gfx.assets_base = IMG_Load(asset_path);
	
	// the saved theme, by name so reordering the list doesn't change anyone's pick
	gfx.theme = 0;
	char theme_name[64] = "Lime"; // until one is picked
	if (exists(THEME_PATH)) getFile(THEME_PATH, theme_name, sizeof(theme_name));
	for (int i=0; i<THEME_COUNT; i++) {
		if (prefixMatch((char*)themes[i].name, theme_name)) gfx.theme = i;
	}
	// flavors that were replaced by more distinct ones
	char* renamed[][2] = { { "Peach", "Tangerine" }, { "Matcha", "Lime" } };
	for (int r=0; r<2; r++) {
		if (!prefixMatch(renamed[r][0], theme_name)) continue;
		for (int i=0; i<THEME_COUNT; i++) if (!strcmp(themes[i].name, renamed[r][1])) gfx.theme = i;
	}
	applyTheme();
	
	TTF_Init();
	font.large 	= TTF_OpenFont(FONT_PATH, SCALE1(FONT_LARGE));
	font.medium = TTF_OpenFont(FONT_PATH, SCALE1(FONT_MEDIUM));
	font.small 	= TTF_OpenFont(FONT_PATH, SCALE1(FONT_SMALL));
	font.tiny 	= TTF_OpenFont(FONT_PATH, SCALE1(FONT_TINY));
	
	TTF_SetFontStyle(font.large, TTF_STYLE_BOLD);
	TTF_SetFontStyle(font.medium, TTF_STYLE_BOLD);
	TTF_SetFontStyle(font.small, TTF_STYLE_BOLD);
	TTF_SetFontStyle(font.tiny, TTF_STYLE_BOLD);
	
	return gfx.screen;
}
void GFX_quit(void) {
	for (int i=0; i<HINT_CACHE_SIZE; i++) if (hint_cache[i].surface) SDL_FreeSurface(hint_cache[i].surface);
	memset(hint_cache, 0, sizeof(hint_cache));
	TTF_CloseFont(font.large);
	TTF_CloseFont(font.medium);
	TTF_CloseFont(font.small);
	TTF_CloseFont(font.tiny);
	
	SDL_FreeSurface(gfx.assets);
	SDL_FreeSurface(gfx.assets_base);
	gfx.assets = gfx.assets_base = NULL;
	
	GFX_freeAAScaler();
	
	GFX_clearAll();

	PLAT_quitVideo();
}

void GFX_setMode(int mode) {
	gfx.mode = mode;
}
int GFX_getVsync(void) {
	return gfx.vsync;
}
void GFX_setVsync(int vsync) {
	PLAT_setVsync(vsync);
	gfx.vsync = vsync;
}

int GFX_hdmiChanged(void) {
	static int had_hdmi = -1;
	int has_hdmi = GetHDMI();
	if (had_hdmi==-1) had_hdmi = has_hdmi;
	if (had_hdmi==has_hdmi) return 0;
	had_hdmi = has_hdmi;
	return 1;
}

#define FRAME_BUDGET 17 // 60fps
static uint32_t frame_start = 0;
void GFX_startFrame(void) {
	frame_start = SDL_GetTicks();
}

void GFX_flip(SDL_Surface* screen) {
	int should_vsync = (gfx.vsync!=VSYNC_OFF && (gfx.vsync==VSYNC_STRICT || frame_start==0 || SDL_GetTicks()-frame_start<FRAME_BUDGET));
	PLAT_flip(screen, should_vsync);
}
void GFX_sync(void) {
	uint32_t frame_duration = SDL_GetTicks() - frame_start;
	if (gfx.vsync!=VSYNC_OFF) {
		// this limiting condition helps SuperFX chip games
		if (gfx.vsync==VSYNC_STRICT || frame_start==0 || frame_duration<FRAME_BUDGET) { // only wait if we're under frame budget
			PLAT_vsync(FRAME_BUDGET-frame_duration);
		}
	}
	else {
		if (frame_duration<FRAME_BUDGET) SDL_Delay(FRAME_BUDGET-frame_duration);
	}
}

FALLBACK_IMPLEMENTATION int PLAT_supportsOverscan(void) { return 0; }
FALLBACK_IMPLEMENTATION void PLAT_showSleepLight(int level) { }
FALLBACK_IMPLEMENTATION int PLAT_supportsBorder(void) { return 0; }
FALLBACK_IMPLEMENTATION void PLAT_setBorder(const char* path) { }
FALLBACK_IMPLEMENTATION void PLAT_setNotification(SDL_Surface* overlay) { }
FALLBACK_IMPLEMENTATION void PLAT_setNotificationAlpha(int alpha) { }
FALLBACK_IMPLEMENTATION void PLAT_setSettingOverlay(SDL_Surface* overlay) { }
FALLBACK_IMPLEMENTATION void PLAT_setSettingAlpha(int alpha) { }
FALLBACK_IMPLEMENTATION SDL_Surface* PLAT_captureScreen(void) { return NULL; }
FALLBACK_IMPLEMENTATION void PLAT_presentLastFrame(void) { }
FALLBACK_IMPLEMENTATION void PLAT_powerSave(int state) { }
FALLBACK_IMPLEMENTATION void PLAT_setCPUFreq(int mhz) {
	PLAT_setCPUSpeedAsync(mhz<=800 ? CPU_SPEED_MENU : (mhz<=1100 ? CPU_SPEED_POWERSAVE : (mhz<=1400 ? CPU_SPEED_NORMAL : CPU_SPEED_PERFORMANCE)));
}
FALLBACK_IMPLEMENTATION GFX_Texture* PLAT_createTexture(SDL_Surface* surface) { return NULL; } // no GPU path, callers draw in software
FALLBACK_IMPLEMENTATION void PLAT_destroyTexture(GFX_Texture* texture) { }
FALLBACK_IMPLEMENTATION void PLAT_queueTexture(GFX_Texture* texture, SDL_Rect* dst, int alpha, int shade) { }
FALLBACK_IMPLEMENTATION void PLAT_setEffectColor(int next_color) { }

int GFX_truncateText(TTF_Font* font, const char* in_name, char* out_name, int max_width, int padding) {
	int text_width;
	strcpy(out_name, in_name);
	TTF_SizeUTF8(font, out_name, &text_width, NULL);
	text_width += padding;
	if (text_width<=max_width) return text_width;

	// the longest start (on a whole UTF-8 character) that fits with "..." after it, found by halving
	// instead of trimming a character at a time, so a long name costs a few measurements instead of dozens
	int len = strlen(in_name);
	int lo = 0, hi = len; // byte lengths: lo always fits (or is where we give up), hi never does
	int lo_width = 0;
	while (hi-lo>1) {
		int mid = (lo + hi) / 2;
		while (mid>lo && (in_name[mid] & 0xC0)==0x80) mid--; // back to the start of a character
		if (mid==lo) { // only continuation bytes between: step forward to the next character instead
			mid = lo + 1;
			while (mid<hi && (in_name[mid] & 0xC0)==0x80) mid++;
			if (mid>=hi) break;
		}
		memcpy(out_name, in_name, mid);
		strcpy(out_name+mid, "...");
		int w;
		TTF_SizeUTF8(font, out_name, &w, NULL);
		w += padding;
		if (w<=max_width) { lo = mid; lo_width = w; }
		else hi = mid;
	}
	int keep = lo;
	if (!keep) { // nothing fits, show at least one character (like before)
		keep = 1;
		while (keep<len && (in_name[keep] & 0xC0)==0x80) keep++;
	}
	int trimmed = keep;
	while (trimmed>1 && (in_name[trimmed-1]==' ' || in_name[trimmed-1]=='.')) trimmed--; // no "Name ..." or "Name...."
	memcpy(out_name, in_name, trimmed);
	strcpy(out_name+trimmed, "...");
	if (trimmed==lo && lo) return lo_width;
	TTF_SizeUTF8(font, out_name, &text_width, NULL);
	return text_width + padding;
}
// wraps str in place at spaces so no line is wider than max_width, keeping the line breaks it already has;
// with more text than max_lines (0 for no limit) the last line stops at a whole word and ends in ending
// (eg. "..."), so str needs strlen(ending) spare bytes
int GFX_wrapTextEnding(TTF_Font* font, char* str, int max_width, int max_lines, const char* ending) {
	if (!str) return 0;
	int widest = 0;
	int lines = 0;
	char* line = str;
	while (1) {
		lines += 1;
		// the last space (or the paragraph's end) where this line still fits; a lone word that's too wide stays whole
		char* cut = NULL;
		int width = 0;
		for (char* end=line; ; end++) {
			if (*end && *end!=' ' && *end!='\n') continue;
			char c = *end;
			*end = '\0';
			int w;
			TTF_SizeUTF8(font, line, &w, NULL);
			*end = c;
			if (w>max_width && cut) break;
			cut = end;
			width = w;
			if (!c || c=='\n' || w>max_width) break;
		}

		if (*cut && max_lines && lines==max_lines) { // out of lines: as many whole words as fit with the ending
			char shown[MAX_PATH+64];
			int keep = 0; // bytes of line kept
			for (char* end=line; ; end++) {
				if (*end && *end!=' ' && *end!='\n') continue;
				int len = end - line;
				while (len>0 && strchr(" .,;:", line[len-1])) len--; // no "word ..." or "word,..."
				snprintf(shown, sizeof(shown), "%.*s%s", len, line, ending);
				int w;
				TTF_SizeUTF8(font, shown, &w, NULL);
				if (w>max_width && keep) break;
				keep = len;
				width = w;
				if (!*end || *end=='\n' || w>max_width) break;
			}
			strcpy(line + keep, ending);
			if (width>widest) widest = width;
			break;
		}

		if (width>widest) widest = width;
		if (!*cut) break;
		*cut = '\n';
		line = cut + 1;
	}
	return widest;
}
int GFX_wrapText(TTF_Font* font, char* str, int max_width, int max_lines) {
	return GFX_wrapTextEnding(font, str, max_width, max_lines, "...");
}

///////////////////////////////

// scale_blend (and supporting logic) from picoarch

struct blend_args {
	int w_ratio_in;
	int w_ratio_out;
	uint16_t w_bp[2];
	int h_ratio_in;
	int h_ratio_out;
	uint16_t h_bp[2];
	uint16_t *blend_line;
} blend_args;

#if __ARM_ARCH >= 5
static inline uint32_t average16(uint32_t c1, uint32_t c2) {
	uint32_t ret, lowbits = 0x0821;
	asm ("eor %0, %2, %3\r\n"
	     "and %0, %0, %1\r\n"
	     "add %0, %3, %0\r\n"
	     "add %0, %0, %2\r\n"
	     "lsr %0, %0, #1\r\n"
	     : "=&r" (ret) : "r" (lowbits), "r" (c1), "r" (c2) : );
	return ret;
}
static inline uint32_t average32(uint32_t c1, uint32_t c2) {
	uint32_t ret, lowbits = 0x08210821;

	asm ("eor %0, %3, %1\r\n"
	     "and %0, %0, %2\r\n"
	     "adds %0, %1, %0\r\n"
	     "and %1, %1, #0\r\n"
	     "movcs %1, #0x80000000\r\n"
	     "adds %0, %0, %3\r\n"
	     "rrx %0, %0\r\n"
	     "orr %0, %0, %1\r\n"
	     : "=&r" (ret), "+r" (c2) : "r" (lowbits), "r" (c1) : "cc" );

	return ret;
}

#define AVERAGE16_NOCHK(c1, c2) (average16((c1), (c2)))
#define AVERAGE32_NOCHK(c1, c2) (average32((c1), (c2)))

#else

static inline uint32_t average16(uint32_t c1, uint32_t c2) {
	return (c1 + c2 + ((c1 ^ c2) & 0x0821))>>1;
}
static inline uint32_t average32(uint32_t c1, uint32_t c2) {
	uint32_t sum = c1 + c2;
	uint32_t ret = sum + ((c1 ^ c2) & 0x08210821);
	uint32_t of = ((sum < c1) | (ret < sum)) << 31;

	return (ret >> 1) | of;
}

#define AVERAGE16_NOCHK(c1, c2) (average16((c1), (c2)))
#define AVERAGE32_NOCHK(c1, c2) (average32((c1), (c2)))

#endif

#define AVERAGE16(c1, c2) ((c1) == (c2) ? (c1) : AVERAGE16_NOCHK((c1), (c2)))
#define AVERAGE16_1_3(c1, c2) ((c1) == (c2) ? (c1) : (AVERAGE16_NOCHK(AVERAGE16_NOCHK((c1), (c2)), (c2))))

#define AVERAGE32(c1, c2) ((c1) == (c2) ? (c1) : AVERAGE32_NOCHK((c1), (c2)))
#define AVERAGE32_1_3(c1, c2) ((c1) == (c2) ? (c1) : (AVERAGE32_NOCHK(AVERAGE32_NOCHK((c1), (c2)), (c2))))

static inline int gcd(int a, int b) {
	return b ? gcd(b, a % b) : a;
}

static void scaleAA(void* __restrict src, void* __restrict dst, uint32_t w, uint32_t h, uint32_t pitch, uint32_t dst_w, uint32_t dst_h, uint32_t dst_p) {
	int dy = 0;
	int lines = h;

	int rat_w = blend_args.w_ratio_in;
	int rat_dst_w = blend_args.w_ratio_out;
	uint16_t *bw = blend_args.w_bp;

	int rat_h = blend_args.h_ratio_in;
	int rat_dst_h = blend_args.h_ratio_out;
	uint16_t *bh = blend_args.h_bp;

	while (lines--) {
		while (dy < rat_dst_h) {
			uint16_t *dst16 = (uint16_t *)dst;
			uint16_t *pblend = (uint16_t *)blend_args.blend_line;
			int col = w;
			int dx = 0;

			uint16_t *pnext = (uint16_t *)(src + pitch);
			if (!lines)
				pnext -= (pitch / sizeof(uint16_t));

			if (dy > rat_dst_h - bh[0]) {
				pblend = pnext;
			} else if (dy <= bh[0]) {
				/* Drops const, won't get touched later though */
				pblend = (uint16_t *)src;
			} else {
				const uint32_t *src32 = (const uint32_t *)src;
				const uint32_t *pnext32 = (const uint32_t *)pnext;
				uint32_t *pblend32 = (uint32_t *)pblend;
				int count = w / 2;

				if (dy <= bh[1]) {
					const uint32_t *tmp = pnext32;
					pnext32 = src32;
					src32 = tmp;
				}

				if (dy > rat_dst_h - bh[1] || dy <= bh[1]) {
					while(count--) {
						*pblend32++ = AVERAGE32_1_3(*src32, *pnext32);
						src32++;
						pnext32++;
					}
				} else {
					while(count--) {
						*pblend32++ = AVERAGE32(*src32, *pnext32);
						src32++;
						pnext32++;
					}
				}
			}

			while (col--) {
				uint16_t a, b, out;

				a = *pblend;
				b = *(pblend+1);

				while (dx < rat_dst_w) {
					if (a == b) {
						out = a;
					} else if (dx > rat_dst_w - bw[0]) { // top quintile, bbbb
						out = b;
					} else if (dx <= bw[0]) { // last quintile, aaaa
						out = a;
					} else {
						if (dx > rat_dst_w - bw[1]) { // 2nd quintile, abbb
							a = AVERAGE16_NOCHK(a,b);
						} else if (dx <= bw[1]) { // 4th quintile, aaab
							b = AVERAGE16_NOCHK(a,b);
						}

						out = AVERAGE16_NOCHK(a, b); // also 3rd quintile, aabb
					}
					*dst16++ = out;
					dx += rat_w;
				}

				dx -= rat_dst_w;
				pblend++;
			}

			dy += rat_h;
			dst += dst_p;
		}

		dy -= rat_dst_h;
		src += pitch;
	}
}

scaler_t GFX_getAAScaler(GFX_Renderer* renderer) {
	int gcd_w, div_w, gcd_h, div_h;
	blend_args.blend_line = calloc(renderer->src_w, sizeof(uint16_t));

	gcd_w = gcd(renderer->src_w, renderer->dst_w);
	blend_args.w_ratio_in = renderer->src_w / gcd_w;
	blend_args.w_ratio_out = renderer->dst_w / gcd_w;
	
	double blend_denominator = (renderer->src_w>renderer->dst_w) ? 5 : 2.5; // TODO: these values are really only good for the nano...
	// blend_denominator = 5.0; // better for trimui
	// LOG_info("blend_denominator: %f (%i && %i)\n", blend_denominator, HAS_SKINNY_SCREEN, renderer->dst_w>renderer->src_w);
	
	div_w = round(blend_args.w_ratio_out / blend_denominator);
	blend_args.w_bp[0] = div_w;
	blend_args.w_bp[1] = blend_args.w_ratio_out >> 1;

	gcd_h = gcd(renderer->src_h, renderer->dst_h);
	blend_args.h_ratio_in = renderer->src_h / gcd_h;
	blend_args.h_ratio_out = renderer->dst_h / gcd_h;

	div_h = round(blend_args.h_ratio_out / blend_denominator);
	blend_args.h_bp[0] = div_h;
	blend_args.h_bp[1] = blend_args.h_ratio_out >> 1;
	
	return scaleAA;
}
void GFX_freeAAScaler(void) {
	if (blend_args.blend_line != NULL) {
		free(blend_args.blend_line);
		blend_args.blend_line = NULL;
	}
}

///////////////////////////////

void GFX_blitAsset(int asset, SDL_Rect* src_rect, SDL_Surface* dst, SDL_Rect* dst_rect) {
	SDL_Rect* rect = &asset_rects[asset];
	SDL_Rect adj_rect = {
		.x = rect->x,
		.y = rect->y,
		.w = rect->w,
		.h = rect->h,
	};
	if (src_rect) {
		adj_rect.x += src_rect->x;
		adj_rect.y += src_rect->y;
		adj_rect.w  = src_rect->w;
		adj_rect.h  = src_rect->h;
	}
	SDL_BlitSurface(gfx.assets, &adj_rect, dst, dst_rect);
}
void GFX_blitPill(int asset, SDL_Surface* dst, SDL_Rect* dst_rect) {
	int x = dst_rect->x;
	int y = dst_rect->y;
	int w = dst_rect->w;
	int h = dst_rect->h;

	if (h==0) h = asset_rects[asset].h;
	
	int r = h / 2;
	if (w < h) w = h;
	w -= h;
	
	GFX_blitAsset(asset, &(SDL_Rect){0,0,r,h}, dst, &(SDL_Rect){x,y});
	x += r;
	if (w>0) {
		SDL_FillRect(dst, &(SDL_Rect){x,y,w,h}, asset_rgbs[asset]);
		x += w;
	}
	GFX_blitAsset(asset, &(SDL_Rect){r,0,r,h}, dst, &(SDL_Rect){x,y});
}
void GFX_blitRect(int asset, SDL_Surface* dst, SDL_Rect* dst_rect) {
	int x = dst_rect->x;
	int y = dst_rect->y;
	int w = dst_rect->w;
	int h = dst_rect->h;
	int c = asset_rgbs[asset];
	
	SDL_Rect* rect = &asset_rects[asset];
	int d = rect->w;
	int r = d / 2;

	GFX_blitAsset(asset, &(SDL_Rect){0,0,r,r}, dst, &(SDL_Rect){x,y});
	SDL_FillRect(dst, &(SDL_Rect){x+r,y,w-d,r}, c);
	GFX_blitAsset(asset, &(SDL_Rect){r,0,r,r}, dst, &(SDL_Rect){x+w-r,y});
	SDL_FillRect(dst, &(SDL_Rect){x,y+r,w,h-d}, c);
	GFX_blitAsset(asset, &(SDL_Rect){0,r,r,r}, dst, &(SDL_Rect){x,y+h-r});
	SDL_FillRect(dst, &(SDL_Rect){x+r,y+h-r,w-d,r}, c);
	GFX_blitAsset(asset, &(SDL_Rect){r,r,r,r}, dst, &(SDL_Rect){x+w-r,y+h-r});
}
// The sheet's ASSET_BATTERY_BOLT is a solid bar with a slit in it, which reads as a full battery. This one sits over the
// battery and leaves the level showing. The outline keeps it clear of a full bar. Drawn for the destination's format
// because the low battery overlay isn't always the screen's.
static void blitChargingBolt(SDL_Surface* dst, int cx, int cy) {
	static const uint8_t spans[][2] = { // x,w at 2x
		{7,4},{6,5},{6,4},{5,5},{5,4},{4,5},{4,4},{3,5},{3,9},{2,10},
		{2,9},{6,5},{6,4},{5,5},{5,4},{4,4},{4,3},{4,2},{3,2},{3,1},
	};
	#define BOLT_UNIT(n) ((n) * FIXED_SCALE / 2)
	int count = sizeof(spans) / sizeof(spans[0]);
	int ox = cx - BOLT_UNIT(7);
	int oy = cy - BOLT_UNIT(count) / 2;
	uint8_t r,g,b;
	SDL_GetRGB(RGB_WHITE, gfx.screen->format, &r,&g,&b);
	uint32_t white = SDL_MapRGB(dst->format, r,g,b);
	SDL_GetRGB(RGB_BLACK, gfx.screen->format, &r,&g,&b);
	uint32_t black = SDL_MapRGB(dst->format, r,g,b);
	for (int i=0; i<count; i++) {
		SDL_FillRect(dst, &(SDL_Rect){
			ox + BOLT_UNIT(spans[i][0]) - BOLT_UNIT(1), oy + BOLT_UNIT(i) - BOLT_UNIT(1),
			BOLT_UNIT(spans[i][1] + 2), BOLT_UNIT(3)
		}, black);
	}
	for (int i=0; i<count; i++) {
		SDL_FillRect(dst, &(SDL_Rect){
			ox + BOLT_UNIT(spans[i][0]), oy + BOLT_UNIT(i),
			BOLT_UNIT(spans[i][1]), BOLT_UNIT(1)
		}, white);
	}
	#undef BOLT_UNIT
}
void GFX_blitBattery(SDL_Surface* dst, SDL_Rect* dst_rect) {
	// LOG_info("dst: %p\n", dst);
	int x = 0;
	int y = 0;
	if (dst_rect) {
		x = dst_rect->x;
		y = dst_rect->y;
	}
	SDL_Rect rect = asset_rects[ASSET_BATTERY];
	x += (SCALE1(PILL_SIZE) - (rect.w + FIXED_SCALE)) / 2;
	y += (SCALE1(PILL_SIZE) - rect.h) / 2;
	
	int percent = pwr.charge;
	GFX_blitAsset(percent<=10?ASSET_BATTERY_LOW:ASSET_BATTERY, NULL, dst, &(SDL_Rect){x,y});
	
	SDL_Rect fill = asset_rects[ASSET_BATTERY_FILL];
	SDL_Rect clip = fill;
	clip.w *= percent;
	clip.w /= 100;
	if (clip.w>0) {
		clip.x = fill.w - clip.w;
		clip.y = 0;
		GFX_blitAsset(percent<=20?ASSET_BATTERY_FILL_LOW:ASSET_BATTERY_FILL, &clip, dst, &(SDL_Rect){x+SCALE1(3)+clip.x,y+SCALE1(2)});
	}
	
	if (pwr.is_charging) blitChargingBolt(dst, x + SCALE1(2) + (rect.w - SCALE1(2)) / 2, y + rect.h / 2);
}
int GFX_getButtonWidth(char* hint, char* button) {
	int button_width = 0;
	int width;
	
	int special_case = !strcmp(button,BRIGHTNESS_BUTTON_LABEL); // TODO: oof
	
	if (strlen(button)==1) {
		button_width += SCALE1(BUTTON_SIZE);
	}
	else {
		// the cached render GFX_blitButton is about to draw, so the width costs nothing extra
		button_width += SCALE1(BUTTON_SIZE) / 2;
		SDL_Surface* label = renderHint(special_case ? font.large : font.tiny, button, COLOR_BUTTON_TEXT);
		width = label ? label->w : 0;
		button_width += width;
	}
	button_width += SCALE1(BUTTON_MARGIN);
	
	SDL_Surface* text = renderHint(font.small, hint, COLOR_WHITE);
	width = text ? text->w : 0;
	button_width += width + SCALE1(BUTTON_MARGIN);
	return button_width;
}
void GFX_blitButton(char* hint, char*button, SDL_Surface* dst, SDL_Rect* dst_rect) {
	SDL_Surface* text;
	int ox = 0;
	
	int special_case = !strcmp(button,BRIGHTNESS_BUTTON_LABEL); // TODO: oof
	
	// button
	if (strlen(button)==1) {
		GFX_blitAsset(ASSET_BUTTON, NULL, dst, dst_rect);

		// label
		text = renderHint(font.medium, button, COLOR_BUTTON_TEXT);
		if (text) SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){dst_rect->x+(SCALE1(BUTTON_SIZE)-text->w)/2,dst_rect->y+(SCALE1(BUTTON_SIZE)-text->h)/2});
		ox += SCALE1(BUTTON_SIZE);
	}
	else {
		text = renderHint(special_case ? font.large : font.tiny, button, COLOR_BUTTON_TEXT);
		if (!text) return;
		GFX_blitPill(ASSET_BUTTON, dst, &(SDL_Rect){dst_rect->x,dst_rect->y,SCALE1(BUTTON_SIZE)/2+text->w,SCALE1(BUTTON_SIZE)});
		ox += SCALE1(BUTTON_SIZE)/4;
		
		int oy = special_case ? SCALE1(-2) : 0;
		SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){ox+dst_rect->x,oy+dst_rect->y+(SCALE1(BUTTON_SIZE)-text->h)/2,text->w,text->h});
		ox += text->w;
		ox += SCALE1(BUTTON_SIZE)/4;
	}
	
	ox += SCALE1(BUTTON_MARGIN);

	// hint text
	text = renderHint(font.small, hint, COLOR_WHITE);
	if (text) SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){ox+dst_rect->x,dst_rect->y+(SCALE1(BUTTON_SIZE)-text->h)/2,text->w,text->h});
}
void GFX_blitMessage(TTF_Font* font, char* msg, SDL_Surface* dst, SDL_Rect* dst_rect) {
	if (!dst_rect) dst_rect = &(SDL_Rect){0,0,dst->w,dst->h};
	
	// LOG_info("GFX_blitMessage: %p (%ix%i)", dst, dst_rect->w,dst_rect->h);
	
	SDL_Surface* text;
#define TEXT_BOX_MAX_ROWS 16
#define LINE_HEIGHT 24
	char* rows[TEXT_BOX_MAX_ROWS];
	int row_count = 0;

	char* tmp;
	rows[row_count++] = msg;
	while ((tmp=strchr(rows[row_count-1], '\n'))!=NULL) {
		if (row_count+1>=TEXT_BOX_MAX_ROWS) return; // TODO: bail
		rows[row_count++] = tmp+1;
	}
	
	int rendered_height = SCALE1(LINE_HEIGHT) * row_count;
	int y = dst_rect->y;
	y += (dst_rect->h - rendered_height) / 2;
	
	char line[256];
	for (int i=0; i<row_count; i++) {
		int len;
		if (i+1<row_count) {
			len = rows[i+1]-rows[i]-1;
			if (len) strncpy(line, rows[i], len);
			line[len] = '\0';
		}
		else {
			len = strlen(rows[i]);
			strcpy(line, rows[i]);
		}
		
		
		if (len) {
			text = TTF_RenderUTF8_Blended(font, line, COLOR_WHITE);
			int x = dst_rect->x;
			x += (dst_rect->w - text->w) / 2;
			SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){x,y});
			SDL_FreeSurface(text);
		}
		y += SCALE1(LINE_HEIGHT);
	}
}

// the volume/brightness bar fades out instead of snapping back to the battery
#define SETTING_FADE_MS 150
static uint32_t setting_fade_until = 0;
static SDL_Surface* setting_shot = NULL; // the bar as last drawn, faded over the battery
static SDL_Rect setting_shot_rect;
int GFX_settingFading(void) {
	return setting_shot && setting_fade_until && (int32_t)(setting_fade_until - SDL_GetTicks())>0;
}
static void GFX_keepSetting(SDL_Surface* dst, SDL_Rect* rect) {
	if (!setting_shot || setting_shot->w!=rect->w || setting_shot->h!=rect->h || setting_shot->format->format!=dst->format->format) {
		if (setting_shot) SDL_FreeSurface(setting_shot);
		setting_shot = SDL_CreateRGBSurfaceWithFormat(0, rect->w, rect->h, dst->format->BitsPerPixel, dst->format->format);
		if (!setting_shot) return;
		SDL_SetSurfaceBlendMode(setting_shot, SDL_BLENDMODE_BLEND);
	}
	SDL_BlitSurface(dst, rect, setting_shot, NULL);
	setting_shot_rect = *rect;
}
static int GFX_blitHardwareGroupNow(SDL_Surface* dst, int show_setting);
// the clock in the hardware group: off, 12 or 24 hour, kept in CLOCK_PATH
static int clock_mode = -1;
static time_t clock_drawn_minute = 0; // PWR_update redraws when the minute changes
static int clock_left = 0; // in a pill of its own at the top left (the carousel: its art covers the top right)
// a pause menu's game name in its pill, up to max_width: one that doesn't fit scrolls back and forth (and returns 1, so
// call this again each frame, over a clean background) instead of being cut short
#define TITLE_PAUSE_MS 700 // still at each end
#define TITLE_SPEED 70 // unscaled pixels a second
int GFX_blitTitle(SDL_Surface* dst, const char* name, int max_width) {
	static uint32_t started = 0;
	static char drawn[256] = "";
	static SDL_Surface* text = NULL;
	if (!text || !exactMatch((char*)name, drawn)) {
		if (text) SDL_FreeSurface(text);
		text = TTF_RenderUTF8_Blended(font.large, name, COLOR_WHITE);
		snprintf(drawn, sizeof(drawn), "%s", name);
		started = SDL_GetTicks();
		if (!text) return 0; // (no name to draw)
	}
	static int drawn_width = 0;
	if (max_width!=drawn_width) { // a volume or brightness bar came or went: from the start, not from a jump
		drawn_width = max_width;
		started = SDL_GetTicks();
	}
	int padding = SCALE1(BUTTON_PADDING*2);
	int width = MIN(text->w + padding, max_width);
	int overflow = text->w + padding - width;
	int offset = 0;
	if (overflow>0) {
		int scroll_ms = overflow * 1000 / SCALE1(TITLE_SPEED);
		int t = (SDL_GetTicks() - started) % (TITLE_PAUSE_MS + scroll_ms + TITLE_PAUSE_MS);
		if (t>=TITLE_PAUSE_MS+scroll_ms) offset = overflow;
		else if (t>=TITLE_PAUSE_MS) offset = (t - TITLE_PAUSE_MS) * overflow / scroll_ms;
	}
	GFX_blitPill(ASSET_BLACK_PILL, dst, &(SDL_Rect){ SCALE1(PADDING), SCALE1(PADDING), width, SCALE1(PILL_SIZE) });
	SDL_BlitSurface(text, &(SDL_Rect){ offset, 0, width-padding, text->h }, dst, &(SDL_Rect){ SCALE1(PADDING+BUTTON_PADDING), SCALE1(PADDING+4) });
	return overflow>0;
}
void GFX_setClockLeft(int left) {
	clock_left = left;
}
int GFX_getClockMode(void) {
	if (clock_mode<0) clock_mode = exists(CLOCK_PATH) ? getInt(CLOCK_PATH) : CLOCK_12H;
	if (clock_mode<CLOCK_OFF || clock_mode>CLOCK_24H) clock_mode = CLOCK_12H;
	return clock_mode;
}
int PWR_getSleepLight(void) {
	if (!exists(SLEEP_LIGHT_PATH)) return SLEEP_LIGHT_ON; // as it always was
	return getInt(SLEEP_LIGHT_PATH) ? SLEEP_LIGHT_ON : SLEEP_LIGHT_OFF; // (an earlier dim/medium/bright counts as on)
}
void PWR_setSleepLight(int level) {
	putInt(SLEEP_LIGHT_PATH, level);
}
void GFX_setClockMode(int mode) {
	clock_mode = mode;
	putInt(CLOCK_PATH, mode);
}
// named zones with their daylight saving rules (POSIX TZ), so the clock follows the season by itself
static const struct { const char* name; const char* rule; } time_zones[] = {
	{ "Hawaii",			"HST10" },
	{ "Alaska",			"AKST9AKDT,M3.2.0,M11.1.0" },
	{ "US Pacific",		"PST8PDT,M3.2.0,M11.1.0" },
	{ "US Mountain",	"MST7MDT,M3.2.0,M11.1.0" },
	{ "Arizona",		"MST7" },
	{ "US Central",		"CST6CDT,M3.2.0,M11.1.0" },
	{ "US Eastern",		"EST5EDT,M3.2.0,M11.1.0" },
	{ "Atlantic",		"AST4ADT,M3.2.0,M11.1.0" },
	{ "Brazil",			"<-03>3" },
	{ "UTC",			"UTC0" },
	{ "UK & Ireland",	"GMT0BST,M3.5.0/1,M10.5.0" },
	{ "Central Europe",	"CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Eastern Europe",	"EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Moscow",			"MSK-3" },
	{ "India",			"IST-5:30" },
	{ "China",			"CST-8" },
	{ "Japan",			"JST-9" },
	{ "Korea",			"KST-9" },
	{ "Australia East",	"AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "New Zealand",	"NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
#define TIME_ZONE_COUNT (int)(sizeof(time_zones)/sizeof(time_zones[0]))
int GFX_getTimeZoneCount(void) {
	return TIME_ZONE_COUNT;
}
const char* GFX_getTimeZoneName(int zone) {
	if (zone>=0 && zone<TIME_ZONE_COUNT) return time_zones[zone].name;
	if (zone==-2) { // looked up, not listed: its offset, eg. "<UTC+0800>-8" as UTC+08:00
		static char name[16];
		char rule[64] = "";
		getFile(TIME_ZONE_PATH, rule, sizeof(rule));
		if (strlen(rule)>=9 && prefixMatch("<UTC", rule)) {
			snprintf(name, sizeof(name), "UTC%c%c%c:%c%c", rule[4], rule[5], rule[6], rule[7], rule[8]);
			return name;
		}
	}
	return "Automatic";
}
int GFX_getTimeZone(void) {
	if (!exists(TIME_ZONE_PATH)) return -1;
	char rule[64] = "";
	getFile(TIME_ZONE_PATH, rule, sizeof(rule));
	trimTrailingNewlines(rule);
	for (int i=0; i<TIME_ZONE_COUNT; i++) if (exactMatch((char*)time_zones[i].rule, rule)) return i;
	return rule[0] ? -2 : -1;
}
// the zone wifi.sh looked up (or another program set) while this one runs, used as soon as it's there
static int clock_set_seen = -1; // whether CLOCK_SET_PATH was there at the last check
static int refreshClock(void) { // 1 when the clock should be drawn again
	static time_t zone_mtime = -1;
	struct stat st;
	time_t mtime = stat(TIME_ZONE_PATH, &st)==0 ? st.st_mtime : 0;
	int changed = 0;
	if (mtime!=zone_mtime) {
		zone_mtime = mtime;
		char rule[64] = "";
		if (mtime) getFile(TIME_ZONE_PATH, rule, sizeof(rule));
		trimTrailingNewlines(rule);
		if (rule[0]) setenv("TZ", rule, 1);
		else unsetenv("TZ");
		tzset();
		changed = 1;
	}
	int set = (mtime || exists(CLOCK_MANUAL_PATH)) && (exists(CLOCK_SET_PATH) || exists(CLOCK_OK_PATH)); // (no zone: it would show UTC, unless that's what was typed)
	if (set!=clock_set_seen) {
		clock_set_seen = set;
		changed = 1;
	}
	return changed;
}
void GFX_setTimeZone(int zone) {
	int keep = zone>=0 && zone<TIME_ZONE_COUNT && exists(CLOCK_MANUAL_PATH); // what was typed was meant for this zone, it shouldn't shift
	time_t now = time(NULL);
	struct tm typed = *localtime(&now);
	if (zone>=0 && zone<TIME_ZONE_COUNT) {
		putFile(TIME_ZONE_PATH, (char*)time_zones[zone].rule);
		setenv("TZ", time_zones[zone].rule, 1); // launch.sh hands it to everything started later
	}
	else {
		unlink(TIME_ZONE_PATH);
		unsetenv("TZ");
	}
	tzset();
	if (keep) PLAT_setDateTime(typed.tm_year+1900, typed.tm_mon+1, typed.tm_mday, typed.tm_hour, typed.tm_min, typed.tm_sec);
	clock_drawn_minute = 0; // redrawn in the new zone
	if (PLAT_isOnline()) system("wifi.sh time >/dev/null 2>&1 &"); // synced in the new zone (automatic: looked up first)
}
// the settings and tools name each screen on the top row, left of the time and battery, in the grey of the settings'
// names, so no row has to share that line
// a header too long for the room left of the hardware group (with a gap before its pill) scrolls there and back
#define HEADER_PAUSE_MS 1200 // still at each end
#define HEADER_SPEED 30 // unscaled pixels a second
// how far a text overflow pixels too wide has scrolled, there and back with a pause at each end
static int scrollOffset(int overflow, uint32_t started) {
	if (overflow<=0) return 0;
	int scroll_ms = MAX(1, overflow * 1000 / SCALE1(HEADER_SPEED));
	int t = (SDL_GetTicks() - started) % (HEADER_PAUSE_MS*2 + scroll_ms*2);
	if (t<HEADER_PAUSE_MS) return 0;
	if (t<HEADER_PAUSE_MS+scroll_ms) return (t - HEADER_PAUSE_MS) * overflow / scroll_ms;
	if (t<HEADER_PAUSE_MS*2+scroll_ms) return overflow;
	return overflow - (t - HEADER_PAUSE_MS*2 - scroll_ms) * overflow / scroll_ms;
}
// a tooltip in a dark pill at the bottom left, beside the button hints that take right_w on the right. Text too long
// for the room scrolls (like a long title) instead of being cut off: returns 1 while it does, so the caller redraws
int GFX_blitHintPill(SDL_Surface* dst, const char* text, int right_w) {
	static SDL_Surface* rendered;
	static char drawn[256];
	static uint32_t started;
	if (!text || !text[0]) return 0;
	if (!rendered || strcmp(drawn, text)) { // rendered once per text, not every frame it scrolls
		if (rendered) SDL_FreeSurface(rendered);
		rendered = TTF_RenderUTF8_Blended(font.small, text, COLOR_WHITE);
		snprintf(drawn, sizeof(drawn), "%s", text);
		started = SDL_GetTicks();
	}
	if (!rendered) return 0;
	int room = dst->w - SCALE1(PADDING*3) - right_w - SCALE1(BUTTON_PADDING*2); // for the text inside the pill
	int shown_w = MIN(rendered->w, room);
	int pill_y = dst->h - SCALE1(PADDING + PILL_SIZE);
	GFX_blitPill(ASSET_DARK_GRAY_PILL, dst, &(SDL_Rect){ SCALE1(PADDING), pill_y, shown_w + SCALE1(BUTTON_PADDING*2), SCALE1(PILL_SIZE) });
	int overflow = rendered->w - room;
	int offset = scrollOffset(overflow, started);
	SDL_BlitSurface(rendered, &(SDL_Rect){ offset, 0, shown_w, rendered->h }, dst, &(SDL_Rect){ SCALE1(PADDING + BUTTON_PADDING), pill_y + (SCALE1(PILL_SIZE) - rendered->h) / 2 });
	return overflow>0;
}
int GFX_blitHeaderText(SDL_Surface* dst, TTF_Font* f, SDL_Color color, const char* title, int ow) {
	static SDL_Surface* text;
	static char drawn[256];
	static TTF_Font* drawn_font;
	static SDL_Color drawn_color;
	static uint32_t started;
	if (!title || !title[0]) return 0;
	if (!text || f!=drawn_font || memcmp(&color, &drawn_color, sizeof(color)) || strcmp(drawn, title)) {
		if (text) SDL_FreeSurface(text);
		text = TTF_RenderUTF8_Blended(f, title, color);
		snprintf(drawn, sizeof(drawn), "%s", title);
		drawn_font = f;
		drawn_color = color;
		started = SDL_GetTicks();
	}
	if (!text) return 0;
	int x = SCALE1(PADDING + BUTTON_PADDING);
	int room = dst->w - SCALE1(PADDING) - ow - SCALE1(PADDING) - x;
	if (room<=0) return 0;
	int overflow = text->w - room;
	int offset = scrollOffset(overflow, started);
	SDL_BlitSurface(text, &(SDL_Rect){ offset, 0, MIN(room, text->w), text->h }, dst, &(SDL_Rect){ x, SCALE1(PADDING) + (SCALE1(PILL_SIZE) - text->h) / 2 });
	return overflow>0;
}
// a list to read through, eg. the games a tool couldn't do something for: each name, with a short note on its right.
// Up and down move (wrapping), left and right a page; A or B closes it
#define LIST_SHOWN 5
void GFX_showList(SDL_Surface* dst, const char* title, char** names, char** notes, int count) {
	int selected = 0, start = 0, dirty = 1, show_setting = 0;
	// the shown rows' text, rendered only when the rows or the selection change (the A30 is slow at rendering text,
	// and a slow frame can lose a quick tap): the selected name whole (it scrolls when too long), the others cut short
	SDL_Surface* row_name[LIST_SHOWN] = {0};
	SDL_Surface* row_note[LIST_SHOWN] = {0};
	int drawn_start = -1, drawn_selected = -1;
	uint32_t selected_at = 0;
	int x = SCALE1(PADDING), w = dst->w - SCALE1(PADDING*2);
	PAD_reset();
	while (1) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B)) break;
		int next = selected;
		// (justPressed too: a press and release read in the same poll leave no repeat)
		if (PAD_justPressed(BTN_UP) || PAD_justRepeated(BTN_UP)) next = (selected + count - 1) % MAX(1, count);
		else if (PAD_justPressed(BTN_DOWN) || PAD_justRepeated(BTN_DOWN)) next = (selected + 1) % MAX(1, count);
		else if (PAD_justPressed(BTN_LEFT) || PAD_justRepeated(BTN_LEFT)) next = MAX(0, selected - LIST_SHOWN);
		else if (PAD_justPressed(BTN_RIGHT) || PAD_justRepeated(BTN_RIGHT)) next = MIN(count - 1, selected + LIST_SHOWN);
		if (next!=selected && next>=0) {
			selected = next;
			dirty = 1;
		}
		if (selected<start) start = selected;
		if (selected>=start+LIST_SHOWN) start = selected - LIST_SHOWN + 1;
		if (start!=drawn_start || selected!=drawn_selected) {
			for (int j=0; j<LIST_SHOWN; j++) {
				if (row_name[j]) SDL_FreeSurface(row_name[j]);
				if (row_note[j]) SDL_FreeSurface(row_note[j]);
				row_name[j] = row_note[j] = NULL;
				int i = start + j;
				if (i>=count) continue;
				int is_selected = i==selected;
				if (notes && notes[i] && notes[i][0]) row_note[j] = TTF_RenderUTF8_Blended(font.tiny, notes[i], is_selected ? COLOR_BLACK : COLOR_LIGHT_TEXT);
				int room = w - SCALE1(BUTTON_PADDING*3) - (row_note[j] ? row_note[j]->w : 0);
				char shown[256];
				if (is_selected) snprintf(shown, sizeof(shown), "%s", names[i]);
				else GFX_truncateText(font.large, names[i], shown, room, 0);
				row_name[j] = TTF_RenderUTF8_Blended(font.large, shown, is_selected ? COLOR_BLACK : COLOR_WHITE);
			}
			if (selected!=drawn_selected) selected_at = SDL_GetTicks();
			drawn_start = start;
			drawn_selected = selected;
		}
		PWR_update(&dirty, &show_setting, NULL, NULL);
		if (dirty) {
			GFX_clear(dst);
			int scrolls = GFX_blitHeader(dst, (char*)title, GFX_blitHardwareGroup(dst, show_setting));
			for (int j=0; j<LIST_SHOWN && start+j<count; j++) {
				int y = SCALE1(PADDING + (j+1)*PILL_SIZE);
				if (start+j==selected) GFX_blitPill(ASSET_WHITE_PILL, dst, &(SDL_Rect){ x, y, w, SCALE1(PILL_SIZE) });
				SDL_Surface* note = row_note[j];
				SDL_Surface* name = row_name[j];
				if (note) SDL_BlitSurface(note, NULL, dst, &(SDL_Rect){ x + w - SCALE1(BUTTON_PADDING) - note->w, y + (SCALE1(PILL_SIZE) - note->h) / 2 });
				if (name) {
					int room = w - SCALE1(BUTTON_PADDING*3) - (note ? note->w : 0);
					int overflow = name->w - room;
					if (overflow>0 && start+j==selected) scrolls = 1;
					int offset = start+j==selected ? scrollOffset(overflow, selected_at) : 0;
					SDL_BlitSurface(name, &(SDL_Rect){ offset, 0, MIN(room, name->w), name->h }, dst, &(SDL_Rect){ x + SCALE1(BUTTON_PADDING), y + (SCALE1(PILL_SIZE) - name->h) / 2 });
				}
			}
			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(dst, show_setting);
			else if (count>LIST_SHOWN) {
				char where[32];
				snprintf(where, sizeof(where), "%i of %i", selected+1, count);
				SDL_Surface* text = TTF_RenderUTF8_Blended(font.small, where, COLOR_LIGHT_TEXT);
				if (text) {
					SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){ SCALE1(PADDING + BUTTON_PADDING), dst->h - SCALE1(PADDING) - (SCALE1(PILL_SIZE) + text->h) / 2 });
					SDL_FreeSurface(text);
				}
			}
			GFX_blitButtonGroup((char*[]){ "B","BACK", NULL }, 0, dst, 1);
			GFX_flip(dst);
			dirty = scrolls; // (a long title or selected name keeps scrolling)
		}
		else GFX_sync();
	}
	for (int j=0; j<LIST_SHOWN; j++) {
		if (row_name[j]) SDL_FreeSurface(row_name[j]);
		if (row_note[j]) SDL_FreeSurface(row_note[j]);
	}
	PAD_reset();
}
int GFX_blitHeader(SDL_Surface* dst, char* title, int ow) {
	return GFX_blitHeaderText(dst, font.large, COLOR_DARK_TEXT, title, ow);
}
// the volume or brightness bar over a game, which draws no hardware group of its own. The bar is drawn like the
// menus' (in the screen's format, on black), then given the see-through edges of a white pill of the same shape.
// It's only drawn again when the value changes; hiding it fades it out
static SDL_Surface* over_game = NULL; // ARGB, the screen's size
static SDL_Surface* over_game_bar = NULL; // the screen's format: the bar, then the pill's mask
static int over_game_setting = 0, over_game_value = -1;
static uint32_t over_game_hidden_at = 0;
void GFX_clearSettingOverGame(void) {
	if (over_game_setting) PLAT_setSettingOverlay(NULL);
	PLAT_setSettingAlpha(255);
	over_game_setting = 0;
	over_game_value = -1;
	over_game_hidden_at = 0;
}
void GFX_settingOverGame(int show_setting) {
	if (show_setting && !GetHDMI()) {
		int value = show_setting==1 ? GetBrightness() : GetVolume();
		if (show_setting!=over_game_setting || value!=over_game_value) {
			// (the device's size and format: a game resizes the screen surface to its own)
			if (!over_game) over_game = SDL_CreateRGBSurfaceWithFormat(0, FIXED_WIDTH, FIXED_HEIGHT, 32, SDL_PIXELFORMAT_ARGB8888);
			if (!over_game_bar) over_game_bar = SDL_CreateRGBSurface(0, FIXED_WIDTH, FIXED_HEIGHT, FIXED_DEPTH, RGBA_MASK_565);
			if (!over_game || !over_game_bar) return;
			int ow = SCALE1(PILL_SIZE + SETTINGS_WIDTH + 10 + 4); // (as GFX_blitHardwareGroupNow lays it out)
			SDL_Rect r = { FIXED_WIDTH - SCALE1(PADDING) - ow, SCALE1(PADDING), ow, SCALE1(PILL_SIZE) };
			SDL_FillRect(over_game, NULL, 0);
			SDL_FillRect(over_game_bar, &r, 0);
			GFX_blitHardwareGroupNow(over_game_bar, show_setting);
			SDL_LockSurface(over_game);
			uint32_t* out = over_game->pixels;
			int pitch = over_game->pitch / 4;
			SDL_PixelFormat* f = over_game_bar->format;
			int bpp = f->BytesPerPixel;
			uint8_t* bar = malloc(r.w * r.h * 3); // the bar's colors, before the mask is drawn over them
			if (bar) {
				for (int y=0; y<r.h; y++) for (int x=0; x<r.w; x++) {
					uint8_t* p = (uint8_t*)over_game_bar->pixels + (r.y+y) * over_game_bar->pitch + (r.x+x) * bpp;
					uint32_t px = bpp==2 ? *(uint16_t*)p : *(uint32_t*)p;
					uint8_t* c = bar + (y*r.w+x)*3;
					SDL_GetRGB(px, f, &c[0], &c[1], &c[2]);
				}
				SDL_FillRect(over_game_bar, &r, 0);
				GFX_blitPill(ASSET_WHITE_PILL, over_game_bar, &r);
				#define MASK_AT(X, Y) ({ \
					uint8_t* p = (uint8_t*)over_game_bar->pixels + (r.y+(Y)) * over_game_bar->pitch + (r.x+(X)) * bpp; \
					uint8_t mr, mg, mb; SDL_GetRGB(bpp==2 ? *(uint16_t*)p : *(uint32_t*)p, f, &mr, &mg, &mb); \
					MAX(mr, MAX(mg, mb)); })
				int full = MAX(1, MASK_AT(r.w/2, r.h/2)); // the theme's "white" isn't always 255: its middle is fully covered
				for (int y=0; y<r.h; y++) for (int x=0; x<r.w; x++) {
					int a = MIN(255, MASK_AT(x, y) * 255 / full); // the pill on black: how much of it covers this pixel
					if (!a) continue;
					uint8_t* c = bar + (y*r.w+x)*3;
					int cr = MIN(255, c[0] * 255 / a), cg = MIN(255, c[1] * 255 / a), cb = MIN(255, c[2] * 255 / a);
					out[(r.y+y) * pitch + r.x+x] = (uint32_t)a<<24 | cr<<16 | cg<<8 | cb;
				}
				#undef MASK_AT
				free(bar);
			}
			SDL_UnlockSurface(over_game);
			PLAT_setSettingOverlay(over_game);
			over_game_setting = show_setting;
			over_game_value = value;
		}
		PLAT_setSettingAlpha(255); // (every time: a fade that ran out leaves it nearly see-through for the next one)
		over_game_hidden_at = 0;
	}
	else if (over_game_setting) {
		uint32_t now = SDL_GetTicks();
		if (!over_game_hidden_at) over_game_hidden_at = now;
		int left = SETTING_FADE_MS - (int)(now - over_game_hidden_at);
		if (left>0) PLAT_setSettingAlpha(255 * left / SETTING_FADE_MS);
		else GFX_clearSettingOverGame();
	}
}
// for a screen with no hardware group of its own (a confirmation, a picker): the volume or brightness bar while it's
// being changed, fading out after
void GFX_blitSettingOnly(SDL_Surface* dst, int show_setting) {
	if (show_setting || GFX_settingFading()) GFX_blitHardwareGroup(dst, show_setting);
}
int GFX_blitHardwareGroup(SDL_Surface* dst, int show_setting) {
	if (show_setting) setting_fade_until = 0;
	int ow = GFX_blitHardwareGroupNow(dst, show_setting);
	if (show_setting && !GetHDMI()) GFX_keepSetting(dst, &(SDL_Rect){ dst->w - SCALE1(PADDING) - ow, SCALE1(PADDING), ow, SCALE1(PILL_SIZE) });
	else if (GFX_settingFading()) {
		int left = setting_fade_until - SDL_GetTicks();
		SDL_SetSurfaceAlphaMod(setting_shot, 255 * left / SETTING_FADE_MS);
		SDL_BlitSurface(setting_shot, NULL, dst, &(SDL_Rect){ setting_shot_rect.x, setting_shot_rect.y });
		ow = MAX(ow, setting_shot_rect.w);
	}
	return ow;
}
static int GFX_blitHardwareGroupNow(SDL_Surface* dst, int show_setting) {
	int ox;
	int oy;
	int ow = 0;
	
	int setting_value;
	int setting_min;
	int setting_max;
	
	if (show_setting && !GetHDMI()) {
		ow = SCALE1(PILL_SIZE + SETTINGS_WIDTH + 10 + 4);
		ox = dst->w - SCALE1(PADDING) - ow;
		oy = SCALE1(PADDING);
		GFX_blitPill(gfx.mode==MODE_MAIN ? ASSET_DARK_GRAY_PILL : ASSET_BLACK_PILL, dst, &(SDL_Rect){
			ox,
			oy,
			ow,
			SCALE1(PILL_SIZE)
		});
		
		if (show_setting==1) {
			setting_value = GetBrightness();
			setting_min = BRIGHTNESS_MIN;
			setting_max = BRIGHTNESS_MAX;
		}
		else {
			setting_value = GetVolume();
			setting_min = VOLUME_MIN;
			setting_max = VOLUME_MAX;
		}
		
		int asset = show_setting==1?ASSET_BRIGHTNESS:(setting_value>0?ASSET_VOLUME:ASSET_VOLUME_MUTE);
		int ax = ox + (show_setting==1 ? SCALE1(6) : SCALE1(8));
		int ay = oy + (show_setting==1 ? SCALE1(5) : SCALE1(7));
		GFX_blitAsset(asset, NULL, dst, &(SDL_Rect){ax,ay});
		
		ox += SCALE1(PILL_SIZE);
		oy += SCALE1((PILL_SIZE - SETTINGS_SIZE) / 2);
		GFX_blitPill(gfx.mode==MODE_MAIN ? ASSET_BAR_BG : ASSET_BAR_BG_MENU, dst, &(SDL_Rect){
			ox,
			oy,
			SCALE1(SETTINGS_WIDTH),
			SCALE1(SETTINGS_SIZE)
		});
		
		float percent = ((float)(setting_value-setting_min) / (setting_max-setting_min));
		if (show_setting==1 || setting_value>0) {
			GFX_blitPill(ASSET_BAR, dst, &(SDL_Rect){
				ox,
				oy,
				SCALE1(SETTINGS_WIDTH) * percent,
				SCALE1(SETTINGS_SIZE)
			});
		}
	}
	else {
		// TODO: handle wifi
		int show_wifi = PLAT_isOnline(); // NOOOOO! not every frame!

		int ww = SCALE1(PILL_SIZE-3);
		ow = SCALE1(PILL_SIZE);
		if (show_wifi) ow += ww;

		// the time, before wifi and the battery
		char clock[16] = "";
		int clock_mode = GFX_getClockMode();
		if (clock_mode && clock_set_seen==-1) refreshClock();
		if (clock_mode && clock_set_seen==1) { // when it can be trusted: see launch.sh
			time_t now = time(NULL);
			struct tm* local = localtime(&now);
			if (clock_mode==CLOCK_24H) strftime(clock, sizeof(clock), "%H:%M", local);
			else {
				strftime(clock, sizeof(clock), "%I:%M %p", local);
				if (clock[0]=='0') memmove(clock, clock+1, strlen(clock)); // "9:41 PM", not "09:41 PM"
			}
			clock_drawn_minute = now / 60;
		}
		SDL_Surface* clock_text = clock[0] ? TTF_RenderUTF8_Blended(font.small, clock, COLOR_WHITE) : NULL;
		if (clock_text && clock_left) {
			int pill_w = clock_text->w + SCALE1(BUTTON_PADDING*2);
			GFX_blitPill(gfx.mode==MODE_MAIN ? ASSET_DARK_GRAY_PILL : ASSET_BLACK_PILL, dst, &(SDL_Rect){ SCALE1(PADDING), SCALE1(PADDING), pill_w, SCALE1(PILL_SIZE) });
			SDL_BlitSurface(clock_text, NULL, dst, &(SDL_Rect){ SCALE1(PADDING + BUTTON_PADDING), SCALE1(PADDING) + (SCALE1(PILL_SIZE) - clock_text->h) / 2 });
			SDL_FreeSurface(clock_text);
			clock_text = NULL;
		}
		int cw = clock_text ? clock_text->w + SCALE1(BUTTON_PADDING) : 0;
		ow += cw;

		ox = dst->w - SCALE1(PADDING) - ow;
		oy = SCALE1(PADDING);
		GFX_blitPill(gfx.mode==MODE_MAIN ? ASSET_DARK_GRAY_PILL : ASSET_BLACK_PILL, dst, &(SDL_Rect){
			ox,
			oy,
			ow,
			SCALE1(PILL_SIZE)
		});
		if (clock_text) {
			SDL_BlitSurface(clock_text, NULL, dst, &(SDL_Rect){ ox + SCALE1(BUTTON_PADDING), oy + (SCALE1(PILL_SIZE) - clock_text->h) / 2 });
			SDL_FreeSurface(clock_text);
			ox += cw;
		}
		if (show_wifi) {
			SDL_Rect rect = asset_rects[ASSET_WIFI];
			int x = ox;
			int y = oy;
			x += (SCALE1(PILL_SIZE) - rect.w) / 2;
			y += (SCALE1(PILL_SIZE) - rect.h) / 2;
			
			GFX_blitAsset(ASSET_WIFI, NULL, dst, &(SDL_Rect){x,y});
			ox += ww;
		}
		GFX_blitBattery(dst, &(SDL_Rect){ox,oy});
	}
	
	return ow;
}
void GFX_blitHardwareHints(SDL_Surface* dst, int show_setting) {
	if (BTN_MOD_VOLUME==BTN_SELECT && BTN_MOD_BRIGHTNESS==BTN_START) {
		if (show_setting==1) GFX_blitButtonGroup((char*[]){ "SELECT","VOLUME",  NULL }, 0, dst, 0);
		else GFX_blitButtonGroup((char*[]){ "START","BRIGHTNESS",  NULL }, 0, dst, 0);
	}
	else {
		if (show_setting==1) GFX_blitButtonGroup((char*[]){ BRIGHTNESS_BUTTON_LABEL,"BRIGHTNESS",  NULL }, 0, dst, 0);
		else GFX_blitButtonGroup((char*[]){ "MENU","BRIGHTNESS",  NULL }, 0, dst, 0);
	}
	
}

int GFX_blitButtonGroup(char** pairs, int primary, SDL_Surface* dst, int align_right) {
	int ox;
	int oy;
	int ow;
	char* hint;
	char* button;

	struct Hint {
		char* hint;
		char* button;
		int ow;
	} hints[2]; 
	int w = 0; // individual button dimension
	int h = 0; // hints index
	ow = 0; // full pill width
	ox = align_right ? dst->w - SCALE1(PADDING) : SCALE1(PADDING);
	oy = dst->h - SCALE1(PADDING + PILL_SIZE);
	
	for (int i=0; i<2; i++) {
		if (!pairs[i*2]) break;
		if (HAS_SKINNY_SCREEN && i!=primary) continue; // space saving
		
		button = pairs[i * 2];
		hint = pairs[i * 2 + 1];
		w = GFX_getButtonWidth(hint, button);
		hints[h].hint = hint;
		hints[h].button = button;
		hints[h].ow = w;
		h += 1;
		ow += SCALE1(BUTTON_MARGIN) + w;
	}
	
	ow += SCALE1(BUTTON_MARGIN);
	if (align_right) ox -= ow;
	GFX_blitPill(gfx.mode==MODE_MAIN ? ASSET_DARK_GRAY_PILL : ASSET_BLACK_PILL, dst, &(SDL_Rect){
		ox,
		oy,
		ow,
		SCALE1(PILL_SIZE)
	});
	
	ox += SCALE1(BUTTON_MARGIN);
	oy += SCALE1(BUTTON_MARGIN);
	for (int i=0; i<h; i++) {
		GFX_blitButton(hints[i].hint, hints[i].button, dst, &(SDL_Rect){ox,oy});
		ox += hints[i].ow + SCALE1(BUTTON_MARGIN);
	}
	return ow;
}

#define MAX_TEXT_LINES 16
void GFX_sizeText(TTF_Font* font, char* str, int leading, int* w, int* h) {
	char* lines[MAX_TEXT_LINES];
	int count = 0;

	char* tmp;
	lines[count++] = str;
	while ((tmp=strchr(lines[count-1], '\n'))!=NULL) {
		if (count+1>MAX_TEXT_LINES) break; // TODO: bail?
		lines[count++] = tmp+1;
	}
	*h = count * leading;
	
	int mw = 0;
	char line[256];
	for (int i=0; i<count; i++) {
		int len;
		if (i+1<count) {
			len = lines[i+1]-lines[i]-1;
			if (len) strncpy(line, lines[i], len);
			line[len] = '\0';
		}
		else {
			len = strlen(lines[i]);
			strcpy(line, lines[i]);
		}
		
		if (len) {
			int lw;
			TTF_SizeUTF8(font, line, &lw, NULL);
			if (lw>mw) mw = lw;
		}
	}
	*w = mw;
}
void GFX_blitText(TTF_Font* font, char* str, int leading, SDL_Color color, SDL_Surface* dst, SDL_Rect* dst_rect) {
	if (dst_rect==NULL) dst_rect = &(SDL_Rect){0,0,dst->w,dst->h};
	
	char* lines[MAX_TEXT_LINES];
	int count = 0;

	char* tmp;
	lines[count++] = str;
	while ((tmp=strchr(lines[count-1], '\n'))!=NULL) {
		if (count+1>MAX_TEXT_LINES) break; // TODO: bail?
		lines[count++] = tmp+1;
	}
	int x = dst_rect->x;
	int y = dst_rect->y;
	
	SDL_Surface* text;
	char line[256];
	for (int i=0; i<count; i++) {
		int len;
		if (i+1<count) {
			len = lines[i+1]-lines[i]-1;
			if (len) strncpy(line, lines[i], len);
			line[len] = '\0';
		}
		else {
			len = strlen(lines[i]);
			strcpy(line, lines[i]);
		}
		
		if (len) {
			text = TTF_RenderUTF8_Blended(font, line, color);
			SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){x+((dst_rect->w-text->w)/2),y+(i*leading)});
			SDL_FreeSurface(text);
		}
	}
}

///////////////////////////////

// based on picoarch's audio 
// implementation, rewritten 
// to (try to) understand it 
// better

#define MAX_SAMPLE_RATE 48000
#define BATCH_SIZE 100
#ifndef SAMPLES
	#define SAMPLES 512 // default
#endif

#define ms SDL_GetTicks

// the audio device as last opened (by the game's sound or the menu sounds), so sleep can close it and open it again
static SDL_AudioSpec audio_want;
static int audio_open = 0;
static int audio_slept = 0;
static void AUDIO_opened(SDL_AudioSpec* want) {
	audio_want = *want;
	audio_open = 1;
}
typedef int (*SND_Resampler)(const SND_Frame frame);
static void SND_fillStream(uint8_t* stream, int len);
static struct SND_Context {
	int initialized;
	double frame_rate;
	
	int sample_rate_in;
	int sample_rate_out;
	
	int buffer_seconds;     // current_audio_buffer_size
	SND_Frame* buffer;		// buf
	size_t frame_count; 	// buf_len
	
	int frame_in;     // buf_w
	int frame_out;    // buf_r
	int frame_filled; // max_buf_w
	
	SND_Resampler resample;
	SND_Frame previous; // the linear resampler's last input frame
	int phase; // how far the next output sits between previous and the frame coming in, 0 to sample_rate_out
} snd = {0};
static void SFX_mix(int16_t* stream, int frames);
static SFX_Music sfx_music = NULL; // background music in the menus, under the sounds
static void SND_audioCallback(void* userdata, uint8_t* stream, int len) { // plat_sound_callback
	SND_fillStream(stream, len);
	if (sfx_music) sfx_music((int16_t*)stream, len / (sizeof(int16_t) * 2)); // the pause menu's music
	SFX_mix((int16_t*)stream, len / (sizeof(int16_t) * 2)); // menu sounds and the achievement chime, over the game
}
static int snd_underruns = 0;
static void SND_fillStream(uint8_t* stream, int len) {
	
	// return (void)memset(stream,0,len); // TODO: tmp, silent
	
	if (snd.frame_count==0) return (void)memset(stream,0,len);
	
	int16_t *out = (int16_t *)stream;
	len /= (sizeof(int16_t) * 2);
	// int full_len = len;
	
	// if (snd.frame_out!=snd.frame_in) LOG_info("%8i consuming samples (%i frames)\n", ms(), len);
	
	while (snd.frame_out!=snd.frame_in && len>0) {
		*out++ = snd.buffer[snd.frame_out].left;
		*out++ = snd.buffer[snd.frame_out].right;
		
		snd.frame_filled = snd.frame_out;
		
		snd.frame_out += 1;
		len -= 1;
		
		if (snd.frame_out>=snd.frame_count) snd.frame_out = 0;
	}
	
	// Never replay old samples backwards (or form a pointer before an empty buffer).
	if (len>0) {
		memset(out, 0, len * sizeof(SND_Frame));
		snd_underruns += 1;
	}
}
static void SND_resizeBuffer(void) { // plat_sound_resize_buffer
	// The ring holds resampled output, so its duration must use the device rate.
	snd.frame_count = snd.buffer_seconds * snd.sample_rate_out / snd.frame_rate;
	if (snd.frame_count==0) return;
	
	// LOG_info("frame_count: %i (%i * %i / %f)\n", snd.frame_count, snd.buffer_seconds, snd.sample_rate_in, snd.frame_rate);
	// snd.frame_count *= 2; // no help
	
	SDL_LockAudio();
	
	int buffer_bytes = snd.frame_count * sizeof(SND_Frame);
	snd.buffer = realloc(snd.buffer, buffer_bytes);
	
	memset(snd.buffer, 0, buffer_bytes);
	
	snd.frame_in = 0;
	snd.frame_out = 0;
	snd.frame_filled = snd.frame_count - 1;
	
	SDL_UnlockAudio();
}
static int SND_resampleNone(SND_Frame frame) { // audio_resample_passthrough
	snd.buffer[snd.frame_in++] = frame;
	if (snd.frame_in >= snd.frame_count) snd.frame_in = 0;
	return 1;
}
// each output frame is a blend of the two input frames either side of it, instead of a copy of the nearest
// (which repeats or drops frames and sounds rough). Called with the same frame until it returns 1: each call
// either writes one output frame (returning 0) or takes the frame in as the new left-hand one (returning 1)
static int SND_resampleLinear(SND_Frame frame) {
	if (snd.phase<snd.sample_rate_out) {
		int64_t t = snd.phase;
		int64_t span = snd.sample_rate_out;
		SND_Frame out = { // always between the two, so it fits back in 16 bits
			.left  = (int16_t)(snd.previous.left  + (int)(((int64_t)(frame.left  - snd.previous.left)  * t) / span)),
			.right = (int16_t)(snd.previous.right + (int)(((int64_t)(frame.right - snd.previous.right) * t) / span)),
		};
		snd.buffer[snd.frame_in++] = out;
		if (snd.frame_in >= snd.frame_count) snd.frame_in = 0;
		snd.phase += snd.sample_rate_in;
		return 0;
	}
	snd.phase -= snd.sample_rate_out;
	snd.previous = frame;
	return 1;
}
static void SND_selectResampler(void) { // plat_sound_select_resampler
	snd.previous = (SND_Frame){0,0};
	snd.phase = 0;
	if (snd.sample_rate_in==snd.sample_rate_out) {
		snd.resample =  SND_resampleNone;
	}
	else {
		snd.resample = SND_resampleLinear;
	}
}
static uint64_t snd_wait_us = 0; // time spent waiting for room, for minarch's Auto CPU speed
static uint64_t snd_waited_us = 0; // and in all
uint64_t SND_waitedUs(void) {
	return snd_waited_us;
}
uint64_t SND_takeWaitUs(void) {
	uint64_t us = snd_wait_us;
	snd_wait_us = 0;
	return us;
}
size_t SND_batchSamples(const SND_Frame* frames, size_t frame_count) { // plat_sound_write / plat_sound_write_resample
	
	// return frame_count; // TODO: tmp, silent
	
	if (snd.frame_count==0) return 0;
	
	// LOG_info("%8i batching samples (%i frames)\n", ms(), frame_count);
	
	SDL_LockAudio();

	int consumed = 0;
	int consumed_frames = 0;
	uint32_t stalled_at = SDL_GetTicks();
	while (frame_count > 0) {
		int tries = 0;
		int amount = MIN(BATCH_SIZE, frame_count);
		
		uint64_t wait_from = snd.frame_in==snd.frame_filled ? getMicroseconds() : 0;
		while (tries < 10 && snd.frame_in==snd.frame_filled) {
			tries++;
			SDL_UnlockAudio();
			SDL_Delay(1);
			SDL_LockAudio();
		}
		if (wait_from) {
			uint64_t waited = getMicroseconds() - wait_from;
			snd_wait_us += waited;
			snd_waited_us += waited;
		}
		if (snd.frame_in==snd.frame_filled && SDL_GetTicks()-stalled_at>=250) {
			// Return to input handling if the audio device stops draining. Drop this batch
			// rather than trapping the emulator in retries forever.
			SDL_UnlockAudio();
			return consumed + frame_count;
		}
		if (snd.frame_in!=snd.frame_filled) stalled_at = SDL_GetTicks();
		// if (tries) LOG_info("%8i waited %ims for buffer to get low...\n", ms(), tries);

		while (amount && snd.frame_in != snd.frame_filled) {
			consumed_frames = snd.resample(*frames);
			
			frames += consumed_frames;
			amount -= consumed_frames;
			frame_count -= consumed_frames;
			consumed += consumed_frames;
		}
	}
	SDL_UnlockAudio();
	
	return consumed;
}

void SND_setRate(double sample_rate) {
	if (!snd.initialized || sample_rate<=0) return;
	SDL_LockAudio();
	snd.sample_rate_in = sample_rate;
	SND_selectResampler();
	SDL_UnlockAudio();
	LOG_info("sample rate: %i (req) %i (rec)\n", snd.sample_rate_in, snd.sample_rate_out);
}
void SND_setBufferFrames(int frames) {
	if (!snd.initialized) return;
	snd.buffer_seconds = frames;
	SND_resizeBuffer();
}
int SND_queuedFrames(void) {
	if (!snd.initialized || snd.frame_count==0 || snd.frame_rate<=0) return 0;
	int queued = (snd.frame_in - snd.frame_out + (int)snd.frame_count) % (int)snd.frame_count;
	return queued * snd.frame_rate / snd.sample_rate_out;
}
int SND_takeUnderruns(void) {
	int underruns = snd_underruns;
	snd_underruns = 0;
	return underruns;
}

void SND_init(double sample_rate, double frame_rate) { // plat_sound_init
	LOG_info("SND_init\n");
	
	SDL_InitSubSystem(SDL_INIT_AUDIO);
	
#if defined(USE_SDL2)
	LOG_info("Available audio drivers:\n");
	for (int i=0; i<SDL_GetNumAudioDrivers(); i++) {
		LOG_info("- %s\n", SDL_GetAudioDriver(i));
	}
	LOG_info("Current audio driver: %s\n", SDL_GetCurrentAudioDriver());
#endif	
	
	memset(&snd, 0, sizeof(struct SND_Context));
	snd.frame_rate = frame_rate;

	SDL_AudioSpec spec_in;
	SDL_AudioSpec spec_out;

	spec_in.freq = PLAT_pickSampleRate(sample_rate, MAX_SAMPLE_RATE);
	spec_in.format = AUDIO_S16;
	spec_in.channels = 2;
	spec_in.samples = SAMPLES;
	spec_in.callback = SND_audioCallback;
	
	if (SDL_OpenAudio(&spec_in, &spec_out)<0) LOG_info("SDL_OpenAudio error: %s\n", SDL_GetError());
	else AUDIO_opened(&spec_in);
	
	snd.buffer_seconds = 5;
	snd.sample_rate_in  = sample_rate;
	snd.sample_rate_out = spec_out.freq;
	
	SND_selectResampler();
	SND_resizeBuffer();
	
	SDL_PauseAudio(0);

	LOG_info("sample rate: %i (req) %i (rec) [samples %i]\n", snd.sample_rate_in, snd.sample_rate_out, SAMPLES);
	snd.initialized = 1;
}
void SND_quit(void) { // plat_sound_finish
	if (!snd.initialized) return;
	
	SDL_PauseAudio(1);
	SDL_CloseAudio();
	audio_open = 0;
	snd.initialized = 0;
	
	if (snd.buffer) {
		free(snd.buffer);
		snd.buffer = NULL;
	}
}

///////////////////////////////

// sound effects: synthesized once at the output's sample rate, then mixed in by whichever callback owns the device
#define SFX_VOICES 4
static struct {
	int rate; // what the bank was made for, 0 before
	int16_t* samples[SFX_COUNT]; // mono
	int lengths[SFX_COUNT];
	struct { int sound; int position; } voices[SFX_VOICES]; // sound -1 when free
	int own_device; // opened by SFX for menus (no game audio running)
	int enabled; // menu sounds, -1 until read
} sfx = { .enabled = -1 };

// a decaying sine partial added into out: frequency, start, amplitude (0-1), decay time constant, all in seconds
// (a rotating phasor and a per-sample decay factor, so it's multiplies instead of sinf/expf for every sample)
static void SFX_partial(float* out, int length, int rate, float freq, float start, float amp, float tau) {
	int from = start * rate;
	float step = 2 * (float)M_PI * freq / rate;
	float rc = cosf(step), rs = sinf(step);
	float c = 1, sn = 0; // cos and sin of the phase
	float decay = expf(-1.0f / (tau * rate));
	float level = amp;
	int attack = rate / 1000; // 1ms fade in, no click at the start
	for (int i=from; i<length; i++) {
		int n = i - from;
		out[i] += level * (n<attack ? (float)n/attack : 1) * sn;
		float nc = c*rc - sn*rs;
		sn = sn*rc + c*rs;
		c = nc;
		level *= decay;
		if (level<0.0005f) break; // inaudible from here on
	}
}
// a burst of noise added into out, decaying over tau: the snap of a click. bright (0-1) mixes in more of its
// high end (first difference) over the full band, darker clicks keep more low end
static void SFX_noise(float* out, int length, int rate, float start, float amp, float tau, float bright) {
	int from = start * rate;
	uint32_t seed = 0x1234567; // the same click every time
	float decay = expf(-1.0f / (tau * rate));
	float level = amp;
	float last = 0;
	for (int i=from; i<length; i++) {
		seed = seed * 1664525 + 1013904223;
		float white = ((seed >> 9) / (float)(1 << 23)) * 2 - 1;
		float high = (white - last) * 0.5f;
		last = white;
		out[i] += level * (high * bright + white * (1 - bright));
		level *= decay;
		if (level<0.0005f) break;
	}
}
static void SFX_make(int sound, int rate) {
	float seconds = sound==SFX_ACHIEVEMENT ? 1.2f : 0.05f;
	int length = seconds * rate;
	float* out = calloc(length, sizeof(float));
	switch (sound) {
		case SFX_TICK: // a crisp click: a snap of bright noise with a faint high ping
			SFX_noise(out, length, rate, 0, 0.55f, 0.0012f, 0.85f);
			SFX_partial(out, length, rate, 3800, 0, 0.12f, 0.0015f);
			break;
		case SFX_CONFIRM: // a fuller click with a little low "thock" under it, a beat longer than the tick
			SFX_noise(out, length, rate, 0, 0.55f, 0.0018f, 0.7f);
			SFX_partial(out, length, rate, 2600, 0, 0.10f, 0.002f);
			SFX_partial(out, length, rate, 520, 0, 0.22f, 0.006f);
			break;
		case SFX_BACK: // softer and darker
			SFX_noise(out, length, rate, 0, 0.40f, 0.0015f, 0.4f);
			SFX_partial(out, length, rate, 1500, 0, 0.08f, 0.002f);
			break;
		case SFX_ACHIEVEMENT: { // a rising bell arpeggio: E6, G#6, B6, E7
			float notes[] = { 1318.5f, 1661.2f, 1975.5f, 2637.0f };
			for (int n=0; n<4; n++) {
				float start = n * 0.085f;
				float amp = n==3 ? 0.22f : 0.17f;
				SFX_partial(out, length, rate, notes[n], start, amp, 0.32f);
				SFX_partial(out, length, rate, notes[n]*2.01f, start, amp*0.25f, 0.12f); // bell overtone
				SFX_partial(out, length, rate, notes[n]*3.02f, start, amp*0.08f, 0.06f);
			}
			break;
		}
	}
	sfx.samples[sound] = malloc(length * sizeof(int16_t));
	for (int i=0; i<length; i++) {
		float v = out[i];
		if (v>1) v = 1;
		if (v<-1) v = -1;
		sfx.samples[sound][i] = v * 32767;
	}
	sfx.lengths[sound] = length;
	free(out);
}
static int sfx_channels = 2; // of the device the sounds mix into
static void SFX_mix(int16_t* stream, int frames) {
	if (!sfx.rate) return;
	int channels = sfx_channels;
	for (int v=0; v<SFX_VOICES; v++) {
		int sound = sfx.voices[v].sound;
		if (sound<0) continue;
		int16_t* samples = sfx.samples[sound];
		int position = sfx.voices[v].position;
		int count = MIN(frames, sfx.lengths[sound] - position);
		for (int i=0; i<count; i++) {
			int s = samples[position + i];
			for (int c=0; c<channels; c++) {
				int v = stream[i*channels+c] + s;
				stream[i*channels+c] = v>32767 ? 32767 : (v<-32768 ? -32768 : v);
			}
		}
		sfx.voices[v].position += count;
		if (sfx.voices[v].position>=sfx.lengths[sound]) sfx.voices[v].sound = -1;
	}
}
static void SFX_callback(void* userdata, uint8_t* stream, int len) { // the device SFX opens when no game is playing
	memset(stream, 0, len);
	if (sfx_music) sfx_music((int16_t*)stream, len / (sizeof(int16_t) * 2));
	SFX_mix((int16_t*)stream, len / (sizeof(int16_t) * 2));
}
static pthread_mutex_t sfx_lock = PTHREAD_MUTEX_INITIALIZER; // the warm-up and music threads open the device too
static void SFX_prepareLocked(void);
static void SFX_prepare(void) {
	pthread_mutex_lock(&sfx_lock);
	SFX_prepareLocked();
	pthread_mutex_unlock(&sfx_lock);
}
static int sfx_external_rate = 0; // a device a game opened itself (PICO-8), which the sounds mix into
void SFX_useDevice(int rate, int channels) {
	pthread_mutex_lock(&sfx_lock);
	sfx_external_rate = rate;
	sfx_channels = channels>0 ? channels : 2;
	SFX_prepareLocked();
	pthread_mutex_unlock(&sfx_lock);
}
void SFX_mixInto(int16_t* stream, int frames) { // from that device's callback, with its lock held
	SFX_mix(stream, frames);
}
int SFX_deviceChannels(void) { return sfx_channels; }
static void SFX_prepareLocked(void) {
	int rate;
	if (snd.initialized) rate = snd.sample_rate_out;
	else if (sfx_external_rate) rate = sfx_external_rate;
	else {
		if (!sfx.own_device) {
			SDL_InitSubSystem(SDL_INIT_AUDIO);
			SDL_AudioSpec want = {0}, have;
			want.freq = PLAT_pickSampleRate(44100, MAX_SAMPLE_RATE);
			want.format = AUDIO_S16;
			want.channels = 2;
			want.samples = SAMPLES;
			want.callback = SFX_callback;
			if (SDL_OpenAudio(&want, &have)<0) return;
			AUDIO_opened(&want);
			sfx.own_device = have.freq;
			for (int v=0; v<SFX_VOICES; v++) sfx.voices[v].sound = -1;
			SDL_PauseAudio(0);
			// opening the device puts the output back at full volume, until the volume buttons set it again: set it now
			if (!GetHDMI()) SetVolume(GetVolume());
		}
		rate = sfx.own_device;
	}
	if (sfx.rate==rate) return;
	SDL_LockAudio();
	for (int i=0; i<SFX_COUNT; i++) { // made again, when first played, at the new rate
		if (sfx.samples[i]) free(sfx.samples[i]);
		sfx.samples[i] = NULL;
		sfx.lengths[i] = 0;
	}
	for (int v=0; v<SFX_VOICES; v++) sfx.voices[v].sound = -1;
	sfx.rate = rate;
	SDL_UnlockAudio();
}
int SFX_menuSoundsEnabled(void) {
	if (sfx.enabled==-1) sfx.enabled = exists(MENU_SOUNDS_PATH) ? getInt(MENU_SOUNDS_PATH)!=0 : 1; // on unless turned off
	return sfx.enabled;
}
void SFX_setMenuSounds(int enabled) {
	sfx.enabled = enabled;
	putInt(MENU_SOUNDS_PATH, enabled);
}
// opening the audio device and making the sounds takes a moment (longer on a CPU busy just after boot),
// so the launcher does it on a thread while its first frame is up instead of on the first button press
static pthread_t sfx_thread;
static volatile int sfx_warming = 0;
static int sfx_thread_started = 0;
static void* SFX_warmThread(void* arg) {
	SFX_prepare();
	if (sfx.rate) {
		int sounds[] = { SFX_TICK, SFX_CONFIRM, SFX_BACK };
		for (int i=0; i<3; i++) if (!sfx.samples[sounds[i]]) SFX_make(sounds[i], sfx.rate);
	}
	sfx_warming = 0;
	return NULL;
}
void SFX_warmUp(void) {
	if (sfx_thread_started || snd.initialized || sfx.rate || !SFX_menuSoundsEnabled()) return;
	sfx_warming = 1;
	if (pthread_create(&sfx_thread, NULL, SFX_warmThread, NULL)==0) sfx_thread_started = 1;
	else sfx_warming = 0;
}
static int sfx_for_input = 0;
void SFX_forInput(int enabled) {
	sfx_for_input = enabled;
	if (enabled) SFX_warmUp();
}
void SFX_afterInput(void) {
	if (!sfx_for_input) return;
	if (PAD_justRepeated(BTN_UP) || PAD_justRepeated(BTN_DOWN) || PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT)
		|| PAD_justRepeated(BTN_L1) || PAD_justRepeated(BTN_R1)) SFX_play(SFX_TICK);
	else if (PAD_justPressed(BTN_A)) SFX_play(SFX_CONFIRM);
	else if (PAD_justPressed(BTN_B)) SFX_play(SFX_BACK);
}
void SFX_play(int sound) {
	if (sound<0 || sound>=SFX_COUNT) return;
	if (sound!=SFX_ACHIEVEMENT && !SFX_menuSoundsEnabled()) return;
	if (sfx_warming) return; // not ready yet: skipped rather than waited for
	SFX_prepare();
	if (!sfx.rate) return;
	if (!sfx.samples[sound]) SFX_make(sound, sfx.rate); // only the sounds that get used (the chime is never made in the launcher)
	SDL_LockAudio();
	int voice = 0;
	for (int v=0; v<SFX_VOICES; v++) { // a free voice, or the one furthest along
		if (sfx.voices[v].sound<0) { voice = v; break; }
		if (sfx.voices[v].position>sfx.voices[voice].position) voice = v;
	}
	if (sound==SFX_TICK) { // a quick run of ticks replaces itself instead of piling up
		for (int v=0; v<SFX_VOICES; v++) if (sfx.voices[v].sound==SFX_TICK) { voice = v; break; }
	}
	sfx.voices[voice].sound = sound;
	sfx.voices[voice].position = 0;
	SDL_UnlockAudio();
}
int SFX_open(void) {
	if (snd.initialized) return snd.sample_rate_out; // a game's audio has the device, music mixes into it
	SFX_prepare();
	return sfx.own_device ? sfx.rate : 0;
}
void SFX_setMusic(SFX_Music music) {
	SDL_LockAudio();
	sfx_music = music;
	SDL_UnlockAudio();
}
void SFX_quit(void) {
	if (sfx_thread_started) {
		pthread_join(sfx_thread, NULL);
		sfx_thread_started = 0;
	}
	if (!sfx.own_device) return;
	for (int waited=0; waited<200; waited+=10) { // eg. the confirm sound when a game is launched
		int playing = 0;
		for (int v=0; v<SFX_VOICES; v++) if (sfx.voices[v].sound>=0) playing = 1;
		if (!playing) break;
		SDL_Delay(10);
	}
	SDL_PauseAudio(1);
	SDL_CloseAudio();
	audio_open = 0;
	sfx.own_device = 0;
	sfx.rate = 0;
}

///////////////////////////////

LID_Context lid = {
	.has_lid = 0,
	.is_open = 1,
};

FALLBACK_IMPLEMENTATION void PLAT_setCPUSpeedAsync(int speed) { PLAT_setCPUSpeed(speed); }

FALLBACK_IMPLEMENTATION void PLAT_initLid(void) {  }
FALLBACK_IMPLEMENTATION int PLAT_lidChanged(int* state) { return 0; }

///////////////////////////////

PAD_Context pad;

#define AXIS_DEADZONE 0x4000
void PAD_setAnalog(int neg_id,int pos_id,int value,int repeat_at) {
	// LOG_info("neg %i pos %i value %i\n", neg_id, pos_id, value);
	int neg = 1 << neg_id;
	int pos = 1 << pos_id;	
	if (value>AXIS_DEADZONE) { // pressing
		if (!(pad.is_pressed&pos)) { // not pressing
			pad.is_pressed 		|= pos; // set
			pad.just_pressed	|= pos; // set
			pad.just_repeated	|= pos; // set
			pad.repeat_at[pos_id]= repeat_at;
		
			if (pad.is_pressed&neg) { // was pressing opposite
				pad.is_pressed 		&= ~neg; // unset
				pad.just_repeated 	&= ~neg; // unset
				pad.just_released	|=  neg; // set
			}
		}
	}
	else if (value<-AXIS_DEADZONE) { // pressing
		if (!(pad.is_pressed&neg)) { // not pressing
			pad.is_pressed		|= neg; // set
			pad.just_pressed	|= neg; // set
			pad.just_repeated	|= neg; // set
			pad.repeat_at[neg_id]= repeat_at;
		
			if (pad.is_pressed&pos) { // was pressing opposite
				pad.is_pressed 		&= ~pos; // unset
				pad.just_repeated 	&= ~pos; // unset
				pad.just_released	|=  pos; // set
			}
		}
	}
	else { // not pressing
		if (pad.is_pressed&neg) { // was pressing
			pad.is_pressed 		&= ~neg; // unset
			pad.just_repeated	&=  neg; // unset
			pad.just_released	|=  neg; // set
		}
		if (pad.is_pressed&pos) { // was pressing
			pad.is_pressed 		&= ~pos; // unset
			pad.just_repeated	&=  pos; // unset
			pad.just_released	|=  pos; // set
		}
	}
}

void PAD_reset(void) {
	// LOG_info("PAD_reset");
	pad.just_pressed = BTN_NONE;
	pad.is_pressed = BTN_NONE;
	pad.just_released = BTN_NONE;
	pad.just_repeated = BTN_NONE;
}
FALLBACK_IMPLEMENTATION void PLAT_pollInput(void) {
	// reset transient state
	pad.just_pressed = BTN_NONE;
	pad.just_released = BTN_NONE;
	pad.just_repeated = BTN_NONE;

	uint32_t tick = SDL_GetTicks();
	for (int i=0; i<BTN_ID_COUNT; i++) {
		int btn = 1 << i;
		if ((pad.is_pressed & btn) && (tick>=pad.repeat_at[i])) {
			pad.just_repeated |= btn; // set
			pad.repeat_at[i] += PAD_REPEAT_INTERVAL;
		}
	}
	
	// the actual poll
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		int btn = BTN_NONE;
		int pressed = 0; // 0=up,1=down
		int id = -1;
		if (event.type==SDL_KEYDOWN || event.type==SDL_KEYUP) {
			uint8_t code = event.key.keysym.scancode;
			pressed = event.type==SDL_KEYDOWN;
			// LOG_info("key event: %i (%i)\n", code,pressed);
				 if (code==CODE_UP) 		{ btn = BTN_DPAD_UP; 		id = BTN_ID_DPAD_UP; }
 			else if (code==CODE_DOWN)		{ btn = BTN_DPAD_DOWN; 		id = BTN_ID_DPAD_DOWN; }
			else if (code==CODE_LEFT)		{ btn = BTN_DPAD_LEFT; 		id = BTN_ID_DPAD_LEFT; }
			else if (code==CODE_RIGHT)		{ btn = BTN_DPAD_RIGHT; 	id = BTN_ID_DPAD_RIGHT; }
			else if (code==CODE_A)			{ btn = BTN_A; 				id = BTN_ID_A; }
			else if (code==CODE_B)			{ btn = BTN_B; 				id = BTN_ID_B; }
			else if (code==CODE_X)			{ btn = BTN_X; 				id = BTN_ID_X; }
			else if (code==CODE_Y)			{ btn = BTN_Y; 				id = BTN_ID_Y; }
			else if (code==CODE_START)		{ btn = BTN_START; 			id = BTN_ID_START; }
			else if (code==CODE_SELECT)		{ btn = BTN_SELECT; 		id = BTN_ID_SELECT; }
			else if (code==CODE_MENU)		{ btn = BTN_MENU; 			id = BTN_ID_MENU; }
			else if (code==CODE_MENU_ALT)	{ btn = BTN_MENU; 			id = BTN_ID_MENU; }
			else if (code==CODE_L1)			{ btn = BTN_L1; 			id = BTN_ID_L1; }
			else if (code==CODE_L2)			{ btn = BTN_L2; 			id = BTN_ID_L2; }
			else if (code==CODE_L3)			{ btn = BTN_L3; 			id = BTN_ID_L3; }
			else if (code==CODE_R1)			{ btn = BTN_R1; 			id = BTN_ID_R1; }
			else if (code==CODE_R2)			{ btn = BTN_R2; 			id = BTN_ID_R2; }
			else if (code==CODE_R3)			{ btn = BTN_R3; 			id = BTN_ID_R3; }
			else if (code==CODE_PLUS)		{ btn = BTN_PLUS; 			id = BTN_ID_PLUS; }
			else if (code==CODE_MINUS)		{ btn = BTN_MINUS; 			id = BTN_ID_MINUS; }
			else if (code==CODE_POWER)		{ btn = BTN_POWER; 			id = BTN_ID_POWER; }
			else if (code==CODE_POWEROFF)	{ btn = BTN_POWEROFF;		id = BTN_ID_POWEROFF; } // nano-only
		}
		else if (event.type==SDL_JOYBUTTONDOWN || event.type==SDL_JOYBUTTONUP) {
			uint8_t joy = event.jbutton.button;
			pressed = event.type==SDL_JOYBUTTONDOWN;
			// LOG_info("joy event: %i (%i)\n", joy,pressed);
				 if (joy==JOY_UP) 		{ btn = BTN_DPAD_UP; 		id = BTN_ID_DPAD_UP; }
 			else if (joy==JOY_DOWN)		{ btn = BTN_DPAD_DOWN; 		id = BTN_ID_DPAD_DOWN; }
			else if (joy==JOY_LEFT)		{ btn = BTN_DPAD_LEFT; 		id = BTN_ID_DPAD_LEFT; }
			else if (joy==JOY_RIGHT)	{ btn = BTN_DPAD_RIGHT; 	id = BTN_ID_DPAD_RIGHT; }
			else if (joy==JOY_A)		{ btn = BTN_A; 				id = BTN_ID_A; }
			else if (joy==JOY_B)		{ btn = BTN_B; 				id = BTN_ID_B; }
			else if (joy==JOY_X)		{ btn = BTN_X; 				id = BTN_ID_X; }
			else if (joy==JOY_Y)		{ btn = BTN_Y; 				id = BTN_ID_Y; }
			else if (joy==JOY_START)	{ btn = BTN_START; 			id = BTN_ID_START; }
			else if (joy==JOY_SELECT)	{ btn = BTN_SELECT; 		id = BTN_ID_SELECT; }
			else if (joy==JOY_MENU)		{ btn = BTN_MENU; 			id = BTN_ID_MENU; }
			else if (joy==JOY_MENU_ALT) { btn = BTN_MENU; 			id = BTN_ID_MENU; }
			else if (joy==JOY_MENU_ALT2){ btn = BTN_MENU; 			id = BTN_ID_MENU; }
			else if (joy==JOY_L1)		{ btn = BTN_L1; 			id = BTN_ID_L1; }
			else if (joy==JOY_L2)		{ btn = BTN_L2; 			id = BTN_ID_L2; }
			else if (joy==JOY_L3)		{ btn = BTN_L3; 			id = BTN_ID_L3; }
			else if (joy==JOY_R1)		{ btn = BTN_R1; 			id = BTN_ID_R1; }
			else if (joy==JOY_R2)		{ btn = BTN_R2; 			id = BTN_ID_R2; }
			else if (joy==JOY_R3)		{ btn = BTN_R3; 			id = BTN_ID_R3; }
			else if (joy==JOY_PLUS)		{ btn = BTN_PLUS; 			id = BTN_ID_PLUS; }
			else if (joy==JOY_MINUS)	{ btn = BTN_MINUS; 			id = BTN_ID_MINUS; }
			else if (joy==JOY_POWER)	{ btn = BTN_POWER; 			id = BTN_ID_POWER; }
		}
		else if (event.type==SDL_JOYHATMOTION) {
			int hats[4] = {-1,-1,-1,-1}; // -1=no change,0=up,1=down,2=left,3=right btn_ids
			int hat = event.jhat.value;
			// LOG_info("hat event: %i\n", hat);
			// TODO: safe to assume hats will always be the primary dpad?
			// TODO: this is literally a bitmask, make it one (oh, except there's 3 states...)
			switch (hat) {
				case SDL_HAT_UP:			hats[0]=1;	  hats[1]=0;	hats[2]=0;	  hats[3]=0;	break;
				case SDL_HAT_DOWN:			hats[0]=0;	  hats[1]=1;	hats[2]=0;	  hats[3]=0;	break;
				case SDL_HAT_LEFT:			hats[0]=0;	  hats[1]=0;	hats[2]=1;	  hats[3]=0;	break;
				case SDL_HAT_RIGHT:			hats[0]=0;	  hats[1]=0;	hats[2]=0;	  hats[3]=1;	break;
				case SDL_HAT_LEFTUP:		hats[0]=1;	  hats[1]=0;	hats[2]=1;	  hats[3]=0;	break;
				case SDL_HAT_LEFTDOWN:		hats[0]=0;	  hats[1]=1;	hats[2]=1;	  hats[3]=0;	break;
				case SDL_HAT_RIGHTUP:		hats[0]=1;	  hats[1]=0;	hats[2]=0;	  hats[3]=1;	break;
				case SDL_HAT_RIGHTDOWN:		hats[0]=0;	  hats[1]=1;	hats[2]=0;	  hats[3]=1;	break;
				case SDL_HAT_CENTERED:		hats[0]=0;	  hats[1]=0;	hats[2]=0;	  hats[3]=0;	break;
				default: break;
			}
			
			for (id=0; id<4; id++) {
				int state = hats[id];
				btn = 1 << id;
				if (state==0) {
					pad.is_pressed		&= ~btn; // unset
					pad.just_repeated	&= ~btn; // unset
					pad.just_released	|= btn; // set
				}
				else if (state==1 && (pad.is_pressed & btn)==BTN_NONE) {
					pad.just_pressed	|= btn; // set
					pad.just_repeated	|= btn; // set
					pad.is_pressed		|= btn; // set
					pad.repeat_at[id]	= tick + PAD_REPEAT_DELAY;
				}
			}
			btn = BTN_NONE; // already handled, force continue
		}
		else if (event.type==SDL_JOYAXISMOTION) {
			int axis = event.jaxis.axis;
			int val = event.jaxis.value;
			// LOG_info("axis: %i (%i)\n", axis,val);
			
			// triggers on tg5040
			if (axis==AXIS_L2) {
				btn = BTN_L2;
				id = BTN_ID_L2;
				pressed = val>0;
			}
			else if (axis==AXIS_R2) {
				btn = BTN_R2;
				id = BTN_ID_R2;
				pressed = val>0;
			}
			
			else if (axis==AXIS_LX) { pad.laxis.x = val; PAD_setAnalog(BTN_ID_ANALOG_LEFT, BTN_ID_ANALOG_RIGHT, val, tick+PAD_REPEAT_DELAY); }
			else if (axis==AXIS_LY) { pad.laxis.y = val; PAD_setAnalog(BTN_ID_ANALOG_UP,   BTN_ID_ANALOG_DOWN,  val, tick+PAD_REPEAT_DELAY); }
			else if (axis==AXIS_RX) pad.raxis.x = val;
			else if (axis==AXIS_RY) pad.raxis.y = val;
			
			// axis will fire off what looks like a release
			// before the first press but you can't release
			// a button that wasn't pressed
			if (!pressed && btn!=BTN_NONE && !(pad.is_pressed & btn)) {
				// LOG_info("cancel: %i\n", axis);
				btn = BTN_NONE;
			}
		}
		// else if (event.type==SDL_QUIT) PWR_powerOff(); // added for macOS debug
		
		if (btn==BTN_NONE) continue;
		
		if (!pressed) {
			pad.is_pressed		&= ~btn; // unset
			pad.just_repeated	&= ~btn; // unset
			pad.just_released	|= btn; // set
		}
		else if ((pad.is_pressed & btn)==BTN_NONE) {
			pad.just_pressed	|= btn; // set
			pad.just_repeated	|= btn; // set
			pad.is_pressed		|= btn; // set
			pad.repeat_at[id]	= tick + PAD_REPEAT_DELAY;
		}
	}
	
	if (lid.has_lid && PLAT_lidChanged(NULL)) pad.just_released |= BTN_SLEEP;
}
FALLBACK_IMPLEMENTATION int PLAT_shouldWake(void) {
	int lid_open = 1; // assume open by default
	if (lid.has_lid && PLAT_lidChanged(&lid_open) && lid_open) return 1;
	
	
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type==SDL_KEYUP) {
			uint8_t code = event.key.keysym.scancode;
			if ((BTN_WAKE==BTN_POWER && code==CODE_POWER) || (BTN_WAKE==BTN_MENU && (code==CODE_MENU || code==CODE_MENU_ALT))) {
				// ignore input while lid is closed
				if (lid.has_lid && !lid.is_open) return 0;  // do it here so we eat the input
				return 1;
			}
		}
		else if (event.type==SDL_JOYBUTTONUP) {
			uint8_t joy = event.jbutton.button;
			if ((BTN_WAKE==BTN_POWER && joy==JOY_POWER) || (BTN_WAKE==BTN_MENU && (joy==JOY_MENU || joy==JOY_MENU_ALT))) {
				// ignore input while lid is closed
				if (lid.has_lid && !lid.is_open) return 0;  // do it here so we eat the input
				return 1;
			}
		} 
	}
	return 0;
}

int PAD_anyJustPressed(void)	{ return pad.just_pressed!=BTN_NONE; }
int PAD_anyPressed(void)		{ return pad.is_pressed!=BTN_NONE; }
int PAD_anyJustReleased(void)	{ return pad.just_released!=BTN_NONE; }

int PAD_justPressed(int btn)	{ return pad.just_pressed & btn; }
int PAD_isPressed(int btn)		{ return pad.is_pressed & btn; }
int PAD_justReleased(int btn)	{ return pad.just_released & btn; }
int PAD_justRepeated(int btn)	{ return pad.just_repeated & btn; }

int PAD_tappedMenu(uint32_t now) {
	#define MENU_DELAY 250 // also in PWR_update()
	static uint32_t menu_start = 0;
	static int ignore_menu = 0; 
	if (PAD_justPressed(BTN_MENU)) {
		ignore_menu = 0;
		menu_start = now;
	}
	else if (PAD_isPressed(BTN_MENU) && BTN_MOD_BRIGHTNESS==BTN_MENU && (PAD_justPressed(BTN_MOD_PLUS) || PAD_justPressed(BTN_MOD_MINUS))) {
		ignore_menu = 1;
	}
	else if (PAD_isPressed(BTN_MENU) && PAD_justPressed(BTN_ANY)) ignore_menu = 1; // part of a combo (eg. MENU + X), not a tap
	return (!ignore_menu && PAD_justReleased(BTN_MENU) && now-menu_start<MENU_DELAY);
}

///////////////////////////////

static struct VIB_Context {
	int initialized;
	pthread_t pt;
	int queued_strength;
	int strength;
} vib = {0};
static void* VIB_thread(void *arg) {
#define DEFER_FRAMES 3
	static int defer = 0;
	while(1) {
		SDL_Delay(17);
		if (vib.queued_strength!=vib.strength) {
			if (defer<DEFER_FRAMES && vib.queued_strength==0) { // minimize vacillation between 0 and some number (which this motor doesn't like)
				defer += 1;
				continue;
			}
			vib.strength = vib.queued_strength;
			defer = 0;

			PLAT_setRumble(vib.strength);
		}
	}
	return 0;
}
void VIB_init(void) {
	vib.queued_strength = vib.strength = 0;
	pthread_create(&vib.pt, NULL, &VIB_thread, NULL);
	vib.initialized = 1;
}
void VIB_quit(void) {
	if (!vib.initialized) return;
	
	VIB_setStrength(0);
	pthread_cancel(vib.pt);
	pthread_join(vib.pt, NULL);
	PLAT_setRumble(0); // VIB_setStrength is queued; make certain the motor stops before exit
}
void VIB_setStrength(int strength) {
	if (vib.queued_strength==strength) return;
	vib.queued_strength = strength;
}
int VIB_coreHasRumble(const char* core) {
	char* cores[] = { "gambatte", "gpsp", "mgba", "pcsx_rearmed", "pokemini", NULL };
	for (int i=0; cores[i]; i++) if (exactMatch((char*)core, cores[i])) return 1;
	return 0;
}
int VIB_getStrength(void) {
	return vib.strength;
}

struct VIB_Pulse {
	int strength;
	int milliseconds;
	unsigned generation;
};
static pthread_mutex_t vib_pulse_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned vib_pulse_generation = 0;
static void* VIB_pulseThread(void* arg) {
	struct VIB_Pulse* pulse = arg;
	int strength = pulse->strength;
	int milliseconds = pulse->milliseconds;
	unsigned generation = pulse->generation;
	free(pulse);

	PLAT_setRumble(strength);
	SDL_Delay(milliseconds);
	pthread_mutex_lock(&vib_pulse_mutex);
	if (generation==vib_pulse_generation) PLAT_setRumble(vib.initialized ? vib.strength : 0);
	pthread_mutex_unlock(&vib_pulse_mutex);
	return NULL;
}
void VIB_pulse(int strength, int milliseconds) {
	if (strength<=0 || milliseconds<=0) return;
	struct VIB_Pulse* pulse = malloc(sizeof(struct VIB_Pulse));
	if (!pulse) return;
	pulse->strength = strength;
	pulse->milliseconds = milliseconds;
	pthread_mutex_lock(&vib_pulse_mutex);
	pulse->generation = ++vib_pulse_generation;
	pthread_mutex_unlock(&vib_pulse_mutex);
	pthread_t thread;
	if (pthread_create(&thread, NULL, VIB_pulseThread, pulse)==0) pthread_detach(thread);
	else free(pulse);
}

///////////////////////////////

static void PWR_initOverlay(void) {
	// setup surface
	pwr.overlay = PLAT_initOverlay();

	// draw battery
	SDLX_SetAlpha(gfx.assets, 0,0);
	GFX_blitAsset(ASSET_BLACK_PILL, NULL, pwr.overlay, NULL);
	SDLX_SetAlpha(gfx.assets, SDL_SRCALPHA,0);
	GFX_blitBattery(pwr.overlay, NULL);
}

static void PWR_updateBatteryStatus(void) {
	PLAT_getBatteryStatus(&pwr.is_charging, &pwr.charge);
	PLAT_enableOverlay(pwr.should_warn && pwr.charge<=PWR_LOW_CHARGE);
}

static void* PWR_monitorBattery(void *arg) {
	while(1) {
		// TODO: the frequency of checking could depend on whether 
		// we're in game (less frequent) or menu (more frequent)
		sleep(5);
		PWR_updateBatteryStatus();
	}
	return NULL;
}

///////////////////////////////

// seconds, 0 means never
static const struct {
	const char* key;
	int default_index;
	int count;
	int seconds[8];
	const char* labels[8];
} pwr_timers[PWR_TIMER_COUNT] = {
	[PWR_TIMER_SLEEP] = { "sleep", 0, 6,
		{ 30, 60, 120, 300, 600, 0 },
		{ "30 sec", "1 min", "2 min", "5 min", "10 min", "Never" },
	},
	[PWR_TIMER_POWEROFF] = { "poweroff", 1, 8,
		{ 60, 120, 300, 600, 900, 1800, 3600, 0 },
		{ "1 min", "2 min", "5 min", "10 min", "15 min", "30 min", "60 min", "Never" },
	},
};
static void PWR_loadTimers(void);
static uint32_t PWR_getTimerMs(int timer) {
	if (!pwr.timers_loaded) PWR_loadTimers();
	return pwr_timers[timer].seconds[pwr.timers[timer]] * 1000;
}
static void PWR_loadTimers(void) {
	pwr.timers_loaded = 1;
	for (int i=0; i<PWR_TIMER_COUNT; i++) pwr.timers[i] = pwr_timers[i].default_index;

	FILE* file = fopen(POWER_PATH, "r");
	if (!file) return;
	char line[64];
	while (fgets(line, sizeof(line), file)) {
		char* value = strchr(line, '=');
		if (!value) continue;
		*value++ = '\0';
		int seconds = atoi(value);
		for (int i=0; i<PWR_TIMER_COUNT; i++) {
			if (!exactMatch(line, (char*)pwr_timers[i].key)) continue;
			for (int j=0; j<pwr_timers[i].count; j++) {
				if (pwr_timers[i].seconds[j]==seconds) pwr.timers[i] = j; // unknown values keep the default
			}
		}
	}
	fclose(file);
}
static void PWR_saveTimers(void) {
	FILE* file = fopen(POWER_PATH, "w");
	if (!file) return;
	for (int i=0; i<PWR_TIMER_COUNT; i++) {
		fprintf(file, "%s=%i\n", pwr_timers[i].key, pwr_timers[i].seconds[pwr.timers[i]]);
	}
	fclose(file);
	sync();
}
int PWR_getTimerCount(int timer) {
	return pwr_timers[timer].count;
}
int PWR_getTimer(int timer) {
	if (!pwr.timers_loaded) PWR_loadTimers();
	return pwr.timers[timer];
}
void PWR_setTimer(int timer, int index) {
	if (!pwr.timers_loaded) PWR_loadTimers();
	pwr.timers[timer] = index;
	PWR_saveTimers();
}
const char* PWR_getTimerLabel(int timer, int index) {
	return pwr_timers[timer].labels[index];
}

void PWR_init(void) {
	PWR_loadTimers();

	pwr.can_sleep = 1;
	pwr.can_poweroff = 1;
	pwr.can_autosleep = 1;
	
	pwr.requested_sleep = 0;
	pwr.requested_wake = 0;
	
	pwr.should_warn = 0;
	pwr.charge = PWR_LOW_CHARGE;
	
	PWR_initOverlay();

	PWR_updateBatteryStatus();
	pthread_create(&pwr.battery_pt, NULL, &PWR_monitorBattery, NULL);
	pwr.initialized = 1;
}
void PWR_quit(void) {
	if (!pwr.initialized) return;
	
	PLAT_quitOverlay();
	
	// cancel battery thread
	pthread_cancel(pwr.battery_pt);
	pthread_join(pwr.battery_pt, NULL);
}
void PWR_warn(int enable) {
	pwr.should_warn = enable;
	PLAT_enableOverlay(pwr.should_warn && pwr.charge<=PWR_LOW_CHARGE);
}

int PWR_ignoreSettingInput(int btn, int show_setting) {
	return show_setting && (btn==BTN_MOD_PLUS || btn==BTN_MOD_MINUS);
}

void PWR_update(int* _dirty, int* _show_setting, PWR_callback_t before_sleep, PWR_callback_t after_sleep) {
	int dirty = _dirty ? *_dirty : 0;
	int show_setting = _show_setting ? *_show_setting : 0;
	
	static uint32_t last_input_at = 0; // timestamp of last input (autosleep)
	static uint32_t checked_charge_at = 0; // timestamp of last time checking charge
	static uint32_t setting_shown_at = 0; // timestamp when settings started being shown
	static uint32_t power_pressed_at = 0; // timestamp when power button was just pressed
	static uint32_t mod_unpressed_at = 0; // timestamp of last time settings modifier key was NOT down
	static uint32_t was_muted = -1;
	if (was_muted==-1) was_muted = GetMute();
	
	static int was_charging = -1;
	if (was_charging==-1) was_charging = pwr.is_charging;

	uint32_t now = SDL_GetTicks();
	if (was_charging || PAD_anyPressed() || last_input_at==0) last_input_at = now;
	
	#define CHARGE_DELAY 1000
	if (dirty || now-checked_charge_at>=CHARGE_DELAY) {
		int is_charging = pwr.is_charging;
		if (was_charging!=is_charging) {
			was_charging = is_charging;
			dirty = 1;
		}
		checked_charge_at = now;
	}
	
	if (PAD_justReleased(BTN_POWEROFF) || (power_pressed_at && now-power_pressed_at>=1000)) {
		if (before_sleep) before_sleep();
		PWR_powerOff();
	}
	
	if (PAD_justPressed(BTN_POWER)) {
		power_pressed_at = now;
	}
	
	uint32_t sleep_delay = PWR_getTimerMs(PWR_TIMER_SLEEP); // 0 means never
	int autosleep_due = sleep_delay && now-last_input_at>=sleep_delay;
	if (autosleep_due && PWR_preventAutosleep()) {
		last_input_at = now;
		autosleep_due = 0;
	}

	if (
		pwr.requested_sleep || // hardware requested sleep
		autosleep_due || // autosleep
		(pwr.can_sleep && PAD_justReleased(BTN_SLEEP)) // manual sleep
	) {
		pwr.requested_sleep = 0;
		if (before_sleep) before_sleep();
		PWR_fauxSleep();
		if (after_sleep) after_sleep();
		
		last_input_at = now = SDL_GetTicks();
		power_pressed_at = 0;
		dirty = 1;
	}
	
	int was_dirty = dirty; // dirty list (not including settings/battery)
	
	// TODO: only delay hiding setting changes if that setting didn't require a modifier button be held, otherwise release as soon as modifier is released
	
	int delay_settings = BTN_MOD_BRIGHTNESS==BTN_MENU; // when both volume and brighness require a modifier hide settings as soon as it is released
	#define SETTING_DELAY 500
	if (show_setting && (now-setting_shown_at>=SETTING_DELAY || !delay_settings) && !PAD_isPressed(BTN_MOD_VOLUME) && !PAD_isPressed(BTN_MOD_BRIGHTNESS)) {
		show_setting = 0;
		dirty = 1;
	}
	
	if (!show_setting && !PAD_isPressed(BTN_MOD_VOLUME) && !PAD_isPressed(BTN_MOD_BRIGHTNESS)) {
		mod_unpressed_at = now; // this feels backwards but is correct
	}
	
	#define MOD_DELAY 250
	if (
		(
			(PAD_isPressed(BTN_MOD_VOLUME) || PAD_isPressed(BTN_MOD_BRIGHTNESS)) && 
			(PAD_justRepeated(BTN_MOD_PLUS) || PAD_justRepeated(BTN_MOD_MINUS)) &&
			(!delay_settings || now-mod_unpressed_at>=MOD_DELAY)
		) || 
		((!BTN_MOD_VOLUME || !BTN_MOD_BRIGHTNESS) && (PAD_justRepeated(BTN_MOD_PLUS) || PAD_justRepeated(BTN_MOD_MINUS)))
	) {
		setting_shown_at = now;
		if (PAD_isPressed(BTN_MOD_BRIGHTNESS)) {
			show_setting = 1;
		}
		else {
			show_setting = 2;
		}
	}
	
	int muted = GetMute();
	if (muted!=was_muted) {
		was_muted = muted;
		show_setting = 2;
		setting_shown_at = now;
	}
	
	// keymon changes the shared settings asynchronously; click when a step actually lands.
	static int last_volume = -1, last_brightness = -1;
	int volume = GetVolume(), brightness = GetBrightness();
	if (show_setting && last_volume>=0 && last_brightness>=0 &&
		(volume!=last_volume || brightness!=last_brightness)) SFX_play(SFX_TICK);
	last_volume = volume;
	last_brightness = brightness;
	if (show_setting) dirty = 1; // shm is slow or keymon is catching input on the next frame
	if (clock_drawn_minute && GFX_getClockMode() && time(NULL) / 60 != clock_drawn_minute) dirty = 1; // the clock moved on
	static uint32_t clock_checked = 0;
	if (now - clock_checked > 2000) { // game RTC needs the time zone even with the clock hidden
		clock_checked = now;
		if (refreshClock()) dirty = 1;
	}
	int was_showing = _show_setting ? *_show_setting : 0;
	if (was_showing && !show_setting) setting_fade_until = now + SETTING_FADE_MS; // GFX_blitHardwareGroup fades it out
	if (setting_fade_until) { // frames while it fades, and one more once it's gone so none of it is left on screen
		dirty = 1;
		if ((int32_t)(setting_fade_until - now)<=0) setting_fade_until = 0;
	}
	if (_dirty) *_dirty = dirty;
	if (_show_setting) *_show_setting = show_setting;
}

// TODO: this isn't whether it can sleep but more if it should sleep in response to the sleep button
void PWR_disableSleep(void) {
	pwr.can_sleep = 0;
}
void PWR_enableSleep(void) {
	pwr.can_sleep = 1;
}

void PWR_disablePowerOff(void) {
	pwr.can_poweroff = 0;
}
void PWR_powerOff(void) {
	if (pwr.can_poweroff) {
		
		int w = FIXED_WIDTH;
		int h = FIXED_HEIGHT;
		int p = FIXED_PITCH;
		if (GetHDMI()) {
			w = HDMI_WIDTH;
			h = HDMI_HEIGHT;
			p = HDMI_PITCH;
		}
		gfx.screen = GFX_resize(w,h,p);
		
		char* msg;
		if (HAS_POWER_BUTTON || HAS_POWEROFF_BUTTON) msg = exists(AUTO_RESUME_PATH) ? "Quicksave created,\npowering off" : "Powering off";
		else msg = exists(AUTO_RESUME_PATH) ? "Quicksave created,\npower off now" : "Power off now";
		
		// LOG_info("PWR_powerOff %s (%ix%i)\n", gfx.screen, gfx.screen->w, gfx.screen->h);
		
		// TODO: for some reason screen's dimensions end up being 0x0 in GFX_blitMessage...
		PLAT_clearVideo(gfx.screen);
		GFX_blitMessage(font.large, msg, gfx.screen,&(SDL_Rect){0,0,gfx.screen->w,gfx.screen->h}); //, NULL);
		GFX_flip(gfx.screen);
		PLAT_powerOff();
	}
}

static void PWR_enterSleep(void) {
	// closed rather than paused: paused, SDL still writes silence to the sound chip ~100 times a second
	if (audio_open) {
		SDL_CloseAudio();
		audio_open = 0;
		audio_slept = 1;
	}
	else SDL_PauseAudio(1);
	PLAT_powerSave(POWER_ASLEEP);
	if (GetHDMI()) {
		PLAT_clearVideo(gfx.screen);
		PLAT_flip(gfx.screen, 0);
	}
	else {
		SetRawVolume(MUTE_VOLUME_RAW);
		PLAT_enableBacklight(0);
	}
	system("killall -STOP keymon.elf");
	
	sync();
}
static void PWR_exitSleep(void) {
	PLAT_powerSave(POWER_AWAKE);
	refreshClock();
	if (audio_slept) { // the same as before sleep, so its rate etc. hold
		SDL_AudioSpec have;
		audio_slept = 0;
		audio_open = 0;
		if (SDL_OpenAudio(&audio_want, &have)==0) audio_open = 1;
	}
	system("killall -CONT keymon.elf");
	if (GetHDMI()) {
		// buh
	}
	else {
		PLAT_enableBacklight(1);
		SetVolume(GetVolume());
	}
	SDL_PauseAudio(0);
	
	sync();
}

#define DEEP_SLEEP_MS 60000 // asleep this long, wifi goes off too (back on when woken)
static void PWR_waitForWake(void) {
	uint32_t sleep_ticks = SDL_GetTicks();
	uint32_t poweroff_delay = PWR_getTimerMs(PWR_TIMER_POWEROFF); // 0 means never
	int deep = 0;
	while (!PAD_wake()) {
		if (!deep && SDL_GetTicks()-sleep_ticks>=DEEP_SLEEP_MS) { // a real break, not a moment: more goes off
			PLAT_powerSave(POWER_ASLEEP_LONG);
			deep = 1;
		}
		if (pwr.requested_wake) {
			pwr.requested_wake = 0;
			break;
		}
		SDL_Delay(200);
		if (pwr.can_poweroff && poweroff_delay && SDL_GetTicks()-sleep_ticks>=poweroff_delay) {
			if (pwr.is_charging) sleep_ticks += 60000; // check again in a minute
			else PWR_powerOff();
		}
	}
	
	return;
}
void PWR_fauxSleep(void) {
	GFX_clear(gfx.screen);
	PAD_reset();
	PWR_enterSleep();
	PWR_waitForWake();
	PWR_exitSleep();
	PAD_reset();
}

void PWR_disableAutosleep(void) {
	pwr.can_autosleep = 0;
}
void PWR_enableAutosleep(void) {
	pwr.can_autosleep = 1;
}
int PWR_preventAutosleep(void) {
	return pwr.is_charging || !pwr.can_autosleep || GetHDMI();
}

// updated by PWR_updateBatteryStatus()
int PWR_isCharging(void) {
	return pwr.is_charging;
}
int PWR_getBattery(void) { // 10-100 in 10-20% fragments
	return pwr.charge;
}

///////////////////////////////

// TODO: tmp? move to individual platforms or allow overriding like PAD_poll/PAD_wake?
int PLAT_setDateTime(int y, int m, int d, int h, int i, int s) {
	char cmd[1024];
	// (in TZ, when one's picked) into the clock that runs while the A30 is off. It's saved too, or a time set back
	// would look like a clock that stopped at the next boot (launch.sh)
	sprintf(cmd, "date -s '%d-%d-%d %d:%d:%d'; hwclock --utc -w; TZ=UTC0 date +'%%F %%T' > \"$DATETIME_PATH\"; "
		"touch " CLOCK_SET_PATH " '" CLOCK_SYNCED_PATH "'; "
		"if [ -f '" TIME_ZONE_PATH "' ]; then rm -f '" CLOCK_MANUAL_PATH "'; else touch '" CLOCK_MANUAL_PATH "'; fi; sync", y,m,d,h,i,s);
	system(cmd);
	refreshClock();
	return 0; // why does this return an int?
}
