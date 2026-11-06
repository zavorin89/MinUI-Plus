#ifndef __API_H__
#define __API_H__
#include "sdl.h"
#include "platform.h"
#include "scaler.h"

///////////////////////////////

enum {
	LOG_DEBUG = 0,
	LOG_INFO,
	LOG_WARN,
	LOG_ERROR,
};

#define LOG_debug(fmt, ...) LOG_note(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_info(fmt, ...) LOG_note(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_warn(fmt, ...) LOG_note(LOG_WARN, fmt, ##__VA_ARGS__)
#define LOG_error(fmt, ...) LOG_note(LOG_ERROR, fmt, ##__VA_ARGS__)
void LOG_note(int level, const char* fmt, ...);

///////////////////////////////

#define PAGE_COUNT	2
#ifndef PAGE_SCALE
#define PAGE_SCALE	3
#endif
#define PAGE_WIDTH	(FIXED_WIDTH * PAGE_SCALE)
#define PAGE_HEIGHT	(FIXED_HEIGHT * PAGE_SCALE)
#define PAGE_PITCH	(PAGE_WIDTH * FIXED_BPP)
#define PAGE_SIZE	(PAGE_PITCH * PAGE_HEIGHT)

///////////////////////////////

// TODO: these only seem to be used by a tmp.pak in trimui (model s)
// used by minarch, optionally defined in platform.h
#ifndef PLAT_PAGE_BPP
#define PLAT_PAGE_BPP 	FIXED_BPP
#endif
#define PLAT_PAGE_DEPTH (PLAT_PAGE_BPP * 8)
#define PLAT_PAGE_PITCH (PAGE_WIDTH * PLAT_PAGE_BPP)
#define PLAT_PAGE_SIZE	(PLAT_PAGE_PITCH * PAGE_HEIGHT)

///////////////////////////////

#define RGBA_MASK_AUTO	0x0, 0x0, 0x0, 0x0
#define RGBA_MASK_565	0xF800, 0x07E0, 0x001F, 0x0000
#define RGBA_MASK_8888	0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000

///////////////////////////////

#define FALLBACK_IMPLEMENTATION __attribute__((weak)) // used if platform doesn't provide an implementation

///////////////////////////////

// themes tint everything white and grey with a light color, named after food
typedef struct ThemeColors {
	SDL_Color white;
	SDL_Color gray;
	SDL_Color light_text;
	SDL_Color dark_text;
} ThemeColors;
extern ThemeColors theme_colors;
int GFX_getThemeCount(void);
const char* GFX_getThemeName(int index);
int GFX_getTheme(void); // index of the current theme
void GFX_setTheme(int index); // recolors everything drawn from here on and saves the choice

extern uint32_t RGB_WHITE;
extern uint32_t RGB_BLACK;
extern uint32_t RGB_LIGHT_GRAY;
extern uint32_t RGB_GRAY;
extern uint32_t RGB_DARK_GRAY;

enum {
	ASSET_WHITE_PILL,
	ASSET_BLACK_PILL,
	ASSET_DARK_GRAY_PILL,
	ASSET_OPTION,
	ASSET_BUTTON,
	ASSET_PAGE_BG,
	ASSET_STATE_BG,
	ASSET_PAGE,
	ASSET_BAR,
	ASSET_BAR_BG,
	ASSET_BAR_BG_MENU,
	ASSET_UNDERLINE,
	ASSET_DOT,
	ASSET_HOLE,
	
	ASSET_COLORS,
	
	ASSET_BRIGHTNESS,
	ASSET_VOLUME_MUTE,
	ASSET_VOLUME,
	ASSET_BATTERY,
	ASSET_BATTERY_LOW,
	ASSET_BATTERY_FILL,
	ASSET_BATTERY_FILL_LOW,
	ASSET_BATTERY_BOLT,
	
	ASSET_SCROLL_UP,
	ASSET_SCROLL_DOWN,
	
	ASSET_WIFI,
	
	ASSET_COUNT,
};

typedef struct GFX_Fonts {
	TTF_Font* large; 	// menu
	TTF_Font* medium; 	// single char button label
	TTF_Font* small; 	// button hint
	TTF_Font* tiny; 	// multi char button label
} GFX_Fonts;
extern GFX_Fonts font;

enum {
	SHARPNESS_SHARP,
	SHARPNESS_CRISP,
	SHARPNESS_SOFT,
};

enum {
	EFFECT_NONE,
	EFFECT_LINE,
	EFFECT_GRID,
	EFFECT_COUNT,
};

typedef struct GFX_Renderer {
	void* src;
	void* dst;
	void* blit;
	double aspect; // 0 for integer, -1 for fullscreen, otherwise aspect ratio, used for SDL2 accelerated scaling
	int scale;
	
	// TODO: document this better
	int true_w;
	int true_h;

	int src_x;
	int src_y;
	int src_w;
	int src_h;
	int src_p;
	
	// TODO: I think this is overscaled
	int dst_x;
	int dst_y;
	int dst_w;
	int dst_h;
	int dst_p;
} GFX_Renderer;

enum {
	MODE_MAIN,
	MODE_MENU,
};

SDL_Surface* GFX_init(int mode);
#define GFX_resize PLAT_resizeVideo // (int w, int h, int pitch);
#define GFX_setScaleClip PLAT_setVideoScaleClip // (int x, int y, int width, int height)
#define GFX_setNearestNeighbor PLAT_setNearestNeighbor // (int enabled)
#define GFX_setSharpness PLAT_setSharpness // (int sharpness)
#define GFX_setEffectColor PLAT_setEffectColor // (int color)
#define GFX_setEffect PLAT_setEffect // (int effect)
void GFX_setMode(int mode);
int GFX_hdmiChanged(void);

#define GFX_clear PLAT_clearVideo // (SDL_Surface* screen)
#define GFX_clearAll PLAT_clearAll // (void)

void GFX_startFrame(void);
void GFX_flip(SDL_Surface* screen);
#define GFX_supportsOverscan PLAT_supportsOverscan // (void)
#define GFX_supportsBorder PLAT_supportsBorder // (void)
#define GFX_setBorder PLAT_setBorder // (const char* path) NULL clears it
#define GFX_setNotification PLAT_setNotification // (SDL_Surface* full screen with transparency) NULL hides it
#define GFX_setNotificationAlpha PLAT_setNotificationAlpha // (int 0-255) fades the notification, kept for the next one
#define GFX_captureScreen PLAT_captureScreen // () the game as it looks on screen, or NULL
#define GFX_presentLastFrame PLAT_presentLastFrame // ()
void GFX_sync(void); // call this to maintain 60fps when not calling GFX_flip() this frame
void GFX_quit(void);

enum {
	VSYNC_OFF = 0,
	VSYNC_LENIENT, // default
	VSYNC_STRICT,
};

int GFX_getVsync(void);
void GFX_setVsync(int vsync);

int GFX_blitTitle(SDL_Surface* dst, const char* name, int max_width); // a game name that scrolls when too long; 1 while it does
int GFX_truncateText(TTF_Font* font, const char* in_name, char* out_name, int max_width, int padding); // returns final width
int GFX_wrapText(TTF_Font* font, char* str, int max_width, int max_lines);
int GFX_wrapTextEnding(TTF_Font* font, char* str, int max_width, int max_lines, const char* ending);

#define GFX_getScaler PLAT_getScaler		// scaler_t:(GFX_Renderer* renderer)
#define GFX_blitRenderer PLAT_blitRenderer	// void:(GFX_Renderer* renderer)

scaler_t GFX_getAAScaler(GFX_Renderer* renderer);
void GFX_freeAAScaler(void);

// NOTE: all dimensions should be pre-scaled
void GFX_blitAsset(int asset, SDL_Rect* src_rect, SDL_Surface* dst, SDL_Rect* dst_rect);
void GFX_blitPill(int asset, SDL_Surface* dst, SDL_Rect* dst_rect);
void GFX_blitRect(int asset, SDL_Surface* dst, SDL_Rect* dst_rect);
void GFX_blitBattery(SDL_Surface* dst, SDL_Rect* dst_rect);
int GFX_getButtonWidth(char* hint, char* button);
void GFX_blitButton(char* hint, char*button, SDL_Surface* dst, SDL_Rect* dst_rect);
void GFX_blitMessage(TTF_Font* font, char* msg, SDL_Surface* dst, SDL_Rect* dst_rect);

int GFX_blitHardwareGroup(SDL_Surface* dst, int show_setting);
int GFX_blitHeader(SDL_Surface* dst, char* title, int ow); // a screen's title, grey, on the top row beside the hardware group (ow: its width); 1 while it scrolls (too long), so draw it again each frame
int GFX_blitHeaderText(SDL_Surface* dst, TTF_Font* font, SDL_Color color, const char* title, int ow); // the same in another font or color
void GFX_showList(SDL_Surface* dst, const char* title, char** names, char** notes, int count); // a list to read through, names with a note each (notes may be NULL); A or B closes it
int GFX_settingFading(void); // the volume/brightness bar is still fading out (GFX_blitHardwareGroup draws it)
enum { CLOCK_OFF, CLOCK_12H, CLOCK_24H };
enum { SLEEP_LIGHT_OFF, SLEEP_LIGHT_ON, SLEEP_LIGHT_COUNT }; // (the A30's LED is on or off: it doesn't dim)
int PWR_getSleepLight(void); // SLEEP_LIGHT_ON unless changed
void PWR_setSleepLight(int level); // also saves it
int GFX_getClockMode(void); // the clock in the hardware group (CLOCK_12H unless changed)
void GFX_setClockMode(int mode); // also saves it
// the time zone the clock syncs over Wi-Fi in: -1 for automatic (looked up from the connection at the next sync),
// -2 for one that was looked up and isn't in the list (shown as its offset)
int GFX_getTimeZoneCount(void);
const char* GFX_getTimeZoneName(int zone);
int GFX_getTimeZone(void);
void GFX_setTimeZone(int zone); // saved, used right away, and the clock syncs if Wi-Fi is on
void GFX_setClockLeft(int left); // the clock in its own pill at the top left instead (eg. the carousel)
void GFX_blitHardwareHints(SDL_Surface* dst, int show_setting);
int GFX_blitButtonGroup(char** hints, int primary, SDL_Surface* dst, int align_right);

void GFX_sizeText(TTF_Font* font, char* str, int leading, int* w, int* h);
void GFX_blitText(TTF_Font* font, char* str, int leading, SDL_Color color, SDL_Surface* dst, SDL_Rect* dst_rect);

///////////////////////////////

typedef struct SND_Frame {
	int16_t left;
	int16_t right;
} SND_Frame;

void SND_init(double sample_rate, double frame_rate);
size_t SND_batchSamples(const SND_Frame* frames, size_t frame_count);
uint64_t SND_takeWaitUs(void); // microseconds SND_batchSamples spent waiting for room since the last call
uint64_t SND_waitedUs(void); // microseconds SND_batchSamples has spent waiting for room in all
void SND_setRate(double sample_rate); // the core's rate changed while it runs (an N64 game sets its own)
void SND_setBufferFrames(int frames); // how many video frames of sound the ring holds
int SND_queuedFrames(void); // video frames of sound waiting to play
int SND_takeUnderruns(void); // times the ring ran dry since the last call
void SND_quit(void);

// short synthesized sounds, mixed into the game's audio when it's playing or on their own device otherwise
enum {
	SFX_TICK, // moving the selection
	SFX_CONFIRM, // opening, choosing
	SFX_BACK, // going back
	SFX_ACHIEVEMENT, // a RetroAchievements unlock (plays even with menu sounds off)
	SFX_COUNT,
};
void SFX_play(int sound);
void SFX_warmUp(void); // opens the device and makes the menu sounds on a thread, so the first one plays without a wait
int SFX_menuSoundsEnabled(void);
void SFX_setMenuSounds(int enabled); // also saves the choice
void SFX_quit(void); // lets a sound that's playing finish (briefly), then closes its device
typedef void (*SFX_Music)(int16_t* stream, int frames); // adds stereo frames into stream, on the audio thread
int SFX_open(void); // opens the menu sounds' device (or uses a game's), returns its sample rate or 0
void SFX_setMusic(SFX_Music music); // NULL stops calling it

///////////////////////////////

typedef struct LID_Context {
	int has_lid;
	int is_open;
} LID_Context;
extern LID_Context lid;

void PLAT_initLid(void);
int PLAT_lidChanged(int* state);

///////////////////////////////

typedef struct PAD_Axis {
		int x;
		int y;
} PAD_Axis;
typedef struct PAD_Context {
	int is_pressed;
	int just_pressed;
	int just_released;
	int just_repeated;
	uint32_t repeat_at[BTN_ID_COUNT];
	PAD_Axis laxis;
	PAD_Axis raxis;
} PAD_Context;
extern PAD_Context pad;

#define PAD_REPEAT_DELAY	300
#define PAD_REPEAT_INTERVAL 100

#define PAD_init PLAT_initInput
#define PAD_quit PLAT_quitInput
// the tools' sounds: once a tool calls SFX_forInput(1), every PAD_poll clicks for moving, confirming and going back
// (inline so it reads input the way the caller does)
void SFX_forInput(int enabled); // also warms the sounds up, so the first plays without a wait
void SFX_afterInput(void);
static inline void PAD_pollWithSounds(void); // below, after PLAT_pollInput
#define PAD_poll PAD_pollWithSounds
#define PAD_wake PLAT_shouldWake

void PAD_setAnalog(int neg, int pos, int value, int repeat_at); // internal

void PAD_reset(void);
int PAD_anyJustPressed(void);
int PAD_anyPressed(void);
int PAD_anyJustReleased(void);

int PAD_justPressed(int btn);
int PAD_isPressed(int btn);
int PAD_justReleased(int btn);
int PAD_justRepeated(int btn);

int PAD_tappedMenu(uint32_t now); // special case, returns 1 on release of BTN_MENU within 250ms if BTN_PLUS/BTN_MINUS haven't been pressed

///////////////////////////////

void VIB_init(void);
void VIB_quit(void);
void VIB_setStrength(int strength);
int VIB_coreHasRumble(const char* core); // the cores with a Rumble Strength setting
int VIB_getStrength(void);
void VIB_pulse(int strength, int milliseconds); // asynchronous, for short UI feedback
	
///////////////////////////////

#define BRIGHTNESS_BUTTON_LABEL "+ -" // ew

typedef void (*PWR_callback_t)(void);
void PWR_init(void);
void PWR_quit(void);
void PWR_warn(int enable);

int PWR_ignoreSettingInput(int btn, int show_setting);
void PWR_update(int* dirty, int* show_setting, PWR_callback_t before_sleep, PWR_callback_t after_sleep);

void PWR_disablePowerOff(void);
void PWR_powerOff(void);
int PWR_isPoweringOff(void);

void PWR_fauxSleep(void);

void PWR_disableSleep(void);
void PWR_enableSleep(void);

void PWR_disableAutosleep(void);
void PWR_enableAutosleep(void);
int PWR_preventAutosleep(void);

enum {
	PWR_TIMER_SLEEP, // idle time before autosleep (menus only)
	PWR_TIMER_POWEROFF, // time asleep before powering off
	PWR_TIMER_COUNT,
};
int PWR_getTimerCount(int timer); // number of choices
int PWR_getTimer(int timer); // index of the current choice
void PWR_setTimer(int timer, int index); // also saves to POWER_PATH
const char* PWR_getTimerLabel(int timer, int index);

int PWR_isCharging(void);
int PWR_getBattery(void);

enum {
	CPU_SPEED_MENU,
	CPU_SPEED_POWERSAVE,
	CPU_SPEED_NORMAL,
	CPU_SPEED_PERFORMANCE,
};
#define PWR_setCPUSpeed PLAT_setCPUSpeed
#define PWR_setCPUSpeedAsync PLAT_setCPUSpeedAsync
#define PWR_setCPUFreq PLAT_setCPUFreq

///////////////////////////////

void PLAT_initInput(void);
void PLAT_quitInput(void);
void PLAT_pollInput(void);
static inline void PAD_pollWithSounds(void) {
	PLAT_pollInput();
	SFX_afterInput();
}
int PLAT_shouldWake(void);

SDL_Surface* PLAT_initVideo(void);
void PLAT_quitVideo(void);
void PLAT_clearVideo(SDL_Surface* screen);
void PLAT_clearAll(void);
void PLAT_setVsync(int vsync);
SDL_Surface* PLAT_resizeVideo(int w, int h, int pitch);
void PLAT_setVideoScaleClip(int x, int y, int width, int height);
void PLAT_setNearestNeighbor(int enabled);
void PLAT_setSharpness(int sharpness);
void PLAT_setEffectColor(int color);
void PLAT_setEffect(int effect);
void PLAT_vsync(int remaining);
scaler_t PLAT_getScaler(GFX_Renderer* renderer);
void PLAT_blitRenderer(GFX_Renderer* renderer);
void PLAT_flip(SDL_Surface* screen, int sync);
int PLAT_supportsOverscan(void);
int PLAT_supportsBorder(void);
void PLAT_setBorder(const char* path); // image drawn behind the game, filling the screen
void PLAT_setNotification(SDL_Surface* overlay); // drawn over the game until cleared, copied so the caller keeps ownership
void PLAT_setNotificationAlpha(int alpha);
SDL_Surface* PLAT_captureScreen(void); // the last game frame as shown (border, effect), device sized RGB565, NULL when it can't; caller frees
void PLAT_presentLastFrame(void); // shows the last game frame again, with the notification over it
enum { POWER_AWAKE, POWER_ASLEEP, POWER_ASLEEP_LONG };
void PLAT_powerSave(int state); // sleep: the least the device can run on (POWER_ASLEEP_LONG after a minute asleep)

// GPU textures: uploaded once, then drawn scaled (smoothly), faded and darkened over the screen on the next flip,
// in the order queued (the queue empties each flip). Platforms without a GPU path return NULL from PLAT_createTexture
typedef struct GFX_Texture GFX_Texture;
GFX_Texture* PLAT_createTexture(SDL_Surface* surface); // a copy, the caller keeps the surface
void PLAT_destroyTexture(GFX_Texture* texture);
void PLAT_queueTexture(GFX_Texture* texture, SDL_Rect* dst, int alpha, int shade); // dst in screen pixels, alpha and shade 0-255

SDL_Surface* PLAT_initOverlay(void);
void PLAT_quitOverlay(void);
void PLAT_enableOverlay(int enable);
	
#define PWR_LOW_CHARGE 10
void PLAT_getBatteryStatus(int* is_charging, int* charge); // 0,1 and 0,10,20,40,60,80,100
void PLAT_enableBacklight(int enable);
void PLAT_showSleepLight(int level); // the sleep light as SLEEP_LIGHT_* now (the settings' preview), -1 for off
void PLAT_powerOff(void);
	
void PLAT_setCPUSpeed(int speed); // enum
void PLAT_setCPUSpeedAsync(int speed); // the same, without waiting for the change to be made
void PLAT_setCPUFreq(int mhz); // about mhz (the nearest the device has), without waiting; other devices pick the nearest CPU_SPEED_
void PLAT_setRumble(int strength);
int PLAT_pickSampleRate(int requested, int max);

char* PLAT_getModel(void);
int PLAT_isOnline(void);
int PLAT_setDateTime(int y, int m, int d, int h, int i, int s);

#endif
