// Controls: a read-only reference of every button and combo, page by page

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <msettings.h>
#include "defines.h"
#include "api.h"
#include "utils.h"

static SDL_Surface* screen;

typedef struct Page {
	char* title;
	char* rows[8][2]; // buttons, what they do; ends at the first empty row
} Page;

static Page pages[] = {
	{ "Game lists", {
		{ "Up / Down",		"Move" },
		{ "Left / Right",	"Jump a page (one game in carousel)" },
		{ "L1 / R1",		"Previous / next letter" },
		{ "L2 / R2",		"Change the sort order" },
		{ "A",				"Play" },
		{ "B",				"Back" },
	}},
	{ "Game lists (more)", {
		{ "X",				"Resume from the quicksave" },
		{ "Y",				"Add to / remove from Favorites" },
		{ "Select",			"Switch list / carousel view" },
		{ "Hold Select",	"Pin to the game switcher" },
		{ "Start",			"Pick a random game" },
		{ "Menu + X",		"Delete the game" },
		{ "Menu + Y",		"Choose the game's core" },
	}},
	{ "Anywhere in MinUI", {
		{ "Menu",			"Settings and play time" },
		{ "Hold Menu",		"Game switcher" },
		{ "Volume + / -",	"Volume" },
		{ "Menu + Volume",	"Brightness" },
		{ "Power",			"Sleep" },
		{ "Hold Power",		"Power off" },
	}},
	{ "Game switcher", {
		{ "Left / Right",	"Choose a game" },
		{ "A",				"Pick up where you left off" },
		{ "X",				"Remove from the switcher" },
		{ "B",				"Back" },
	}},
	{ "Settings screen", {
		{ "Up / Down",		"Choose a setting" },
		{ "Left / Right",	"Change it" },
		{ "A",				"Most played: see the list" },
		{ "X twice",		"Most played: clear play time" },
		{ "B / Menu",		"Back" },
	}},
	{ "In game", {
		{ "Menu",			"Pause menu" },
		{ "Hold Menu",		"Quicksave and open the switcher" },
		{ "Menu + Volume",	"Brightness" },
		{ "Power",			"Sleep" },
		{ "Hold Power",		"Quicksave and power off" },
		{ "Your shortcuts",	"Set in Options > Shortcuts" },
	}},
	{ "Pause menu", {
		{ "Up / Down",		"Choose" },
		{ "Left / Right",	"Change the save slot" },
		{ "A",				"Select" },
		{ "X",				"Pin to the game switcher" },
		{ "B / Menu",		"Back to the game" },
	}},
	{ "Achievements list", {
		{ "Up / Down",		"Move" },
		{ "Left / Right",	"Jump a page" },
		{ "X",				"Change the sort order" },
		{ "B / Menu",		"Back to the pause menu" },
	}},
};
#define PAGE_COUNT (int)(sizeof(pages) / sizeof(pages[0]))

int main(int argc , char* argv[]) {
	PWR_setCPUSpeed(CPU_SPEED_MENU);
	screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	InitSettings();

	int page = 0;
	int dirty = 1;
	int show_setting = 0;
	while (1) {
		GFX_startFrame();
		PAD_poll();

		if (PAD_justPressed(BTN_B)) break;
		else if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_L1) || PAD_justRepeated(BTN_UP)) {
			page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
			SFX_play(SFX_TICK);
			dirty = 1;
		}
		else if (PAD_justRepeated(BTN_RIGHT) || PAD_justRepeated(BTN_R1) || PAD_justRepeated(BTN_DOWN)) {
			page = (page + 1) % PAGE_COUNT;
			SFX_play(SFX_TICK);
			dirty = 1;
		}

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (dirty) {
			GFX_clear(screen);
			GFX_blitHardwareGroup(screen, show_setting);

			// the page's name, and where it sits among the pages
			Page* p = &pages[page];
			SDL_Surface* title = TTF_RenderUTF8_Blended(font.large, p->title, COLOR_WHITE);
			int title_y = SCALE1(PADDING) + (SCALE1(PILL_SIZE) - title->h) / 2;
			SDL_BlitSurface(title, NULL, screen, &(SDL_Rect){ SCALE1(PADDING+BUTTON_PADDING), title_y });
			char count[16];
			sprintf(count, "%i/%i", page+1, PAGE_COUNT);
			SDL_Surface* counter = TTF_RenderUTF8_Blended(font.small, count, COLOR_DARK_TEXT);
			SDL_BlitSurface(counter, NULL, screen, &(SDL_Rect){ SCALE1(PADDING+BUTTON_PADDING) + title->w + SCALE1(8), title_y + title->h - counter->h - SCALE1(2) });
			SDL_FreeSurface(title);
			SDL_FreeSurface(counter);

			// buttons on the left, lined up, what they do beside them
			int rows = 0;
			int key_w = 0;
			for (; rows<8 && p->rows[rows][0]; rows++) {
				int w;
				TTF_SizeUTF8(font.small, p->rows[rows][0], &w, NULL);
				if (w>key_w) key_w = w;
			}
			int row_h = SCALE1(20);
			int top = SCALE1(PADDING + PILL_SIZE);
			int bottom = screen->h - SCALE1(PADDING + PILL_SIZE);
			int y = top + (bottom - top - rows*row_h) / 2;
			int x = SCALE1(PADDING+BUTTON_PADDING);
			for (int i=0; i<rows; i++) {
				SDL_Surface* key = TTF_RenderUTF8_Blended(font.small, p->rows[i][0], COLOR_WHITE);
				SDL_Surface* what = TTF_RenderUTF8_Blended(font.small, p->rows[i][1], COLOR_DARK_TEXT);
				SDL_BlitSurface(key, NULL, screen, &(SDL_Rect){ x, y + (row_h - key->h) / 2 });
				SDL_BlitSurface(what, NULL, screen, &(SDL_Rect){ x + key_w + SCALE1(BUTTON_PADDING), y + (row_h - what->h) / 2 });
				SDL_FreeSurface(key);
				SDL_FreeSurface(what);
				y += row_h;
			}

			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			else GFX_blitButtonGroup((char*[]){ "L1","PREV", "R1","NEXT", NULL }, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){ "B","BACK", NULL }, 0, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}

	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return EXIT_SUCCESS;
}
