#ifndef RA_H
#define RA_H

#include <stddef.h>
#include <stdint.h>
#include "sdl.h"

struct retro_memory_map;
typedef void (*RA_GetMemory)(unsigned id, uint8_t** data, size_t* size); // wraps the core's retro_get_memory_*

// starts sign in and game identification in the background when .userdata/shared/retroachievements.txt exists
// rom_data (NULL when the core loads from disk) is the game already in memory, hashed instead of reading the file again
void RA_init(const char* core_tag, const char* rom_path, const void* rom_data, size_t rom_size, const struct retro_memory_map* mmap, RA_GetMemory get_memory);
void RA_doFrame(void); // once per emulated frame
void RA_update(void); // once per main loop iteration, shows and hides notifications
void RA_quit(void); // waits briefly for pending unlocks to be sent

int RA_hasAchievements(void); // signed in and the game has a set
int RA_isLoading(void); // still signing in or identifying the game
int RA_isHardcore(void); // hardcore is on for this game: save states would end it
void RA_stateLoaded(void); // call after loading a save state, drops a hardcore session to softcore
void RA_notify(const char* title, const char* text, const char* detail); // a popup like the achievement ones
typedef void (*RA_SleepCallback)(void);
void RA_showAchievements(SDL_Surface* screen, RA_SleepCallback before_sleep, RA_SleepCallback after_sleep); // returns on B

#endif
