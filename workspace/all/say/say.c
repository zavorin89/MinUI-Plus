#include <stdio.h>
#include <string.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"

int main(int argc , char* argv[]) {
	// say.elf "message": A OKAY. say.elf --confirm "message": B LATER / A SHUT DOWN.
	// say.elf --ask "B label" "A label" "message": a question with its own buttons (6 lines fit).
	// Both questions return 0 for A and 1 for B.
	int ask = argc>4 && strcmp(argv[1], "--ask")==0;
	int confirm = ask || (argc>2 && strcmp(argv[1], "--confirm")==0);
	if ((!confirm && argc<2) || (confirm && argc<3)) return EXIT_FAILURE;
	char* message = argv[ask ? 4 : confirm ? 2 : 1];
	char* b_label = ask ? argv[2] : "LATER";
	char* a_label = ask ? argv[3] : "SHUT DOWN";
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
			if (confirm) GFX_blitButtonGroup((char*[]){ "B",b_label, "A",a_label, NULL }, 1, screen, 1);
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
