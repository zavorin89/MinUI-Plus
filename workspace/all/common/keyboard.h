#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "sdl.h"

// Edits text (starting from its current contents) on an on-screen keyboard. A types, B deletes (cancels when empty),
// X shift, Y symbols, START done. Returns 1 when done, 0 when cancelled. max_len includes the terminator.
int KB_enterText(SDL_Surface* screen, const char* title, char* text, int max_len);

#endif
