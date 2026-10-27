#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "sdl.h"

#define KB_FILE_NAME 1 // leaves out the characters a file name can't have (/ \ : * ? " < > |)

// Edits text (starting from its current contents, cursor at the end) on an on-screen keyboard. A types, B deletes
// (cancels when empty), X shift, Y symbols, L1/R1 move the cursor, SELECT cancels, START finishes (done names it in the
// button hints, eg. "JOIN"). Returns 1 when done, 0 when cancelled. max_len includes the terminator.
int KB_enterText(SDL_Surface* screen, const char* title, char* text, int max_len, const char* done, int flags);

#endif
