#ifndef RA_CACHE_H
#define RA_CACHE_H

#include <stddef.h>
#include <stdint.h>

// Offline RetroAchievements: server responses needed to start a game are cached on the card, unlocks earned
// offline are queued and sent later. Shared by minarch and the Achievements sync tool.

#define RA_CREDS_PATH SHARED_USERDATA_PATH "/retroachievements.txt"
#define RA_CACHE_DIR SHARED_USERDATA_PATH "/.achievements"
#define RA_HARDCORE_PATH RA_CACHE_DIR "/hardcore.txt" // exists when hardcore mode is on
#define RA_CA_PATH RES_PATH "/cacert.pem"

typedef struct RA_Creds {
	char username[128];
	char password[128];
	char token[128];
} RA_Creds;
int RACache_readCreds(RA_Creds* creds); // returns 1 when there's a username and a password or token
void RACache_saveToken(const char* username, const char* token); // drops the password

uint32_t RACache_consoleForTag(const char* tag); // 0 when unsupported

// one cache file per request that's needed to start a game (sign in, game data, unlock state)
int RACache_key(const char* post_data, char* key, size_t size); // returns 0 for requests that aren't cached
int RACache_has(const char* key);
char* RACache_read(const char* key, size_t* length); // caller frees, NULL when missing
void RACache_write(const char* key, const char* body, size_t length);

// saves a response for offline use: 200s as the request's cache file, 404s ("unknown game") as a marker so
// they aren't asked about again
void RACache_store(const char* post_data, int status, const char* body, size_t length);
int RACache_isKnownUnknown(const char* key); // a 404 was stored for this key

// what to answer when offline: the cached body, or a minimal success for requests that can wait (caller frees)
char* RACache_offlineResponse(const char* post_data, size_t* length, int* status);

// achievement badges, one pack per game (RA_CACHE_DIR/badges/<game id>.bin) because thousands of tiny
// PNGs waste most of a FAT32 card's space. Pack: "RAB1", uint32 count, count x {char name[8], uint32 offset,
// uint32 size}, then the PNG data (little-endian, offsets from the start of the file).
typedef char RA_BadgeName[8];
int RACache_fetchBadges(uint32_t game_id, const RA_BadgeName* names, int count, const char* user_agent); // downloads the missing ones, returns how many are in the pack
unsigned char* RACache_loadBadge(uint32_t game_id, const char* name, size_t* size); // PNG data, caller frees, NULL when missing

int RACache_pendingCount(void);
// sends queued unlocks with their original unlock times, returns how many were accepted
int RACache_sendPending(const RA_Creds* creds, const char* user_agent);

#endif
