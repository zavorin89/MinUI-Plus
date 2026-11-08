// rgb30
#include <stdio.h>
#include <stdlib.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>

#include <msettings.h>
#include <mstick.h>

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"

#include "scaler.h"

///////////////////////////////

#define RAW_UP		103
#define RAW_DOWN	108
#define RAW_LEFT	105
#define RAW_RIGHT	106
#define RAW_A		 57
#define RAW_B		 29
#define RAW_X		 42
#define RAW_Y		 56
#define RAW_START	 28
#define RAW_SELECT	 97
#define RAW_MENU	  1
#define RAW_L1		 18
#define RAW_L2		 15
#define RAW_R1		 20
#define RAW_R2		 14
#define RAW_PLUS	115
#define RAW_MINUS	114
#define RAW_POWER	116

#define INPUT_COUNT 2
static int inputs[INPUT_COUNT];

void PLAT_initInput(void) {
	inputs[0] = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC); // power
	inputs[1] = open("/dev/input/event3", O_RDONLY | O_NONBLOCK | O_CLOEXEC); // controller
	Stick_init(); // analog
}
void PLAT_quitInput(void) {
	Stick_quit();
	for (int i=0; i<INPUT_COUNT; i++) {
		close(inputs[i]);
	}
}

// from <linux/input.h> which has BTN_ constants that conflict with platform.h
struct input_event {
	struct timeval time;
	__u16 type;
	__u16 code;
	__s32 value;
};
#define EV_KEY			0x01
#define EV_ABS			0x03

void PLAT_pollInput(void) {
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
	int input;
	static struct input_event event;
	for (int i=0; i<INPUT_COUNT; i++) {
		input = inputs[i];
		while (read(input, &event, sizeof(event))==sizeof(event)) {
			if (event.type!=EV_KEY && event.type!=EV_ABS) continue;

			int btn = BTN_NONE;
			int pressed = 0; // 0=up,1=down
			int id = -1;
			int type = event.type;
			int code = event.code;
			int value = event.value;
			
			// TODO: tmp, hardcoded, missing some buttons
			if (type==EV_KEY) {
				if (value>1) continue; // ignore repeats
			
				pressed = value;
				// LOG_info("key event: %i (%i)\n", code,pressed);
					 if (code==RAW_UP) 		{ btn = BTN_DPAD_UP; 	id = BTN_ID_DPAD_UP; }
	 			else if (code==RAW_DOWN)	{ btn = BTN_DPAD_DOWN; 	id = BTN_ID_DPAD_DOWN; }
				else if (code==RAW_LEFT)	{ btn = BTN_DPAD_LEFT; 	id = BTN_ID_DPAD_LEFT; }
				else if (code==RAW_RIGHT)	{ btn = BTN_DPAD_RIGHT; id = BTN_ID_DPAD_RIGHT; }
				else if (code==RAW_A)		{ btn = BTN_A; 			id = BTN_ID_A; }
				else if (code==RAW_B)		{ btn = BTN_B; 			id = BTN_ID_B; }
				else if (code==RAW_X)		{ btn = BTN_X; 			id = BTN_ID_X; }
				else if (code==RAW_Y)		{ btn = BTN_Y; 			id = BTN_ID_Y; }
				else if (code==RAW_START)	{ btn = BTN_START; 		id = BTN_ID_START; }
				else if (code==RAW_SELECT)	{ btn = BTN_SELECT; 	id = BTN_ID_SELECT; }
				else if (code==RAW_MENU)	{ btn = BTN_MENU; 		id = BTN_ID_MENU; }
				else if (code==RAW_L1)		{ btn = BTN_L1; 		id = BTN_ID_L1; }
				else if (code==RAW_L2)		{ btn = BTN_L2; 		id = BTN_ID_L2; }
				else if (code==RAW_R1)		{ btn = BTN_R1; 		id = BTN_ID_R1; }
				else if (code==RAW_R2)		{ btn = BTN_R2; 		id = BTN_ID_R2; }
				else if (code==RAW_PLUS)	{ btn = BTN_PLUS; 		id = BTN_ID_PLUS; }
				else if (code==RAW_MINUS)	{ btn = BTN_MINUS; 		id = BTN_ID_MINUS; }
				else if (code==RAW_POWER)	{ btn = BTN_POWER; 		id = BTN_ID_POWER; }
			}
			
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
	}
	
	Stick_get(&(pad.laxis.x), &(pad.laxis.y));
	PAD_setAnalog(BTN_ID_ANALOG_LEFT, BTN_ID_ANALOG_RIGHT, pad.laxis.x, tick+PAD_REPEAT_DELAY);
	PAD_setAnalog(BTN_ID_ANALOG_UP,   BTN_ID_ANALOG_DOWN,  pad.laxis.y, tick+PAD_REPEAT_DELAY);
}

int PLAT_shouldWake(void) {
	int input;
	static struct input_event event;
	for (int i=0; i<INPUT_COUNT; i++) {
		input = inputs[i];
		while (read(input, &event, sizeof(event))==sizeof(event)) {
			if (event.type==EV_KEY && event.code==RAW_POWER && event.value==0) {
				return 1;
			}
		}
	}
	return 0;
}

///////////////////////////////

// based on rg35xxplus

static struct VID_Context {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* texture;
	SDL_Texture* target;
	SDL_Texture* effect;
	SDL_Texture* border;
	char border_path[MAX_PATH];
	SDL_Surface* border_image; // as loaded, for refitting
	SDL_Rect border_window; // its see-through part, w 0 when it has none
	SDL_Surface* border_native; // <name>-native.png when there is one: made for the game at native scaling
	SDL_Surface* border_base; // which of the two is on screen (not owned), for PLAT_captureScreen
	SDL_Rect border_native_window;
	SDL_Rect border_fit; // the game rect the texture was fitted to, w 0 when it's the image as is
	SDL_Surface* border_shown; // the fitted border as a surface (NULL: border_image as is), for PLAT_captureScreen
	SDL_Surface* effect_image; // the grid/lines, as a surface with the texture's opacity
	// the last game frame as drawn, for PLAT_presentLastFrame and PLAT_captureScreen
	int has_frame;
	SDL_Texture* frame_texture;
	SDL_Rect frame_src; // in frame_texture (scaled up when crisp)
	SDL_Rect frame_dst;
	int frame_effect;
	SDL_Rect frame_effect_src; // the part of the effect image drawn over frame_dst
	void* frame_pixels; // the frame itself, RGB565
	int frame_pitch;
	int frame_w, frame_h;
	SDL_Rect frame_src_pixels; // frame_src in frame_pixels

	SDL_Surface* buffer;
	SDL_Surface* screen;
	
	GFX_Renderer* blit; // yeesh
	
	int width;
	int height;
	int pitch;
	int sharpness;
} vid;

static int device_width;
static int device_height;
static int device_pitch;
static int rotate = 0;
SDL_Surface* PLAT_initVideo(void) {
	// LOG_info("PLAT_initVideo\n");
	
	SDL_InitSubSystem(SDL_INIT_VIDEO);
	SDL_ShowCursor(0);
	
	// SDL_version compiled;
	// SDL_version linked;
	// SDL_VERSION(&compiled);
	// SDL_GetVersion(&linked);
	// LOG_info("Compiled SDL version %d.%d.%d ...\n", compiled.major, compiled.minor, compiled.patch);
	// LOG_info("Linked SDL version %d.%d.%d.\n", linked.major, linked.minor, linked.patch);
	//
	// int num_displays = SDL_GetNumVideoDisplays();
	// LOG_info("SDL_GetNumVideoDisplays(): %i\n", num_displays);
	//
	// LOG_info("Available video drivers:\n");
	// for (int i=0; i<SDL_GetNumVideoDrivers(); i++) {
	// 	LOG_info("- %s\n", SDL_GetVideoDriver(i));
	// }
	// LOG_info("Current video driver: %s\n", SDL_GetCurrentVideoDriver());
	//
	// LOG_info("Available render drivers:\n");
	// for (int i=0; i<SDL_GetNumRenderDrivers(); i++) {
	// 	SDL_RendererInfo info;
	// 	SDL_GetRenderDriverInfo(i,&info);
	// 	LOG_info("- %s\n", info.name);
	// }
	//
	// LOG_info("Available display modes:\n");
	// SDL_DisplayMode mode;
	// for (int i=0; i<SDL_GetNumDisplayModes(0); i++) {
	// 	SDL_GetDisplayMode(0, i, &mode);
	// 	LOG_info("- %ix%i (%s)\n", mode.w,mode.h, SDL_GetPixelFormatName(mode.format));
	// }
	// SDL_GetCurrentDisplayMode(0, &mode);
	// LOG_info("Current display mode: %ix%i (%s)\n", mode.w,mode.h, SDL_GetPixelFormatName(mode.format));

	// LOG_info("Available audio drivers:\n");
	// for (int i=0; i<SDL_GetNumAudioDrivers(); i++) {
	// 	LOG_info("- %s\n", SDL_GetAudioDriver(i));
	// }
	// LOG_info("Current audio driver: %s\n", SDL_GetCurrentAudioDriver()); // NOTE: hadn't been selected yet so will always be NULL!

	// SDL_SetHint(SDL_HINT_RENDER_VSYNC,"0"); // ignored?

	int w = FIXED_WIDTH;
	int h = FIXED_HEIGHT;
	int p = FIXED_PITCH;
	vid.window   = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w,h, SDL_WINDOW_SHOWN);
	LOG_info("window size: %ix%i\n", w,h);
	
	SDL_DisplayMode mode;
	SDL_GetCurrentDisplayMode(0, &mode);
	LOG_info("Current display mode: %ix%i (%s)\n", mode.w,mode.h, SDL_GetPixelFormatName(mode.format));
	if (mode.h>mode.w) rotate = 3;
	vid.renderer = SDL_CreateRenderer(vid.window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
	// SDL_RenderSetLogicalSize(vid.renderer, w,h); // TODO: wrong, but without and with the below it's even wrong-er
	
	// int renderer_width,renderer_height;
	// SDL_GetRendererOutputSize(vid.renderer, &renderer_width, &renderer_height);
	// LOG_info("output size: %ix%i\n", renderer_width, renderer_height);
	// if (renderer_width!=w) { // I think this can only be hdmi
	// 	float x_scale = (float)renderer_width / w;
	// 	float y_scale = (float)renderer_height / h;
	// 	SDL_SetWindowSize(vid.window, w / x_scale, h / y_scale);
	//
	// 	SDL_GetRendererOutputSize(vid.renderer, &renderer_width, &renderer_height);
	// 	LOG_info("adjusted size: %ix%i\n", renderer_width, renderer_height);
	// 	x_scale = (float)renderer_width / w;
	// 	y_scale = (float)renderer_height / h;
	// 	SDL_RenderSetScale(vid.renderer, x_scale,y_scale);
	//
	// 	// for some reason we need to clear and present
	// 	// after setting the window size or we'll miss
	// 	// the first frame
	// 	SDL_RenderClear(vid.renderer);
	// 	SDL_RenderPresent(vid.renderer);
	// }
	
	// SDL_RendererInfo info;
	// SDL_GetRendererInfo(vid.renderer, &info);
	// LOG_info("Current render driver: %s\n", info.name);
	
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"1"); // linear
	vid.texture = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, w,h);
	vid.target	= NULL; // only needed for non-native sizes
	
	// TODO: doesn't work here
	// SDL_SetTextureScaleMode(vid.texture, SDL_ScaleModeLinear); // we always start at device size so use linear for better upscaling over hdmi
	
	// SDL_ScaleMode scale_mode;
	// SDL_GetTextureScaleMode(vid.texture, &scale_mode);
	// LOG_info("texture scale mode: %i\n", scale_mode);
	
	// int format;
	// int access_;
	// SDL_QueryTexture(vid.texture, &format, &access_, NULL,NULL);
	// LOG_info("texture format: %s (streaming: %i)\n", SDL_GetPixelFormatName(format), access_==SDL_TEXTUREACCESS_STREAMING);
	
	vid.buffer	= SDL_CreateRGBSurfaceFrom(NULL, w,h, FIXED_DEPTH, p, RGBA_MASK_565);
	vid.screen	= SDL_CreateRGBSurface(SDL_SWSURFACE, w,h, FIXED_DEPTH, RGBA_MASK_565);
	vid.width	= w;
	vid.height	= h;
	vid.pitch	= p;
	
	device_width	= w;
	device_height	= h;
	device_pitch	= p;
	
	vid.sharpness = SHARPNESS_SOFT;
	
	return vid.screen;
}

static void clearVideo(void) {
	SDL_FillRect(vid.screen, NULL, 0);
	for (int i=0; i<3; i++) {
		SDL_RenderClear(vid.renderer);
		SDL_RenderPresent(vid.renderer);
	}
}

static void freeMenuTextures(void);
static void freeOverlays(void);
void PLAT_quitVideo(void) {
	// clearVideo();
	freeMenuTextures();

	SDL_FreeSurface(vid.screen);
	SDL_FreeSurface(vid.buffer);
	if (vid.target) SDL_DestroyTexture(vid.target);
	if (vid.effect) SDL_DestroyTexture(vid.effect);
	if (vid.border) SDL_DestroyTexture(vid.border);
	if (vid.border_image) SDL_FreeSurface(vid.border_image);
	if (vid.border_shown) SDL_FreeSurface(vid.border_shown);
	if (vid.border_native) SDL_FreeSurface(vid.border_native);
	if (vid.effect_image) SDL_FreeSurface(vid.effect_image);
	freeOverlays();
	SDL_DestroyTexture(vid.texture);
	SDL_DestroyRenderer(vid.renderer);
	SDL_DestroyWindow(vid.window);
	
	// system("cat /dev/zero > /dev/fb0 2>/dev/null");
	SDL_Quit();
}

void PLAT_clearVideo(SDL_Surface* screen) {
	SDL_FillRect(screen, NULL, 0); // TODO: revisit
}
void PLAT_clearAll(void) {
	PLAT_clearVideo(vid.screen); // TODO: revist
	SDL_RenderClear(vid.renderer);
}

void PLAT_setVsync(int vsync) {
	// buh
}

static int hard_scale = 4; // TODO: base src size, eg. 160x144 can be 4

static void resizeVideo(int w, int h, int p) {
	if (w==vid.width && h==vid.height && p==vid.pitch) return;
	
	// TODO: minarch disables crisp (and nn upscale before linear downscale) when native
	
	if (w>=device_width && h>=device_height) hard_scale = 1;
	else if (h>=160) hard_scale = 2; // limits gba and up to 2x (seems sufficient)
	else hard_scale = 4;

	LOG_info("resizeVideo(%i,%i,%i) hard_scale: %i crisp: %i\n",w,h,p, hard_scale,vid.sharpness==SHARPNESS_CRISP);

	SDL_FreeSurface(vid.buffer);
	SDL_DestroyTexture(vid.texture);
	if (vid.target) SDL_DestroyTexture(vid.target);
	vid.has_frame = 0; // its texture is gone
	
	SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, vid.sharpness==SHARPNESS_SOFT?"1":"0", SDL_HINT_OVERRIDE);
	vid.texture = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, w,h);
	
	if (vid.sharpness==SHARPNESS_CRISP) {
		SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, "1", SDL_HINT_OVERRIDE);
		vid.target = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_TARGET, w * hard_scale,h * hard_scale);
	}
	else {
		vid.target = NULL;
	}

	vid.buffer	= SDL_CreateRGBSurfaceFrom(NULL, w,h, FIXED_DEPTH, p, RGBA_MASK_565);

	vid.width	= w;
	vid.height	= h;
	vid.pitch	= p;
}

SDL_Surface* PLAT_resizeVideo(int w, int h, int p) {
	resizeVideo(w,h,p);
	return vid.screen;
}

void PLAT_setVideoScaleClip(int x, int y, int width, int height) {
	// buh
}
void PLAT_setNearestNeighbor(int enabled) {
	// buh
}
void PLAT_setSharpness(int sharpness) {
	if (vid.sharpness==sharpness) return;
	int p = vid.pitch;
	vid.pitch = 0;
	vid.sharpness = sharpness;
	resizeVideo(vid.width,vid.height,p);
}

static struct FX_Context {
	int scale;
	int type;
	int color;
	int next_scale;
	int next_type;
	int next_color;
	int live_type;
} effect = {
	.scale = 1,
	.next_scale = 1,
	.type = EFFECT_NONE,
	.next_type = EFFECT_NONE,
	.live_type = EFFECT_NONE,
	.color = 0,
	.next_color = 0,
};
static void rgb565_to_rgb888(uint32_t rgb565, uint8_t *r, uint8_t *g, uint8_t *b) {
    // Extract the red component (5 bits)
    uint8_t red = (rgb565 >> 11) & 0x1F;
    // Extract the green component (6 bits)
    uint8_t green = (rgb565 >> 5) & 0x3F;
    // Extract the blue component (5 bits)
    uint8_t blue = rgb565 & 0x1F;

    // Scale the values to 8-bit range
    *r = (red << 3) | (red >> 2);
    *g = (green << 2) | (green >> 4);
    *b = (blue << 3) | (blue >> 2);
}
static void updateEffect(void) {
	if (effect.next_scale==effect.scale && effect.next_type==effect.type && effect.next_color==effect.color) return; // unchanged
	
	int live_scale = effect.scale;
	int live_color = effect.color;
	effect.scale = effect.next_scale;
	effect.type = effect.next_type;
	effect.color = effect.next_color;
	
	if (effect.type==EFFECT_NONE) return; // disabled
	if (effect.type==effect.live_type && effect.scale==live_scale && effect.color==live_color) return; // already loaded
	
	char* effect_path;
	int opacity = 128; // 1 - 1/2 = 50%
	if (effect.type==EFFECT_LINE) {
		opacity = 88; // ~35%: the lines without darkening the picture as much (50% read as too dark)
		if (effect.scale<3) {
			effect_path = RES_PATH "/line-2.png";
		}
		else if (effect.scale<4) {
			effect_path = RES_PATH "/line-3.png";
		}
		else if (effect.scale<5) {
			effect_path = RES_PATH "/line-4.png";
		}
		else if (effect.scale<6) {
			effect_path = RES_PATH "/line-5.png";
		}
		else if (effect.scale<8) {
			effect_path = RES_PATH "/line-6.png";
		}
		else {
			effect_path = RES_PATH "/line-8.png";
		}
	}
	else if (effect.type==EFFECT_GRID) {
		if (effect.scale<3) {
			effect_path = RES_PATH "/grid-2.png";
			opacity = 112; // as strong as grid-3's (25%, 1 - 3/4, left 2x games looking like they had no grid)
		}
		else if (effect.scale<4) {
			effect_path = RES_PATH "/grid-3.png";
			opacity = 112; // 1 - 5/9 = ~44%
		}
		else if (effect.scale<5) {
			effect_path = RES_PATH "/grid-4.png";
			opacity = 144; // 1 - 7/16 = ~56%
		}
		else if (effect.scale<6) {
			effect_path = RES_PATH "/grid-5.png";
			opacity = 160; // 1 - 9/25 = ~64%
		}
		else if (effect.scale<8) {
			effect_path = RES_PATH "/grid-6.png";
			opacity = 112; // 1 - 5/9 = ~44%
		}
		else if (effect.scale<11) {
			effect_path = RES_PATH "/grid-8.png";
			opacity = 144; // 1 - 7/16 = ~56%
		}
		else {
			effect_path = RES_PATH "/grid-11.png";
			opacity = 136; // 1 - 57/121 = ~52%
		}
	}
	
	// LOG_info("effect: %s opacity: %i\n", effect_path, opacity);
	SDL_Surface* tmp = IMG_Load(effect_path);
	if (tmp) {
		if (effect.type==EFFECT_GRID) {
			if (effect.color) {
				// LOG_info("dmg color grid...\n");
			
				uint8_t r,g,b;
				rgb565_to_rgb888(effect.color,&r,&g,&b);
				// LOG_info("rgb %i,%i,%i\n",r,g,b);
				
				uint32_t* pixels = (uint32_t*)tmp->pixels;
				int width = tmp->w;
				int height = tmp->h;
				for (int y = 0; y < height; ++y) {
				    for (int x = 0; x < width; ++x) {
				        uint32_t pixel = pixels[y * width + x];
				        uint8_t _,a;
				        SDL_GetRGBA(pixel, tmp->format, &_, &_, &_, &a);
				        if (a) pixels[y * width + x] = SDL_MapRGBA(tmp->format, r,g,b, a);
				    }
				}
			}
		}
		
		if (vid.effect) SDL_DestroyTexture(vid.effect);
		vid.effect = SDL_CreateTextureFromSurface(vid.renderer, tmp);
		SDL_SetTextureAlphaMod(vid.effect, opacity);
		if (vid.effect_image) SDL_FreeSurface(vid.effect_image);
		vid.effect_image = tmp; // kept for PLAT_captureScreen
		SDL_SetSurfaceBlendMode(tmp, SDL_BLENDMODE_BLEND);
		SDL_SetSurfaceAlphaMod(tmp, opacity);
		effect.live_type = effect.type;
	}
	}
void PLAT_setEffect(int next_type) {
	effect.next_type = next_type;
}
void PLAT_setEffectColor(int next_color) {
	effect.next_color = next_color;
}
void PLAT_vsync(int remaining) {
	if (remaining>0) SDL_Delay(remaining);
}

scaler_t PLAT_getScaler(GFX_Renderer* renderer) {
	// LOG_info("getScaler for scale: %i\n", renderer->scale);
	effect.next_scale = renderer->scale;
	return scale1x1_c16;
}

int PLAT_supportsBorder(void) { return 1; }
static SDL_Surface* loadBorderImage(const char* path) {
	SDL_Surface* loaded = IMG_Load(path);
	if (!loaded) return NULL;
	SDL_Surface* image = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(loaded);
	return image;
}
// the see-through window a border leaves for the game, w 0 when it has none
static SDL_Rect borderWindow(SDL_Surface* image) {
	int x0 = image->w, y0 = image->h, x1 = -1, y1 = -1;
	for (int y=0; y<image->h; y++) {
		uint32_t* row = (uint32_t*)((uint8_t*)image->pixels + y * image->pitch);
		for (int x=0; x<image->w; x++) {
			if ((row[x] >> 24)>=128) continue;
			if (x<x0) x0 = x;
			if (x>x1) x1 = x;
			if (y<y0) y0 = y;
			if (y>y1) y1 = y;
		}
	}
	return x1<0 ? (SDL_Rect){0,0,0,0} : (SDL_Rect){x0, y0, x1-x0+1, y1-y0+1};
}
void PLAT_setBorder(const char* path) {
	if (path && exactMatch((char*)path, vid.border_path)) return; // already loaded

	if (vid.border) SDL_DestroyTexture(vid.border);
	if (vid.border_image) SDL_FreeSurface(vid.border_image);
	if (vid.border_shown) SDL_FreeSurface(vid.border_shown);
	if (vid.border_native) SDL_FreeSurface(vid.border_native);
	vid.border = NULL;
	vid.border_image = NULL;
	vid.border_shown = NULL;
	vid.border_native = NULL;
	vid.border_base = NULL;
	vid.border_path[0] = '\0';
	if (!path) return;

	SDL_Surface* image = loadBorderImage(path);
	if (!image) {
		LOG_warn("couldn't load bezel: %s\n", path);
		return;
	}
	char native_path[MAX_PATH];
	snprintf(native_path, sizeof(native_path), "%s", path);
	char* ext = strrchr(native_path, '.');
	if (ext && ext - native_path + strlen("-native.png") < sizeof(native_path)) {
		strcpy(ext, "-native.png");
		if (exists(native_path)) {
			vid.border_native = loadBorderImage(native_path);
			if (vid.border_native) vid.border_native_window = borderWindow(vid.border_native);
		}
	}
	vid.border = SDL_CreateTextureFromSurface(vid.renderer, image);
	if (!vid.border) {
		SDL_FreeSurface(image);
		return;
	}
	strcpy(vid.border_path, path);
	vid.border_image = image;
	vid.border_fit = (SDL_Rect){0,0,0,0};
	vid.border_window = borderWindow(image); // (made for aspect scaling)
}
// natively scaled games are smaller than the window a border leaves for them: its panels and bars move in to meet
// the game, and what's left between is filled with the frame's own color (rebuilt only when the game's size changes)
static void fitBorder(SDL_Rect* game) {
	SDL_Surface* image = vid.border_image;
	SDL_Rect* win = &vid.border_window;
	SDL_Rect want = game ? *game : (SDL_Rect){0,0,0,0};
	if (want.w && vid.border_native && vid.border_native_window.w) { // one made for native: fitted (if at all) from that
		image = vid.border_native;
		win = &vid.border_native_window;
	}
	if (!image || !win->w) return;
	if (want.x==vid.border_fit.x && want.y==vid.border_fit.y && want.w==vid.border_fit.w && want.h==vid.border_fit.h) return;
	vid.border_fit = want;
	int as_made = want.w && win->x==want.x && win->y==want.y && win->w==want.w && win->h==want.h;
	if (as_made) want = (SDL_Rect){0,0,0,0}; // the native one exactly fits: shown as is
	vid.border_base = image;

	SDL_Surface* out = image;
	if (want.w) {
		int w = image->w, h = image->h;
		out = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
		if (!out) return;
		// the frame's color: its most common one (not an average, which its logos and trim would tint)
		static uint32_t counts[4096], sums[4096][3];
		memset(counts, 0, sizeof(counts));
		memset(sums, 0, sizeof(sums));
		for (int y=0; y<h; y+=2) {
			uint32_t* row = (uint32_t*)((uint8_t*)image->pixels + y * image->pitch);
			for (int x=0; x<w; x+=2) {
				uint32_t c = row[x];
				if ((c >> 24)<128) continue;
				int bucket = ((c >> 12) & 0xF00) | ((c >> 8) & 0xF0) | ((c >> 4) & 0xF);
				counts[bucket] += 1;
				sums[bucket][0] += (c >> 16) & 0xFF; sums[bucket][1] += (c >> 8) & 0xFF; sums[bucket][2] += c & 0xFF;
			}
		}
		int most = 0;
		for (int i=1; i<4096; i++) if (counts[i]>counts[most]) most = i;
		uint32_t n = counts[most];
		uint32_t fill = n ? 0xFF000000 | ((sums[most][0]/n) << 16) | ((sums[most][1]/n) << 8) | (sums[most][2]/n) : 0xFF000000;
		// each side that has a panel slides in to the game, sides without one keep their place
		int left = win->x>0, right = win->x+win->w<w, top = win->y>0, bottom = win->y+win->h<h;
		int* map_x = malloc(w * sizeof(int));
		int* map_y = malloc(h * sizeof(int));
		for (int x=0; x<w; x++) {
			if (left && x<want.x) map_x[x] = MAX(0, x - (want.x - win->x));
			else if (right && x>=want.x+want.w) map_x[x] = MIN(w-1, win->x + win->w + (x - want.x - want.w));
			else map_x[x] = x;
		}
		for (int y=0; y<h; y++) {
			if (top && y<want.y) map_y[y] = MAX(0, y - (want.y - win->y));
			else if (bottom && y>=want.y+want.h) map_y[y] = MIN(h-1, win->y + win->h + (y - want.y - want.h));
			else map_y[y] = y;
		}
		for (int y=0; y<h; y++) {
			uint32_t* src_row = (uint32_t*)((uint8_t*)image->pixels + map_y[y] * image->pitch);
			uint32_t* dst = (uint32_t*)((uint8_t*)out->pixels + y * out->pitch);
			int in_y = y>=want.y && y<want.y+want.h;
			for (int x=0; x<w; x++) {
				if (in_y && x>=want.x && x<want.x+want.w) dst[x] = 0; // the game
				else {
					uint32_t c = src_row[map_x[x]];
					dst[x] = (c >> 24)<128 ? fill : c;
				}
			}
		}
		free(map_x);
		free(map_y);
	}
	SDL_Texture* texture = SDL_CreateTextureFromSurface(vid.renderer, out);
	if (vid.border_shown) SDL_FreeSurface(vid.border_shown);
	vid.border_shown = out!=image ? out : NULL; // kept for PLAT_captureScreen
	if (!texture) return;
	SDL_DestroyTexture(vid.border);
	vid.border = texture;
}

// the last game frame as it was on screen (border, scaling, grid or lines), not rotated, for the pause menu and save
// state pictures. Put together in software from the same pieces the GPU draws (reading the screen back from the GPU
// turned out unreliable here, and showed whatever popup was up)
SDL_Surface* PLAT_captureScreen(void) {
	if (!vid.has_frame || !vid.frame_pixels || !vid.frame_src_pixels.w || !vid.frame_src_pixels.h) return NULL;
	SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(0, device_width, device_height, 16, SDL_PIXELFORMAT_RGB565);
	if (!out) return NULL;
	SDL_FillRect(out, NULL, 0);
	SDL_Surface* border = vid.border_shown ? vid.border_shown : (vid.border_base ? vid.border_base : vid.border_image);
	if (vid.border && border) SDL_BlitSurface(border, NULL, out, NULL);
	SDL_Surface* frame = SDL_CreateRGBSurfaceWithFormatFrom(vid.frame_pixels, vid.frame_w, vid.frame_h, 16, vid.frame_pitch, SDL_PIXELFORMAT_RGB565);
	if (frame) {
		SDL_Rect dst = vid.frame_dst;
		SDL_BlitScaled(frame, &vid.frame_src_pixels, out, &dst);
		SDL_FreeSurface(frame);
	}
	if (vid.frame_effect && vid.effect_image) {
		SDL_Rect dst = vid.frame_dst;
		SDL_BlitScaled(vid.effect_image, &vid.frame_effect_src, out, &dst);
	}
	return out;
}

// the notification (eg. an achievement) and then the volume or brightness bar, over everything else. A game can set
// them from its own thread (minarch's Prevent Tearing runs the core on one), and the renderer only works on the one
// that draws: so setting one only keeps a copy of the picture, made into a texture here, when it's next drawn
typedef struct Overlay {
	SDL_Texture* texture;
	SDL_Surface* pending; // the picture to show next (NULL with changed set: hide it)
	int changed;
	volatile int alpha;
} Overlay;
static Overlay overlays[2] = { {.alpha = 255}, {.alpha = 255} }; // 0 the notification, 1 the bar
static pthread_mutex_t overlay_lock = PTHREAD_MUTEX_INITIALIZER;
static void setOverlay(Overlay* o, SDL_Surface* picture) {
	SDL_Surface* copy = picture ? SDL_ConvertSurfaceFormat(picture, SDL_PIXELFORMAT_ARGB8888, 0) : NULL;
	pthread_mutex_lock(&overlay_lock);
	if (o->pending) SDL_FreeSurface(o->pending);
	o->pending = copy;
	o->changed = 1;
	pthread_mutex_unlock(&overlay_lock);
}
static void freeOverlays(void) {
	for (int i=0; i<2; i++) {
		if (overlays[i].texture) SDL_DestroyTexture(overlays[i].texture);
		if (overlays[i].pending) SDL_FreeSurface(overlays[i].pending);
		overlays[i].texture = NULL;
		overlays[i].pending = NULL;
		overlays[i].changed = 0;
	}
}
static int overlaysShown(void) {
	return overlays[0].texture || overlays[1].texture || overlays[0].changed || overlays[1].changed;
}
static void drawOverlays(int ox, int oy) {
	for (int i=0; i<2; i++) {
		Overlay* o = &overlays[i];
		if (o->changed) {
			pthread_mutex_lock(&overlay_lock);
			if (o->texture) SDL_DestroyTexture(o->texture);
			o->texture = o->pending ? SDL_CreateTextureFromSurface(vid.renderer, o->pending) : NULL;
			if (o->texture) SDL_SetTextureBlendMode(o->texture, SDL_BLENDMODE_BLEND);
			if (o->pending) SDL_FreeSurface(o->pending);
			o->pending = NULL;
			o->changed = 0;
			pthread_mutex_unlock(&overlay_lock);
		}
		if (!o->texture) continue;
		SDL_SetTextureAlphaMod(o->texture, o->alpha);
		if (rotate) SDL_RenderCopyEx(vid.renderer,o->texture,NULL,&(SDL_Rect){ox,oy,device_width,device_height},rotate*90,NULL,SDL_FLIP_NONE);
		else SDL_RenderCopy(vid.renderer, o->texture, NULL, NULL);
	}
}
// the last game frame again, with the notification over it: shows something (eg. that it's saving) while the game is held
void PLAT_presentLastFrame(void) {
	if (!vid.has_frame || !vid.frame_texture) return;
	int oy = (device_width-device_height)/2;
	int ox = -oy;
	SDL_SetRenderDrawColor(vid.renderer, 0, 0, 0, 255);
	SDL_RenderClear(vid.renderer);
	SDL_Rect* d = &vid.frame_dst;
	if (vid.border) {
		if (rotate) SDL_RenderCopyEx(vid.renderer,vid.border,NULL,&(SDL_Rect){ox,oy,device_width,device_height},rotate*90,NULL,SDL_FLIP_NONE);
		else SDL_RenderCopy(vid.renderer, vid.border, NULL, NULL);
	}
	if (rotate) SDL_RenderCopyEx(vid.renderer,vid.frame_texture,&vid.frame_src,&(SDL_Rect){ox+d->x,oy+d->y,d->w,d->h},rotate*90,NULL,SDL_FLIP_NONE);
	else SDL_RenderCopy(vid.renderer, vid.frame_texture, &vid.frame_src, d);
	if (vid.frame_effect) {
		if (rotate) SDL_RenderCopyEx(vid.renderer,vid.effect,&vid.frame_effect_src,&(SDL_Rect){ox+d->x,oy+d->y,d->w,d->h},rotate*90,NULL,SDL_FLIP_NONE);
		else SDL_RenderCopy(vid.renderer, vid.effect, &vid.frame_effect_src, d);
	}
	drawOverlays(ox, oy);
	SDL_RenderPresent(vid.renderer);
}

void PLAT_setNotification(SDL_Surface* overlay) { setOverlay(&overlays[0], overlay); }
void PLAT_setNotificationAlpha(int alpha) { overlays[0].alpha = alpha; }
void PLAT_setSettingOverlay(SDL_Surface* overlay) { setOverlay(&overlays[1], overlay); }
void PLAT_setSettingAlpha(int alpha) { overlays[1].alpha = alpha; }

///////////////////////////////

// GPU textures drawn over the screen on the next flip, see api.h
struct GFX_Texture {
	SDL_Texture* texture;
};
#define TEXTURE_QUEUE_SIZE 48
static struct { GFX_Texture* texture; SDL_Rect dst; int alpha; int shade; } texture_queue[TEXTURE_QUEUE_SIZE];
static int texture_queue_count = 0;

GFX_Texture* PLAT_createTexture(SDL_Surface* surface) {
	if (!surface) return NULL;
	SDL_Texture* texture = SDL_CreateTextureFromSurface(vid.renderer, surface);
	if (!texture) return NULL;
	SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear); // smooth when drawn at a different size
	GFX_Texture* self = malloc(sizeof(GFX_Texture));
	self->texture = texture;
	return self;
}
void PLAT_destroyTexture(GFX_Texture* self) {
	if (!self) return;
	for (int i=0; i<texture_queue_count; i++) if (texture_queue[i].texture==self) texture_queue[i].texture = NULL; // not drawn after it's gone
	SDL_DestroyTexture(self->texture);
	free(self);
}
void PLAT_queueTexture(GFX_Texture* self, SDL_Rect* dst, int alpha, int shade) {
	if (!self || texture_queue_count==TEXTURE_QUEUE_SIZE || dst->w<=0 || dst->h<=0) return;
	texture_queue[texture_queue_count++] = (typeof(texture_queue[0])){ self, *dst, alpha, shade };
}
// the queue, rotated like the screen (around the same point so everything lines up), then emptied
static void drawTextureQueue(void) {
	for (int i=0; i<texture_queue_count; i++) {
		GFX_Texture* self = texture_queue[i].texture;
		if (!self) continue;
		SDL_Rect* dst = &texture_queue[i].dst;
		int shade = texture_queue[i].shade;
		SDL_SetTextureAlphaMod(self->texture, texture_queue[i].alpha);
		SDL_SetTextureColorMod(self->texture, shade, shade, shade);
		if (rotate) SDL_RenderCopyEx(vid.renderer, self->texture, NULL, &(SDL_Rect){ dst->x, device_width + dst->y, dst->w, dst->h }, rotate*90, &(SDL_Point){ -dst->x, -dst->y }, SDL_FLIP_NONE);
		else SDL_RenderCopy(vid.renderer, self->texture, NULL, dst);
	}
	texture_queue_count = 0;
}

void PLAT_blitRenderer(GFX_Renderer* renderer) {
	vid.blit = renderer;
	SDL_RenderClear(vid.renderer);
	resizeVideo(vid.blit->true_w,vid.blit->true_h,vid.blit->src_p);
}

// the menus' screen goes up into one of a few textures in turn: rewriting the one the GPU is still drawing the
// last frame from makes the Mali driver wait for that frame to reach the display, which held menus to 20fps
#define MENU_TEXTURES 3
static SDL_Texture* menu_textures[MENU_TEXTURES];
static int menu_texture_next = 0;
static void freeMenuTextures(void) {
	for (int i=0; i<MENU_TEXTURES; i++) {
		if (menu_textures[i]) SDL_DestroyTexture(menu_textures[i]);
		menu_textures[i] = NULL;
	}
}
void PLAT_flip(SDL_Surface* IGNORED, int ignored) {
	if (!vid.blit) {
		resizeVideo(device_width,device_height,FIXED_PITCH); // !!!???
		SDL_Texture* texture = menu_textures[menu_texture_next];
		if (!texture) texture = menu_textures[menu_texture_next] = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, device_width,device_height);
		if (!texture) texture = vid.texture;
		else menu_texture_next = (menu_texture_next + 1) % MENU_TEXTURES;
		SDL_UpdateTexture(texture,NULL,vid.screen->pixels,vid.screen->pitch);
		if (rotate) SDL_RenderCopyEx(vid.renderer,texture,NULL,&(SDL_Rect){0,device_width,device_width,device_height},rotate*90,&(SDL_Point){0,0},SDL_FLIP_NONE);
		else SDL_RenderCopy(vid.renderer, texture, NULL,NULL);
		drawTextureQueue(); // eg. the carousel's art, over the menu
		SDL_RenderPresent(vid.renderer);
		return;
	}
	
	texture_queue_count = 0; // only menus queue textures
	// uint32_t then = SDL_GetTicks();
	SDL_UpdateTexture(vid.texture,NULL,vid.blit->src,vid.blit->src_p);
	// LOG_info("blit blocked for %ims (%i,%i)\n", SDL_GetTicks()-then,vid.buffer->w,vid.buffer->h);
	
	SDL_Texture* target = vid.texture;
	int x = vid.blit->src_x;
	int y = vid.blit->src_y;
	int w = vid.blit->src_w;
	int h = vid.blit->src_h;
	vid.frame_pixels = vid.blit->src;
	vid.frame_pitch = vid.blit->src_p;
	vid.frame_w = vid.width;
	vid.frame_h = vid.height;
	vid.frame_src_pixels = (SDL_Rect){x,y,w,h};
	if (vid.sharpness==SHARPNESS_CRISP) {
		SDL_SetRenderTarget(vid.renderer,vid.target);
		SDL_RenderCopy(vid.renderer, vid.texture, NULL,NULL);
		SDL_SetRenderTarget(vid.renderer,NULL);
		x *= hard_scale;
		y *= hard_scale;
		w *= hard_scale;
		h *= hard_scale;
		target = vid.target;
	}
	
	SDL_Rect* src_rect = &(SDL_Rect){x,y,w,h};
	SDL_Rect* dst_rect = &(SDL_Rect){0,0,device_width,device_height};
	if (vid.blit->aspect==0) { // native or cropped
		// LOG_info("src_rect %i,%i %ix%i\n",src_rect->x,src_rect->y,src_rect->w,src_rect->h);

		int w = vid.blit->src_w * vid.blit->scale;
		int h = vid.blit->src_h * vid.blit->scale;
		int x = (device_width - w) / 2;
		int y = (device_height - h) / 2;
		dst_rect->x = x;
		dst_rect->y = y;
		dst_rect->w = w;
		dst_rect->h = h;
		
		// LOG_info("dst_rect %i,%i %ix%i\n",dst_rect->x,dst_rect->y,dst_rect->w,dst_rect->h);
	}
	else if (vid.blit->aspect>0) { // aspect
		int h = device_height;
		int w = h * vid.blit->aspect;
		if (w>device_width) {
			double ratio = 1 / vid.blit->aspect;
			w = device_width;
			h = w * ratio;
		}
		int x = (device_width - w) / 2;
		int y = (device_height - h) / 2;
		// dst_rect = &(SDL_Rect){x,y,w,h};
		dst_rect->x = x;
		dst_rect->y = y;
		dst_rect->w = w;
		dst_rect->h = h;
	}
	
	int ox,oy;
	oy = (device_width-device_height)/2;
	ox = -oy;

	// border first so the game always draws over it
	if (vid.border) {
		// a vertical game (taller than wide) is shown full height with the border's side panels moved in to meet it,
		// whatever its exact shape: arcade borders leave a 3:4 window, and 7:9 or 2:3 games fill theirs too
		int tall = dst_rect->h > dst_rect->w;
		fitBorder(vid.blit->aspect==0 || tall ? dst_rect : NULL);
		if (rotate) SDL_RenderCopyEx(vid.renderer,vid.border,NULL,&(SDL_Rect){ox,oy,device_width,device_height},rotate*90,NULL,SDL_FLIP_NONE);
		else SDL_RenderCopy(vid.renderer, vid.border, NULL, NULL);
	}

	if (rotate) SDL_RenderCopyEx(vid.renderer,target,src_rect,&(SDL_Rect){ox+dst_rect->x,oy+dst_rect->y,dst_rect->w,dst_rect->h},rotate*90,NULL,SDL_FLIP_NONE);
	else SDL_RenderCopy(vid.renderer, target, src_rect, dst_rect);
	
	SDL_Rect effect_src = { 0, 0, dst_rect->w, dst_rect->h };
	updateEffect();
	vid.frame_effect_src = effect_src;
	vid.has_frame = 1;
	vid.frame_texture = target;
	vid.frame_src = *src_rect;
	vid.frame_dst = *dst_rect;
	vid.frame_effect = vid.blit && effect.type!=EFFECT_NONE && vid.effect;
	if (vid.blit && effect.type!=EFFECT_NONE && vid.effect) {
		// ox = effect.scale - (dst_rect->x % effect.scale);
		// oy = effect.scale - (dst_rect->y % effect.scale);
		// if (ox==effect.scale) ox = 0;
		// if (oy==effect.scale) oy = 0;
 		// LOG_info("rotate: %i ox: %i oy: %i\n", rotate, ox,oy);
		if (rotate) SDL_RenderCopyEx(vid.renderer,vid.effect,&effect_src,&(SDL_Rect){ox+dst_rect->x,oy+dst_rect->y,dst_rect->w,dst_rect->h},rotate*90,NULL,SDL_FLIP_NONE);
		else SDL_RenderCopy(vid.renderer, vid.effect, &effect_src,dst_rect);
	}
	
	// notifications (eg. achievements) go over everything, positioned like the border
	drawOverlays(ox, oy);
	
	// uint32_t then = SDL_GetTicks();
	SDL_RenderPresent(vid.renderer);
	// LOG_info("SDL_RenderPresent blocked for %ims\n", SDL_GetTicks()-then);
	vid.blit = NULL;
}

// a GPU core draws straight into this one, so there's nothing to copy back or up. ABGR8888 is the format SDL's renderer
// draws without swapping red and blue (it has no RGB565 of its own: asking for that gets a swapped one, and a blue game)
static int gpu_filter = 0;
SDL_Texture* PLAT_gpuTexture(int width, int height) {
	SDL_Texture* texture = SDL_CreateTexture(vid.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, width, height);
	if (texture) SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
	gpu_filter = 0;
	return texture;
}
// as PLAT_flip, for a whole 2x picture with no border or notification over it.
// Anything else goes the long way, through PLAT_flip.
int PLAT_flipGpuFrame(SDL_Texture* texture, int width, int height, double aspect, int bottom_up) {
	if (vid.border || overlaysShown()) return 0;
	// a 2x frame fills this screen exactly, so Native (-1) and Fullscreen (0) are the same picture
	int w = aspect>0 ? (int)(device_height * aspect + 0.5) : width*2;
	if (w!=device_width || w!=width*2 || device_height!=height*2) return 0;
	int filter = vid.sharpness==SHARPNESS_SOFT;
	if (filter!=gpu_filter) { // (it stays on the texture, so only when it changes)
		SDL_SetTextureScaleMode(texture, filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
		gpu_filter = filter;
	}
	SDL_Rect src = {0,0,width,height};
	SDL_Rect dst = {0,0,device_width,device_height};
	int oy = (device_width-device_height)/2;
	int ox = -oy;
	if (rotate) {
		dst.x = ox;
		dst.y = oy;
	}
	SDL_RenderClear(vid.renderer);
	SDL_RenderCopyEx(vid.renderer, texture, &src, &dst, rotate*90, NULL, bottom_up ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
	updateEffect();
	if (effect.type!=EFFECT_NONE && vid.effect) {
		SDL_Rect effect_src = {0,0,device_width,device_height};
		SDL_RenderCopyEx(vid.renderer, vid.effect, &effect_src, &dst, rotate*90, NULL, SDL_FLIP_NONE);
	}
	SDL_RenderPresent(vid.renderer);
	return 1;
}

// for all/ndslayer, which draws DraStic's screens itself
int PLAT_getRotate(void) { return rotate; }
void PLAT_drawNotification(void) { // for a layer that draws the frame itself (the volume or brightness bar too)
	if (!overlaysShown()) return;
	int oy = (device_width-device_height)/2;
	drawOverlays(-oy, oy);
}
SDL_Texture* PLAT_getEffectTexture(int scale) { // the screen effect, NULL when it's off
	effect.next_scale = scale;
	updateEffect();
	return effect.type==EFFECT_NONE ? NULL : vid.effect;
}

///////////////////////////////

// TODO: 
#define OVERLAY_WIDTH PILL_SIZE // unscaled
#define OVERLAY_HEIGHT PILL_SIZE // unscaled
#define OVERLAY_BPP 4
#define OVERLAY_DEPTH 16
#define OVERLAY_PITCH (OVERLAY_WIDTH * OVERLAY_BPP) // unscaled
#define OVERLAY_RGBA_MASK 0x00ff0000,0x0000ff00,0x000000ff,0xff000000 // ARGB
static struct OVL_Context {
	SDL_Surface* overlay;
} ovl;

SDL_Surface* PLAT_initOverlay(void) {
	ovl.overlay = SDL_CreateRGBSurface(SDL_SWSURFACE, SCALE2(OVERLAY_WIDTH,OVERLAY_HEIGHT),OVERLAY_DEPTH,OVERLAY_RGBA_MASK);
	return ovl.overlay;
}
void PLAT_quitOverlay(void) {
	if (ovl.overlay) SDL_FreeSurface(ovl.overlay);
}
void PLAT_enableOverlay(int enable) {

}

///////////////////////////////

static int online = 0;
void PLAT_getBatteryStatus(int* is_charging, int* charge) {
	// *is_charging = 0;
	// *charge = PWR_LOW_CHARGE;
	// return;
	
	// a wall charger can show up as ac rather than usb, and the battery says for itself, so any of them counts
	char charging[16] = {0};
	getFile("/sys/class/power_supply/battery/status", charging, 16);
	int usb = getInt("/sys/class/power_supply/usb/online");
	int ac = getInt("/sys/class/power_supply/ac/online");
	*is_charging = usb || ac || prefixMatch("Charging", charging) || prefixMatch("Full", charging);
	static int was_usb = -1, was_ac = -1;
	static char was_charging[16];
	if (usb!=was_usb || ac!=was_ac || strcmp(charging, was_charging)) { // what each says, whenever one changes (only the A30 itself can show that)
		LOG_info("charging: %i (usb %i, ac %i, battery says %s)\n", *is_charging, usb, ac, charging);
		was_usb = usb;
		was_ac = ac;
		memcpy(was_charging, charging, sizeof(was_charging));
	}

	int i = getInt("/sys/class/power_supply/battery/capacity");
	// Preserve the driver's percentage instead of rounding up in 20% steps.
	*charge = i < 0 ? 0 : (i > 100 ? 100 : i);

	// wifi status, just hooking into the regular PWR polling
	char status[16];
	getFile("/sys/class/net/wlan0/operstate", status,16);
	online = prefixMatch("up", status);
}

#define LED_PATH "/sys/class/leds/led1/brightness"
// the sleep light on or off: it reports a max_brightness of 255, but any value lights it fully (tried on the A30)
static int sleepLightValue(int level) {
	return level>SLEEP_LIGHT_OFF ? 255 : 0;
}
void PLAT_showSleepLight(int level) {
	putInt(LED_PATH, level<0 ? 0 : sleepLightValue(level));
}
void PLAT_enableBacklight(int enable) {
	if (enable) {
		SetBrightness(GetBrightness());
		putInt(LED_PATH, 0);
	}
	else {
		SetRawBrightness(0);
		putInt(LED_PATH, sleepLightValue(PWR_getSleepLight()));
	}
}

void PLAT_powerOff(void) {
	system("rm -f /tmp/minui_exec && sync");
	sleep(2);

	SetRawVolume(MUTE_VOLUME_RAW);
	PLAT_enableBacklight(0);
	putInt(LED_PATH,255);
	SND_quit();
	VIB_quit();
	PWR_quit();
	GFX_quit();
	exit(0);
}

///////////////////////////////

static int cpu_freq_now = 1344; // what was last asked for, to go back to after sleep
static int cpu_cores_now = 2;
// changing speed in-process: the CPU PLL register written directly (as overclock.elf does) and the second core switched
// through sysfs, instead of starting overclock.elf, whose shells and rewrites of the GPU and RAM clocks made every
// change take a noticeable moment. overclock.elf still sets the governor, GPU and RAM clocks at boot (launch.sh)
#include <sys/mman.h>
#include "../overclock/cpu_clocks.h"
static volatile uint32_t* ccu = NULL;
static int ccu_tried = 0;
static void setCoreOnline(int core, int online) {
	char path[64];
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%i/online", core);
	FILE* file = fopen(path, "r");
	if (!file) return;
	int now = fgetc(file)=='1';
	fclose(file);
	if (now==online) return;
	file = fopen(path, "w");
	if (!file) return;
	fputs(online ? "1" : "0", file);
	fclose(file);
}
static int fastCPU(int freq, int cpu) {
	if (!ccu_tried) {
		ccu_tried = 1;
		int fd = open("/dev/mem", O_RDWR | O_SYNC);
		if (fd>=0) {
			void* mem = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, CCU_BASE);
			if (mem!=MAP_FAILED) ccu = mem;
			close(fd);
		}
	}
	if (!ccu) return 0;
	int best = 0;
	for (int i=1; i<(int)(sizeof(cpu_clock)/sizeof(cpu_clock[0])); i++) {
		if (abs(cpu_clock[i].clk - freq)<abs(cpu_clock[best].clk - freq)) best = i;
	}
	setCoreOnline(1, cpu>=2);
	ccu[0] = cpu_clock[best].reg;
	return 1;
}
static void setCPUCores(int freq, int cpu, int wait) {
	cpu_freq_now = freq;
	cpu_cores_now = cpu;
	if (fastCPU(freq, cpu)) return;
	char cmd[128];
	sprintf(cmd,"overclock.elf userspace %d %d 384 1080 0%s", cpu, freq, wait ? "" : " &");
	system(cmd);
}
static void setCPUFreq(int freq, int wait) {
	setCPUCores(freq, 2, wait); // both cores at every speed: switching one off and on as you scroll was slow, and risky
}
static int speedFreq(int speed) {
	switch (speed) {
		case CPU_SPEED_MENU: 		return  576;
		case CPU_SPEED_POWERSAVE:	return 1056;
		case CPU_SPEED_PERFORMANCE: return 1512;
		default:					return 1344; // CPU_SPEED_NORMAL
	}
}
static void setCPUSpeed(int speed, int wait) {
	setCPUFreq(speedFreq(speed), wait);
}
static pthread_mutex_t cpu_lock = PTHREAD_MUTEX_INITIALIZER;
static int cpu_generation = 0;
void PLAT_setCPUSpeed(int speed) {
	pthread_mutex_lock(&cpu_lock);
	cpu_generation += 1; // any change still on its way is now stale
	setCPUSpeed(speed, 1);
	pthread_mutex_unlock(&cpu_lock);
}
// even backgrounded with "&", system() stops the caller while it forks and starts a shell (a dropped frame or
// two in the menu), so the change is made from a thread of its own instead. Every change bumps a generation
// under one lock, so a thread that gets there after a newer change (eg. leaving a menu quickly) does nothing
typedef struct { int freq; int cores; int generation; } CPUChange;
static void* setCPUSpeedThread(void* arg) {
	CPUChange* change = arg;
	pthread_mutex_lock(&cpu_lock);
	if (change->generation==cpu_generation) setCPUCores(change->freq, change->cores, 1);
	pthread_mutex_unlock(&cpu_lock);
	free(change);
	return NULL;
}
static void setCPUFreqAsync(int freq, int cores) {
	CPUChange* change = malloc(sizeof(CPUChange));
	pthread_mutex_lock(&cpu_lock);
	change->freq = freq;
	change->cores = cores;
	change->generation = ++cpu_generation;
	pthread_mutex_unlock(&cpu_lock);
	pthread_t thread;
	if (pthread_create(&thread, NULL, setCPUSpeedThread, change)==0) pthread_detach(thread);
	else {
		free(change);
		setCPUCores(freq, cores, 0);
	}
}
void PLAT_setCPUSpeedAsync(int speed) {
	setCPUFreqAsync(speedFreq(speed), 2);
}
// minarch's Auto speed, without holding up the frame. Both cores stay on at every speed: switching one on and off over
// and over while a game's threads run can hang the device, and an idle second core costs next to nothing
void PLAT_setCPUFreq(int mhz) {
	setCPUFreqAsync(mhz, 2);
}

// sleep: the screen and sound are off and all that runs is a check of the buttons a few times a second, so one core
// at a crawl does. After a minute asleep wifi goes too (it's the biggest draw left), and comes back when woken
#define ASLEEP_PATH "/tmp/minui_asleep" // launch.sh's background loop checks in less often while it's there
static int wifi_slept = 0;
void PLAT_powerSave(int state) {
	if (state==POWER_ASLEEP) {
		pthread_mutex_lock(&cpu_lock);
		cpu_generation += 1; // a change still on its way would undo this
		if (!fastCPU(240, 1)) system("overclock.elf userspace 1 240 384 1080 0"); // (cpu_freq_now kept, to go back to)
		pthread_mutex_unlock(&cpu_lock);
		touch(ASLEEP_PATH);
	}
	else if (state==POWER_ASLEEP_LONG) {
		if (PLAT_isOnline()) {
			system("wifi.sh down");
			wifi_slept = 1;
		}
	}
	else {
		pthread_mutex_lock(&cpu_lock);
		cpu_generation += 1;
		setCPUCores(cpu_freq_now, cpu_cores_now, 1);
		pthread_mutex_unlock(&cpu_lock);
		unlink(ASLEEP_PATH);
		if (wifi_slept) {
			wifi_slept = 0;
			system("wifi.sh up >/dev/null 2>&1 &"); // reconnects in the background
		}
	}
}

#define RUMBLE_PATH "/sys/devices/virtual/timed_output/vibrator/enable"
// The A30's timed-output driver exposes duration, not voltage or amplitude. Honor libretro's strength by pulsing the
// fixed-strength motor at a gentler duty cycle instead of treating every non-zero request as full power.
static pthread_once_t rumble_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t rumble_mutex = PTHREAD_MUTEX_INITIALIZER;
static int rumble_strength = 0;
static void* rumbleThread(void* arg) {
	(void)arg;
	while (1) {
		pthread_mutex_lock(&rumble_mutex);
		int strength = rumble_strength;
		pthread_mutex_unlock(&rumble_mutex);
		if (!strength) {
			SDL_Delay(12);
			continue;
		}
		// The driver only takes whole milliseconds, too coarse for ten steps on their own, so the cycle's length varies
		// too: about 15% on at the weakest, and under 40% of the old continuous drive at the strongest.
		if (strength > 65535) strength = 65535;
		int on_ms = 4 + strength * 5 / 65535;
		int duty = 130 + strength / 240; // per thousand: 130 to 403
		putInt(RUMBLE_PATH, on_ms);
		SDL_Delay((on_ms * 1000 + duty / 2) / duty);
	}
	return NULL;
}
static void startRumbleThread(void) {
	pthread_t thread;
	if (pthread_create(&thread, NULL, rumbleThread, NULL)==0) pthread_detach(thread);
}
void PLAT_setRumble(int strength) {
	pthread_once(&rumble_once, startRumbleThread);
	pthread_mutex_lock(&rumble_mutex);
	rumble_strength = strength > 0 ? strength : 0;
	pthread_mutex_unlock(&rumble_mutex);
	if (!strength) putInt(RUMBLE_PATH, 0);
}

int PLAT_pickSampleRate(int requested, int max) {
	// the firmware mixes everything at 48000Hz (asound.conf's dmix), so anything else gets converted again
	// on its way out: output that and let minarch's resampler do the one conversion
	return max;
}

char* PLAT_getModel(void) {
	return "Miyoo A30";
}

SDL_Window* PLAT_getWindow(void) { // for minarch's GPU-drawn cores (hwrender.c), which draw on a context of their own here
	return vid.window;
}
int PLAT_isOnline(void) {
	return online;
}
