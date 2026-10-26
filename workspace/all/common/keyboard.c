// On-screen keyboard shared by tools that need typed input (Wi-Fi passwords, RetroAchievements sign in).

#include <stdio.h>
#include <string.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "keyboard.h"

// on-screen keyboard: four rows of characters plus a row of wide keys
enum {
	LAYOUT_LOWER,
	LAYOUT_UPPER,
	LAYOUT_SYMBOLS,
	LAYOUT_COUNT,
};
static char* layouts[LAYOUT_COUNT][4] = {
	[LAYOUT_LOWER]   = { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-" },
	[LAYOUT_UPPER]   = { "1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:_" },
	[LAYOUT_SYMBOLS] = { "!@#$%^&*()", "`~-_=+[]{}", "\\|;:'\",.<>", "/?" },
};
enum {
	KEY_SHIFT,
	KEY_SYMBOLS,
	KEY_SPACE,
	KEY_DELETE,
	KEY_DONE,
	KEY_COUNT,
};
static char* special_keys[KEY_COUNT] = { "Shift", "123", "Space", "Delete", "Done" };
#define SPECIAL_ROW 4
#define KEYBOARD_ROWS 5

static int rowLength(int layout, int row) {
	return row==SPECIAL_ROW ? KEY_COUNT : strlen(layouts[layout][row]);
}

static void blitText(SDL_Surface* screen, TTF_Font* font, char* text, SDL_Color color, int x, int y) {
	SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text, color);
	SDL_BlitSurface(surface, NULL, screen, &(SDL_Rect){x, y});
	SDL_FreeSurface(surface);
}

int KB_enterText(SDL_Surface* screen, const char* title, char* text, int max_len) {
	int layout = LAYOUT_LOWER;
	int row = 1;
	int col = 0;
	int len = strlen(text);
	int dirty = 1;
	int show_setting = 0;

	while (1) {
		GFX_startFrame();
		PAD_poll();

		if (PAD_justRepeated(BTN_UP)) { row = (row + KEYBOARD_ROWS - 1) % KEYBOARD_ROWS; dirty = 1; }
		else if (PAD_justRepeated(BTN_DOWN)) { row = (row + 1) % KEYBOARD_ROWS; dirty = 1; }
		else if (PAD_justRepeated(BTN_LEFT)) { col -= 1; dirty = 1; }
		else if (PAD_justRepeated(BTN_RIGHT)) { col += 1; dirty = 1; }
		int count = rowLength(layout, row);
		if (col<0) col = count-1;
		if (col>=count) col = 0;

		int key = -1; // special key pressed
		if (PAD_justPressed(BTN_A)) {
			if (row==SPECIAL_ROW) key = col;
			else if (len<max_len-1) {
				text[len++] = layouts[layout][row][col];
				text[len] = '\0';
				if (layout==LAYOUT_UPPER) layout = LAYOUT_LOWER; // shift applies to one letter
			}
			dirty = 1;
		}
		else if (PAD_justRepeated(BTN_B)) {
			if (len==0) return 0;
			key = KEY_DELETE;
		}
		else if (PAD_justPressed(BTN_X)) key = KEY_SHIFT;
		else if (PAD_justPressed(BTN_Y)) key = KEY_SYMBOLS;
		else if (PAD_justPressed(BTN_START)) key = KEY_DONE;

		if (key!=-1) {
			switch (key) {
				case KEY_SHIFT: layout = layout==LAYOUT_UPPER ? LAYOUT_LOWER : LAYOUT_UPPER; break;
				case KEY_SYMBOLS: layout = layout==LAYOUT_SYMBOLS ? LAYOUT_LOWER : LAYOUT_SYMBOLS; break;
				case KEY_SPACE: if (len<max_len-1) { text[len++] = ' '; text[len] = '\0'; } break;
				case KEY_DELETE: if (len>0) text[--len] = '\0'; break;
				case KEY_DONE: if (len>0) return 1; break;
			}
			int count = rowLength(layout, row); // the symbols layout has a short last row
			if (col>=count) col = count-1;
			dirty = 1;
		}

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (dirty) {
			GFX_clear(screen);
			GFX_blitHardwareGroup(screen, show_setting);

			// title and the typed text (shown, even passwords: it's your own handheld)
			char shown[128];
			GFX_truncateText(font.large, (char*)title, shown, screen->w - SCALE1(PADDING*2 + PILL_SIZE*3), 0);
			blitText(screen, font.large, shown, COLOR_WHITE, SCALE1(PADDING), SCALE1(PADDING+4));

			int field_y = SCALE1(PADDING + PILL_SIZE + 4);
			GFX_blitPill(ASSET_DARK_GRAY_PILL, screen, &(SDL_Rect){ SCALE1(PADDING), field_y, screen->w - SCALE1(PADDING*2), SCALE1(PILL_SIZE) });
			char typed[256];
			snprintf(typed, sizeof(typed), "%s_", text);
			// keep the end of long passwords visible
			char* visible = typed;
			int w;
			TTF_SizeUTF8(font.large, visible, &w, NULL);
			while (w>screen->w - SCALE1(PADDING*2 + BUTTON_PADDING*2) && visible[1]) {
				visible += 1;
				TTF_SizeUTF8(font.large, visible, &w, NULL);
			}
			blitText(screen, font.large, visible, COLOR_WHITE, SCALE1(PADDING + BUTTON_PADDING), field_y + SCALE1(4));

			// keys
			#define KEY_HEIGHT 22 // five rows have to fit between the password field and the button hints
			#define KEY_GAP 2
			int keys_y = field_y + SCALE1(PILL_SIZE + 4);
			int area_w = screen->w - SCALE1(PADDING*2);
			for (int r=0; r<KEYBOARD_ROWS; r++) {
				int n = rowLength(layout, r);
				int key_w = area_w / (r==SPECIAL_ROW ? KEY_COUNT : 10);
				int row_x = SCALE1(PADDING) + (area_w - key_w*n) / 2; // center short rows
				int y = keys_y + r * SCALE1(KEY_HEIGHT + KEY_GAP);
				for (int c=0; c<n; c++) {
					int selected = r==row && c==col;
					SDL_Rect rect = { row_x + c*key_w + SCALE1(KEY_GAP)/2, y, key_w - SCALE1(KEY_GAP), SCALE1(KEY_HEIGHT) };
					GFX_blitRect(selected ? ASSET_BUTTON : ASSET_OPTION, screen, &rect);

					char label[8];
					int active = (r==SPECIAL_ROW && c==KEY_SHIFT && layout==LAYOUT_UPPER) || (r==SPECIAL_ROW && c==KEY_SYMBOLS && layout==LAYOUT_SYMBOLS);
					if (r==SPECIAL_ROW) snprintf(label, sizeof(label), "%s", c==KEY_SYMBOLS && layout==LAYOUT_SYMBOLS ? "abc" : special_keys[c]);
					else snprintf(label, sizeof(label), "%c", layouts[layout][r][c]);

					TTF_Font* f = r==SPECIAL_ROW ? font.medium : font.large;
					SDL_Color color = selected ? COLOR_BLACK : COLOR_WHITE;
					SDL_Surface* text = TTF_RenderUTF8_Blended(f, label, color);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ rect.x + (rect.w-text->w)/2, rect.y + (rect.h-text->h)/2 });
					SDL_FreeSurface(text);
					if (active && !selected) { // underline the layout that's on
						SDL_FillRect(screen, &(SDL_Rect){ rect.x + SCALE1(6), rect.y + rect.h - SCALE1(4), rect.w - SCALE1(12), SCALE1(2) }, RGB_WHITE);
					}
				}
			}

			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			else GFX_blitButtonGroup((char*[]){ "X","SHIFT", "Y",layout==LAYOUT_SYMBOLS?"ABC":"123", NULL }, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){ "B","DEL", "START","JOIN", NULL }, 1, screen, 1);

			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}
}
