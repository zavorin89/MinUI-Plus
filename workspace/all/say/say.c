#include <stdio.h>
#include <string.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"

int main(int argc , char* argv[]) {
	int confirm = argc>2 && strcmp(argv[1], "--confirm")==0;
	if ((!confirm && argc<2) || (confirm && argc<3)) return EXIT_FAILURE;
	char* message = argv[confirm ? 2 : 1];
	char msg[1024];
	snprintf(msg, sizeof(msg), "%s", message);
	
	PWR_setCPUSpeed(CPU_SPEED_MENU);
	
	SDL_Surface* screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	InitSettings();
	
	SDL_Event event;
	int result = -1;
	
	int dirty = 1;
	while(result<0) {
		uint32_t frame_start = SDL_GetTicks();
		
		PAD_poll();
		if (PAD_justPressed(BTN_A)) result = 0;
		else if (PAD_justPressed(BTN_B)) result = confirm ? 1 : 0;
		
		// PWR_update(&dirty, NULL, NULL,NULL);
		
		if (dirty) {
			GFX_clear(screen);
			
			GFX_blitMessage(font.large, msg, screen,&(SDL_Rect){0,0,screen->w,screen->h-SCALE1(PADDING + PILL_SIZE + PADDING)});
			if (confirm) GFX_blitButtonGroup((char*[]){ "B","LATER", "A","RESTART", NULL }, 1, screen, 1);
			else GFX_blitButtonGroup((char*[]){ "A","OKAY", NULL }, 1, screen, 1);
			
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}
	
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	
	return result;
}
