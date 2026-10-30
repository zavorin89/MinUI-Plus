// my282
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#define FIXED_WIDTH		640
#define FIXED_HEIGHT	480
#define FIXED_BPP		2
#define FIXED_DEPTH		(FIXED_BPP * 8)
#define FIXED_PITCH		(FIXED_WIDTH * FIXED_BPP)

#define RGBA_MASK_565	0xF800, 0x07E0, 0x001F, 0x0000

static volatile sig_atomic_t quit = 0;

static void handle_signal(int signal) {
	(void)signal;
	quit = 1;
}

int main(int argc , char* argv[]) {
	if (argc<2) {
		puts("Usage: show.elf image.png [delay] [progress-file]");
		return 0;
	}
	
	char path[256];
	strncpy(path,argv[1],256);
	if (access(path, F_OK)!=0) return 0; // nothing to show :(
	
	int delay = argc>2 ? atoi(argv[2]) : 2;
	char* progress_path = argc>3 ? argv[3] : NULL;
	
	// SDL normally turns SIGINT/SIGTERM into SDL_QUIT events. This small viewer used to ignore those events, so the
	// updater could wait on its progress screen until the full delay expired. Own the signals and poll window events.
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	SDL_Init(SDL_INIT_VIDEO);
	SDL_ShowCursor(0);
	
	int w = FIXED_WIDTH;
	int h = FIXED_HEIGHT;
	int p = FIXED_PITCH;
	SDL_Window* window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w,h, SDL_WINDOW_SHOWN);
	
	int rotate = 0;
	SDL_DisplayMode mode;
	SDL_GetCurrentDisplayMode(0, &mode);
	if (mode.h>mode.w) rotate = 3;
	
	SDL_Renderer* renderer = SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
	SDL_Texture* texture = SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, w,h);
	SDL_Surface* screen = SDL_CreateRGBSurfaceFrom(NULL, w,h, FIXED_DEPTH, p, RGBA_MASK_565);
	
	SDL_Surface* img = IMG_Load(path);
	uint32_t until = SDL_GetTicks() + delay * 1000;
	int last_progress = -1;
	do {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type==SDL_QUIT) quit = 1;
		}
		if (quit) break;

		int progress = -1;
		if (progress_path) {
			FILE* file = fopen(progress_path, "r");
			if (file) {
				if (fscanf(file, "%d", &progress)!=1) progress = 0;
				fclose(file);
			}
			if (progress<0) progress = 0;
			if (progress>100) progress = 100;
		}
		if (!progress_path || progress!=last_progress) {
			SDL_LockTexture(texture,NULL,&screen->pixels,&screen->pitch);
			SDL_FillRect(screen, NULL, 0);
			SDL_BlitSurface(img, NULL, screen, &(SDL_Rect){(screen->w-img->w)/2,(screen->h-img->h)/2});
			if (progress_path) {
				SDL_Rect outside = { 158, 286, 324, 14 };
				SDL_Rect inside = { 160, 288, 320, 10 };
				SDL_Rect filled = { 160, 288, 320 * progress / 100, 10 };
				SDL_FillRect(screen, &outside, SDL_MapRGB(screen->format, 96, 96, 96));
				SDL_FillRect(screen, &inside, SDL_MapRGB(screen->format, 24, 24, 24));
				if (filled.w) SDL_FillRect(screen, &filled, SDL_MapRGB(screen->format, 255, 255, 255));
			}
			char* shot = getenv("MINUI_SHOW_SHOT"); // desktop test harness; unset on the A30
			if (shot) SDL_SaveBMP(screen, shot);
			SDL_UnlockTexture(texture);

			if (rotate) SDL_RenderCopyEx(renderer,texture,NULL,&(SDL_Rect){0,w,w,h},rotate*90,&(SDL_Point){0,0},SDL_FLIP_NONE);
			else SDL_RenderCopy(renderer, texture, NULL,NULL);
			SDL_RenderPresent(renderer);
			last_progress = progress;
		}
		if (!progress_path) break;
		SDL_Delay(100);
	} while (!quit && (int32_t)(until-SDL_GetTicks())>0);
	SDL_FreeSurface(img);
	if (!progress_path) sleep(delay);
	
	SDL_FreeSurface(screen);
	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
