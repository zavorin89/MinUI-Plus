#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include "defines.h" // SWITCHER_MAX, MAX_PATH

int prefixMatch(char* pre, char* str);
int suffixMatch(char* suf, char* str);
int exactMatch(char* str1, char* str2);
int containsString(char* haystack, char* needle);
int hide(char* file_name);

void getDisplayName(const char* in_name, char* out_name);
void getEmuName(const char* in_name, char* out_name);
void getEmuPath(char* emu_name, char* pak_path);

void normalizeNewline(char* line);
void trimTrailingNewlines(char* line);
void trimSortingMeta(char** str);

int exists(char* path);
void touch(char* path);
void putFile(char* path, char* contents);
char* allocFile(char* path); // caller must free
void getFile(char* path, char* buffer, size_t buffer_size);
void putInt(char* path, int value);
int getInt(char* path);

uint64_t getMicroseconds(void);

#define PLAYTIME_SESSION_GAP 3600 // launches closer together than this continue the same session
typedef struct PlayTime {
	int seconds; // total
	int sessions;
	int last_played; // unix time the most recent session was last saved, 0 if unknown
	int last_session; // seconds played in the most recent session
} PlayTime;
int parsePlayTimeLine(char* line, char** path, PlayTime* out); // modifies line, returns 0 if malformed
int getPlayTime(char* path, PlayTime* out); // path relative to SDCARD_PATH, returns 1 if found
void setPlayTime(char* path, PlayTime* time);
void removePlayTime(char* path); // eg. for a deleted game
int getPlayTimeTotals(int* total_seconds, char* top_path, int* top_seconds); // returns number of games
int getAveragePlayTime(PlayTime* time); // seconds per session
void formatPlayTime(int seconds, char* out); // eg. "12h 34m"

// the game switcher's list, paths are relative to SDCARD_PATH
int Switcher_load(char paths[SWITCHER_MAX][MAX_PATH]); // returns how many
int Switcher_has(char* path);
int Switcher_add(char* path); // 1 when added, 0 when already there, -1 when full
void Switcher_remove(char* path);

#endif
