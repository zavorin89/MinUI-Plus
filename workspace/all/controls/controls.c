// Controls: the buttons and combos MinUI+ doesn't show anywhere on screen (what the button hints already say is left out)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <msettings.h>
#include "defines.h"
#include "api.h"
#include "bgm.h"
#include "utils.h"

static SDL_Surface* screen;

// keys are written as the buttons to show, separated by spaces: "MENU + Y", "L1 R1", "hold SELECT".
// Lowercase words, "+" and "/" are drawn as plain text
#define ROW_MAX 8
typedef struct Section {
	char* title;
	char* where; // when these buttons apply
	char* rows[ROW_MAX][2]; // keys, what they do; ends at the first empty row
} Section;

static Section sections[] = {
	{ "Game Lists", NULL, {
		{ "L1 R1",			"Jump by letter" },
		{ "L2 R2",			"Change the sort order" },
		{ "SELECT",			"List or carousel view" },
		{ "hold SELECT",	"Pin to the game switcher" },
		{ "START",			"Random game, any system" },
		{ "MENU + Y",		"Game menu: core, art, rename" },
		{ "MENU + X",		"Delete game, or clear Play Time" },
	}},
	{ "Other", NULL, {
		{ "MENU",			"Settings, or pause a game" },
		{ "MENU",			"Hide an achievement popup" },
		{ "hold MENU",		"Game switcher" },
		{ "MENU + VOL",		"Brightness" },
		{ "hold POWER",		"Power off" },
	}},
	{ "Keyboard", NULL, {
		{ "X",				"Shift, for one capital letter" },
		{ "Y",				"Numbers and symbols" },
		{ "L1 R1",			"Move the cursor" },
	}},
};
#define SECTION_COUNT (int)(sizeof(sections) / sizeof(sections[0]))

#define KEY_GAP SCALE1(4)
#define SHOULDER_W SCALE1(24)
static int isText(char* word) { // drawn as words, not a button
	return word[0]=='+' || word[0]=='/' || (word[0]>='a' && word[0]<='z');
}
// draws the keys at x (when draw is set), returns their width
static int blitKeys(char* keys, int x, int y, int h, int draw) {
	int start = x;
	char copy[128];
	snprintf(copy, sizeof(copy), "%s", keys);
	int first = 1;
	for (char* word=strtok(copy, " "); word; word=strtok(NULL, " ")) {
		if (!first) x += KEY_GAP;
		first = 0;
		int button_y = y + (h - SCALE1(BUTTON_SIZE)) / 2;
		if (isText(word)) {
			SDL_Surface* text = TTF_RenderUTF8_Blended(font.small, word, COLOR_LIGHT_TEXT);
			if (draw) SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ x, y + (h - text->h) / 2 });
			x += text->w;
			SDL_FreeSurface(text);
		}
		else if (strlen(word)==1) { // a round button, like the hints
			if (draw) {
				GFX_blitAsset(ASSET_BUTTON, NULL, screen, &(SDL_Rect){ x, button_y });
				SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, word, COLOR_BUTTON_TEXT);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ x + (SCALE1(BUTTON_SIZE) - text->w) / 2, button_y + (SCALE1(BUTTON_SIZE) - text->h) / 2 });
				SDL_FreeSurface(text);
			}
			x += SCALE1(BUTTON_SIZE);
		}
		else { // a pill with its name: every shoulder button one size, every other one the width of a pair of them
			SDL_Surface* text = TTF_RenderUTF8_Blended(font.tiny, word, COLOR_BUTTON_TEXT);
			int shoulder = (word[0]=='L' || word[0]=='R') && strlen(word)==2;
			int w = shoulder ? SHOULDER_W : SHOULDER_W*2 + KEY_GAP;
			if (draw) {
				GFX_blitPill(ASSET_BUTTON, screen, &(SDL_Rect){ x, button_y, w, SCALE1(BUTTON_SIZE) });
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ x + (w - text->w) / 2, button_y + (SCALE1(BUTTON_SIZE) - text->h) / 2 });
			}
			x += w;
			SDL_FreeSurface(text);
		}
	}
	return x - start;
}

static void blitText(TTF_Font* f, char* text, SDL_Color color, int x, int y, int max_w) {
	char shown[256];
	GFX_truncateText(f, text, shown, max_w, 0);
	SDL_Surface* surface = TTF_RenderUTF8_Blended(f, shown, color);
	if (!surface) return;
	SDL_BlitSurface(surface, NULL, screen, &(SDL_Rect){ x, y });
	SDL_FreeSurface(surface);
}

// the header: a title on the left, the battery etc. on the right
static void blitHeader(char* title, char* where, int show_setting) {
	int ow = GFX_blitHardwareGroup(screen, show_setting);
	int x = SCALE1(PADDING + BUTTON_PADDING);
	int max_w = screen->w - x - ow - SCALE1(PADDING*2);
	SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
	int y = SCALE1(PADDING) + (SCALE1(PILL_SIZE) - text->h) / 2;
	SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ x, y });
	int title_w = text->w;
	int title_h = text->h;
	SDL_FreeSurface(text);
	if (where && max_w - title_w > SCALE1(40)) {
		int h = TTF_FontHeight(font.small);
		blitText(font.small, where, COLOR_DARK_TEXT, x + title_w + SCALE1(8), y + title_h - h - SCALE1(1), max_w - title_w - SCALE1(8));
	}
}

static void blitSection(Section* s, int show_setting) {
	blitHeader(s->title, s->where, show_setting);

	int rows = 0;
	int keys_w = 0;
	for (; rows<ROW_MAX && s->rows[rows][0]; rows++) {
		int w = blitKeys(s->rows[rows][0], 0, 0, 0, 0);
		if (w>keys_w) keys_w = w;
	}
	// buttons right-aligned in a column, what they do beside them, the whole block centered
	int top = SCALE1(PADDING + PILL_SIZE + 4);
	int bottom = screen->h - SCALE1(PADDING + PILL_SIZE + 4);
	int row_h = MIN(SCALE1(28), (bottom - top) / rows);
	int y = top + (bottom - top - rows*row_h) / 2;
	int gap = SCALE1(BUTTON_PADDING);
	int text_w = 0;
	for (int i=0; i<rows; i++) {
		int w;
		TTF_SizeUTF8(font.small, s->rows[i][1], &w, NULL);
		if (w>text_w) text_w = w;
	}
	int area = screen->w - SCALE1(PADDING*2);
	int block_w = MIN(area, keys_w + gap + text_w);
	int keys_right = SCALE1(PADDING) + (area - block_w) / 2 + keys_w;
	int text_x = keys_right + gap;
	for (int i=0; i<rows; i++) {
		int w = blitKeys(s->rows[i][0], 0, 0, 0, 0);
		blitKeys(s->rows[i][0], keys_right - w, y, row_h, 1);
		int h = TTF_FontHeight(font.small);
		blitText(font.small, s->rows[i][1], COLOR_WHITE, text_x, y + (row_h - h) / 2, screen->w - SCALE1(PADDING) - text_x);
		y += row_h;
	}
}

// which page this is, as dots along the bottom
static void blitDots(int page) {
	int size = SCALE1(6);
	int gap = SCALE1(6);
	int total = SECTION_COUNT * size + (SECTION_COUNT - 1) * gap;
	int x = (screen->w - total) / 2;
	int y = screen->h - SCALE1(PADDING + PILL_SIZE/2) - size/2;
	for (int i=0; i<SECTION_COUNT; i++) {
		SDL_FillRect(screen, &(SDL_Rect){ x + i*(size+gap), y, size, size }, i==page ? RGB_WHITE : RGB_GRAY);
	}
}

int main(int argc , char* argv[]) {
	PWR_setCPUSpeed(CPU_SPEED_MENU);
	screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	InitSettings();
	SFX_forInput(1); // clicks for moving, choosing and going back
	BGM_start(); // the menus' music, when it's on

	int page = 0;
	int dirty = 1;
	int show_setting = 0;
	while (1) {
		GFX_startFrame();
		PAD_poll();

		if (PAD_justPressed(BTN_B)) break;
		else if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_L1)) {
			page = (page + SECTION_COUNT - 1) % SECTION_COUNT;
			dirty = 1;
		}
		else if (PAD_justRepeated(BTN_RIGHT) || PAD_justRepeated(BTN_R1)) {
			page = (page + 1) % SECTION_COUNT;
			dirty = 1;
		}

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (dirty) {
			GFX_clear(screen);
			blitSection(&sections[page], show_setting);
			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			blitDots(page);
			GFX_blitButtonGroup((char*[]){ "B","BACK", NULL }, 0, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}

	BGM_quit();
	SFX_quit();
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return EXIT_SUCCESS;
}
