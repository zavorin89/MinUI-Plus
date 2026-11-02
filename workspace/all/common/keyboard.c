// On-screen keyboard shared by tools that need typed input (Wi-Fi passwords, RetroAchievements sign in, renaming games).

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
	KEY_BACK,
	KEY_FORWARD,
	KEY_DELETE,
	KEY_DONE,
	KEY_COUNT,
};
static char* special_keys[KEY_COUNT] = { "Shift", "123", "Space", "<", ">", "Del", "Done" };
#define SPECIAL_ROW 4
#define KEYBOARD_ROWS 5
#define FILE_NAME_BANNED "/\\:*?\"<>|"

static int rowLength(int layout, int row) {
	return row==SPECIAL_ROW ? KEY_COUNT : strlen(layouts[layout][row]);
}
static int canType(char c, int flags) {
	return !(flags & KB_FILE_NAME) || !strchr(FILE_NAME_BANNED, c);
}

static void blitText(SDL_Surface* screen, TTF_Font* font, char* text, SDL_Color color, int x, int y) {
	SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text, color);
	if (!surface) return;
	SDL_BlitSurface(surface, NULL, screen, &(SDL_Rect){x, y});
	SDL_FreeSurface(surface);
}

// the cursor is a byte offset, kept on the start of a character (names can have é etc.)
static int prevChar(char* text, int pos) {
	if (pos>0) pos -= 1;
	while (pos>0 && (text[pos] & 0xC0)==0x80) pos -= 1;
	return pos;
}
static int nextChar(char* text, int pos) {
	if (text[pos]) pos += 1;
	while (text[pos] && (text[pos] & 0xC0)==0x80) pos += 1;
	return pos;
}
static void insertChar(char* text, int* pos, int max_len, char c) {
	int len = strlen(text);
	if (len>=max_len-1) return;
	memmove(text + *pos + 1, text + *pos, len - *pos + 1);
	text[*pos] = c;
	*pos += 1;
}
static void deleteChar(char* text, int* pos) {
	if (*pos==0) return;
	int from = prevChar(text, *pos);
	memmove(text + from, text + *pos, strlen(text) - *pos + 1);
	*pos = from;
}

int KB_enterText(SDL_Surface* screen, const char* title, char* text, int max_len, const char* done, int flags) {
	int layout = LAYOUT_LOWER;
	int row = 1;
	int col = 0;
	int pos = strlen(text);
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
			else {
				char c = layouts[layout][row][col];
				if (canType(c, flags)) {
					insertChar(text, &pos, max_len, c);
					if (layout==LAYOUT_UPPER) layout = LAYOUT_LOWER; // shift applies to one letter
				}
			}
			dirty = 1;
		}
		else if (PAD_justRepeated(BTN_B)) {
			if (!text[0]) return 0;
			key = KEY_DELETE;
		}
		else if (PAD_justRepeated(BTN_L1)) key = KEY_BACK;
		else if (PAD_justRepeated(BTN_R1)) key = KEY_FORWARD;
		else if (PAD_justPressed(BTN_X)) key = KEY_SHIFT;
		else if (PAD_justPressed(BTN_Y)) key = KEY_SYMBOLS;
		else if (PAD_justPressed(BTN_SELECT)) return 0;
		else if (PAD_justPressed(BTN_START)) key = KEY_DONE;

		if (key!=-1) {
			switch (key) {
				case KEY_SHIFT: layout = layout==LAYOUT_UPPER ? LAYOUT_LOWER : LAYOUT_UPPER; break;
				case KEY_SYMBOLS: layout = layout==LAYOUT_SYMBOLS ? LAYOUT_LOWER : LAYOUT_SYMBOLS; break;
				case KEY_SPACE: insertChar(text, &pos, max_len, ' '); break;
				case KEY_BACK: pos = prevChar(text, pos); break;
				case KEY_FORWARD: pos = nextChar(text, pos); break;
				case KEY_DELETE: deleteChar(text, &pos); break;
				case KEY_DONE: if (text[0]) return 1; break;
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
			blitText(screen, font.large, shown, COLOR_DARK_TEXT, SCALE1(PADDING + BUTTON_PADDING), SCALE1(PADDING+4)); // grey, as every header

			int field_y = SCALE1(PADDING + PILL_SIZE + 4);
			GFX_blitPill(ASSET_DARK_GRAY_PILL, screen, &(SDL_Rect){ SCALE1(PADDING), field_y, screen->w - SCALE1(PADDING*2), SCALE1(PILL_SIZE) });
			// scrolled so the cursor stays in view
			int inner_x = SCALE1(PADDING + BUTTON_PADDING);
			int inner_w = screen->w - SCALE1(PADDING*2 + BUTTON_PADDING*2);
			int cursor_x = 0;
			if (pos>0) {
				char before[256];
				snprintf(before, sizeof(before), "%.*s", pos, text);
				TTF_SizeUTF8(font.large, before, &cursor_x, NULL);
			}
			SDL_Surface* typed = text[0] ? TTF_RenderUTF8_Blended(font.large, text, COLOR_WHITE) : NULL;
			int scroll = 0;
			if (typed && typed->w>inner_w) {
				scroll = cursor_x - inner_w + SCALE1(4);
				if (scroll>typed->w - inner_w) scroll = typed->w - inner_w;
				if (scroll<0) scroll = 0;
			}
			int text_y = field_y + (SCALE1(PILL_SIZE) - TTF_FontHeight(font.large)) / 2;
			if (typed) {
				SDL_BlitSurface(typed, &(SDL_Rect){ scroll, 0, inner_w, typed->h }, screen, &(SDL_Rect){ inner_x, text_y });
				SDL_FreeSurface(typed);
			}
			SDL_FillRect(screen, &(SDL_Rect){ inner_x + cursor_x - scroll, field_y + SCALE1(6), SCALE1(1), SCALE1(PILL_SIZE - 12) }, RGB_WHITE);

			// keys
			#define KEY_HEIGHT 22 // five rows have to fit between the text field and the button hints
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
					int usable = 1;
					if (r==SPECIAL_ROW) snprintf(label, sizeof(label), "%s", c==KEY_SYMBOLS && layout==LAYOUT_SYMBOLS ? "abc" : special_keys[c]);
					else {
						snprintf(label, sizeof(label), "%c", layouts[layout][r][c]);
						usable = canType(label[0], flags);
					}

					TTF_Font* f = r==SPECIAL_ROW ? font.medium : font.large;
					SDL_Color color = selected ? COLOR_BLACK : (usable ? COLOR_WHITE : COLOR_DARK_TEXT);
					SDL_Surface* text = TTF_RenderUTF8_Blended(f, label, color);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ rect.x + (rect.w-text->w)/2, rect.y + (rect.h-text->h)/2 });
					SDL_FreeSurface(text);
					if (active && !selected) { // underline the layout that's on
						SDL_FillRect(screen, &(SDL_Rect){ rect.x + SCALE1(6), rect.y + rect.h - SCALE1(4), rect.w - SCALE1(12), SCALE1(2) }, RGB_WHITE);
					}
				}
			}

			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			else GFX_blitButtonGroup((char*[]){ "SELECT","CANCEL", NULL }, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){ "B","DEL", "START",(char*)done, NULL }, 1, screen, 1);

			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}
}
