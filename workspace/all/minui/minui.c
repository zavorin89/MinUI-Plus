#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <msettings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#if defined(__arm__) && defined(__linux__)
#include <ucontext.h>
#endif

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "keyboard.h"
#include "bgm.h"

///////////////////////////////////////

typedef struct Array {
	int count;
	int capacity;
	void** items;
} Array;

static Array* Array_new(void) {
	Array* self = malloc(sizeof(Array));
	self->count = 0;
	self->capacity = 8;
	self->items = malloc(sizeof(void*) * self->capacity);
	return self;
}
static void Array_push(Array* self, void* item) {
	if (self->count>=self->capacity) {
		self->capacity *= 2;
		self->items = realloc(self->items, sizeof(void*) * self->capacity);
	}
	self->items[self->count++] = item;
}
static void Array_unshift(Array* self, void* item) {
	if (self->count==0) return Array_push(self, item);
	Array_push(self, NULL); // ensures we have enough capacity
	for (int i=self->count-2; i>=0; i--) {
		self->items[i+1] = self->items[i];
	}
	self->items[0] = item;
}
static void* Array_pop(Array* self) {
	if (self->count==0) return NULL;
	return self->items[--self->count];
}
static void Array_insert(Array* self, int index, void* item) {
	Array_push(self, NULL); // ensures we have enough capacity
	for (int i=self->count-1; i>index; i--) {
		self->items[i] = self->items[i-1];
	}
	self->items[index] = item;
}
static void* Array_remove(Array* self, int index) {
	void* item = self->items[index];
	for (int i=index; i<self->count-1; i++) {
		self->items[i] = self->items[i+1];
	}
	self->count -= 1;
	return item;
}
static void Array_reverse(Array* self) {
	int end = self->count-1;
	int mid = self->count/2;
	for (int i=0; i<mid; i++) {
		void* item = self->items[i];
		self->items[i] = self->items[end-i];
		self->items[end-i] = item;
	}
}
static void Array_free(Array* self) {
	free(self->items); 
	free(self);
}

static int StringArray_indexOf(Array* self, char* str) {
	for (int i=0; i<self->count; i++) {
		if (exactMatch(self->items[i], str)) return i;
	}
	return -1;
}
static void StringArray_free(Array* self) {
	for (int i=0; i<self->count; i++) {
		free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////

typedef struct Hash {
	Array* keys;
	Array* values;
} Hash; // not really a hash

static Hash* Hash_new(void) {
	Hash* self = malloc(sizeof(Hash));
	self->keys = Array_new();
	self->values = Array_new();
	return self;
}
static void Hash_free(Hash* self) {
	StringArray_free(self->keys);
	StringArray_free(self->values);
	free(self);
}
static void Hash_set(Hash* self, char* key, char* value) {
	Array_push(self->keys, strdup(key));
	Array_push(self->values, strdup(value));
}
static char* Hash_get(Hash* self, char* key) {
	int i = StringArray_indexOf(self->keys, key);
	if (i==-1) return NULL;
	return self->values->items[i];
}

///////////////////////////////////////

enum EntryType {
	ENTRY_DIR,
	ENTRY_PAK,
	ENTRY_ROM,
};
typedef struct Entry {
	char* path;
	char* name;
	char* unique;
	int type;
	int alpha; // index in parent Directory's alphas Array, which points to the index of an Entry in its entries Array :sweat_smile:
	int favorite;
	PlayTime play; // only filled in for Most Played
} Entry;

static Entry* Entry_new(char* path, int type) {
	char display_name[256];
	getDisplayName(path, display_name);
	Entry* self = malloc(sizeof(Entry));
	self->path = strdup(path);
	self->name = strdup(display_name);
	self->unique = NULL;
	self->type = type;
	self->alpha = 0;
	self->favorite = 0;
	memset(&self->play, 0, sizeof(PlayTime));
	return self;
}
static void Entry_free(Entry* self) {
	free(self->path);
	free(self->name);
	if (self->unique) free(self->unique);
	free(self);
}

static int EntryArray_indexOf(Array* self, char* path) {
	for (int i=0; i<self->count; i++) {
		Entry* entry = self->items[i];
		if (exactMatch(entry->path, path)) return i;
	}
	return -1;
}
static int EntryArray_sortEntry(const void* a, const void* b) {
	Entry* item1 = *(Entry**)a;
	Entry* item2 = *(Entry**)b;
	return strcasecmp(item1->name, item2->name);
}
static void EntryArray_sort(Array* self) {
	qsort(self->items, self->count, sizeof(void*), EntryArray_sortEntry);
}

static void EntryArray_free(Array* self) {
	for (int i=0; i<self->count; i++) {
		Entry_free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////

#define INT_ARRAY_MAX 27
typedef struct IntArray {
	int count;
	int items[INT_ARRAY_MAX];
} IntArray;
static IntArray* IntArray_new(void) {
	IntArray* self = malloc(sizeof(IntArray));
	self->count = 0;
	memset(self->items, 0, sizeof(int) * INT_ARRAY_MAX);
	return self;
}
static void IntArray_push(IntArray* self, int i) {
	self->items[self->count++] = i;
}
static void IntArray_free(IntArray* self) {
	free(self);
}

///////////////////////////////////////

typedef struct Directory {
	char* path;
	char* name;
	Array* entries;
	IntArray* alphas;
	// rendering
	int selected;
	int start;
	int end;
} Directory;

static int getIndexChar(char* str) {
	char i = 0;
	char c = tolower(str[0]);
	if (c>='a' && c<='z') i = (c-'a')+1;
	return i;
}

static void getUniqueName(Entry* entry, char* out_name) {
	char* filename = strrchr(entry->path, '/')+1;
	char emu_tag[256];
	getEmuName(entry->path, emu_tag);
	
	char *tmp;
	strcpy(out_name, entry->name);
	tmp = out_name + strlen(out_name);
	strcpy(tmp, " (");
	tmp = out_name + strlen(out_name);
	strcpy(tmp, emu_tag);
	tmp = out_name + strlen(out_name);
	strcpy(tmp, ")");
}

enum {
	FAVORITES_SORT_NAME,
	FAVORITES_SORT_SYSTEM,
	FAVORITES_SORT_RECENT, // most recently added first
	FAVORITES_SORT_COUNT,
};
static char* favorites_sort_labels[FAVORITES_SORT_COUNT] = { "Sorted A-Z", "Sorted by system", "Recently added first" };
#define FAVORITES_SORT_PATH SHARED_USERDATA_PATH "/.minui/favorites_sort.txt"
static int favorites_sort = FAVORITES_SORT_NAME;

// game folders show as a list or a carousel of box art, switched with SELECT
#define VIEW_LIST 0
#define VIEW_CAROUSEL 1
#define VIEW_MODE_PATH SHARED_USERDATA_PATH "/.minui/view.txt"
static int view_mode = VIEW_LIST;


// plain game folders can be re-sorted with L2/R2 (Favorites cycles its own sorts)
enum {
	FOLDER_SORT_AZ,
	FOLDER_SORT_ZA,
	FOLDER_SORT_RECENT, // last played first, never played after (A-Z)
	FOLDER_SORT_FAVORITES, // favorites first, each group A-Z
	FOLDER_SORT_COUNT,
};
static char* folder_sort_labels[FOLDER_SORT_COUNT] = { "Sorted A-Z", "Sorted Z-A", "Last played first", "Favorites first" };
#define FOLDER_SORT_PATH SHARED_USERDATA_PATH "/.minui/folder_sort.txt"
static int folder_sort = FOLDER_SORT_AZ;

static int isSortableFolder(char* path) { // a folder of games read from disk (not Tools, or one of the faux lists)
	return prefixMatch(ROMS_PATH, path) && !suffixMatch(".m3u", path);
}
static void Directory_sort(Directory* self);

static void Directory_index(Directory* self) {
	int is_collection = prefixMatch(COLLECTIONS_PATH, self->path);
	int is_unsorted_favorites = exactMatch(FAUX_FAVORITES_PATH, self->path) && favorites_sort!=FAVORITES_SORT_NAME;
	int is_unsorted_folder = isSortableFolder(self->path) && folder_sort>=FOLDER_SORT_RECENT; // letters are scattered, L1/R1 has nothing to jump between
	int skip_index = exactMatch(FAUX_RECENT_PATH, self->path) || exactMatch(FAUX_MOSTPLAYED_PATH, self->path) || exactMatch(FAUX_SWITCHER_PATH, self->path) || is_collection || is_unsorted_favorites || is_unsorted_folder; // not alphabetized
	
	Hash* map = NULL;
	char map_path[256];
	sprintf(map_path, "%s/map.txt", is_collection ? COLLECTIONS_PATH : self->path);
	if (exists(map_path)) {
		FILE* file = fopen(map_path, "r");
		if (file) {
			map = Hash_new();
			char line[256];
			while (fgets(line,256,file)!=NULL) {
				normalizeNewline(line);
				trimTrailingNewlines(line);
				if (strlen(line)==0) continue; // skip empty lines

				char* tmp = strchr(line,'\t');
				if (tmp) {
					tmp[0] = '\0';
					char* key = line;
					char* value = tmp+1;
					Hash_set(map, key, value);
				}
			}
			fclose(file);
			
			int resort = 0;
			int filter = 0;
			for (int i=0; i<self->entries->count; i++) {
				Entry* entry = self->entries->items[i];
				char* filename = strrchr(entry->path, '/')+1;
				char* alias = Hash_get(map, filename);
				if (alias) {
					free(entry->name);
					entry->name = strdup(alias);
					resort = 1;
					if (!filter && hide(entry->name)) filter = 1;
				}
			}
			
			if (filter) {
				Array* entries = Array_new();
				for (int i=0; i<self->entries->count; i++) {
					Entry* entry = self->entries->items[i];
					if (hide(entry->name)) {
						Entry_free(entry);
					}
					else {
						Array_push(entries, entry);
					}
				}
				Array_free(self->entries); // not EntryArray_free because we've just moved the entries from the original to the filtered one!
				self->entries = entries;
			}
			if (resort) {
				if (isSortableFolder(self->path)) Directory_sort(self);
				else EntryArray_sort(self->entries);
			}
		}
	}
	
	Entry* prior = NULL;
	int alpha = -1;
	int index = 0;
	for (int i=0; i<self->entries->count; i++) {
		Entry* entry = self->entries->items[i];
		if (map) {
			char* filename = strrchr(entry->path, '/')+1;
			char* alias = Hash_get(map, filename);
			if (alias) {
				free(entry->name);
				entry->name = strdup(alias);
			}
		}
		
		if (prior!=NULL && exactMatch(prior->name, entry->name)) {
			if (prior->unique) free(prior->unique);
			if (entry->unique) free(entry->unique);
			
			char* prior_filename = strrchr(prior->path, '/')+1;
			char* entry_filename = strrchr(entry->path, '/')+1;
			if (exactMatch(prior_filename, entry_filename)) {
				char prior_unique[256];
				char entry_unique[256];
				getUniqueName(prior, prior_unique);
				getUniqueName(entry, entry_unique);
				
				prior->unique = strdup(prior_unique);
				entry->unique = strdup(entry_unique);
			}
			else {
				prior->unique = strdup(prior_filename);
				entry->unique = strdup(entry_filename);
			}
		}

		if (!skip_index) {
			int a = getIndexChar(entry->name);
			if (a!=alpha) {
				index = self->alphas->count;
				IntArray_push(self->alphas, i);
				alpha = a;
			}
			entry->alpha = index;
		}
		
		prior = entry;
	}
	
	if (map) Hash_free(map);
}

static Array* getRoot(void);
static Array* getRecents(void);
static Array* getFavorites(void);
static Array* getMostPlayed(void);
static Array* getSwitcher(void);
static void Directory_loadLastPlayed(Directory* self);
static int EntryArray_sortByLastPlayed(const void* a, const void* b);
static void Directory_markFavorites(Directory* self);
static Array* getCollection(char* path);
static Array* getDiscs(char* path);
static Array* getEntries(char* path);

static Directory* Directory_new(char* path, int selected) {
	char display_name[256];
	getDisplayName(path, display_name);
	
	Directory* self = malloc(sizeof(Directory));
	self->path = strdup(path);
	self->name = strdup(display_name);
	if (exactMatch(path, SDCARD_PATH)) {
		self->entries = getRoot();
	}
	else if (exactMatch(path, FAUX_RECENT_PATH)) {
		self->entries = getRecents();
	}
	else if (exactMatch(path, FAUX_FAVORITES_PATH)) {
		self->entries = getFavorites();
	}
	else if (exactMatch(path, FAUX_MOSTPLAYED_PATH)) {
		self->entries = getMostPlayed();
	}
	else if (exactMatch(path, FAUX_SWITCHER_PATH)) {
		self->entries = getSwitcher();
	}
	else if (!exactMatch(path, COLLECTIONS_PATH) && prefixMatch(COLLECTIONS_PATH, path) && suffixMatch(".txt", path)) {
		self->entries = getCollection(path);
	}
	else if (suffixMatch(".m3u", path)) {
		self->entries = getDiscs(path);
	}
	else {
		self->entries = getEntries(path);
	}
	self->alphas = IntArray_new();
	self->selected = selected;
	if (exactMatch(path, FAUX_SWITCHER_PATH)) { // the game you were just in comes first
		Directory_loadLastPlayed(self);
		qsort(self->entries->items, self->entries->count, sizeof(void*), EntryArray_sortByLastPlayed);
	}
	Directory_markFavorites(self); // before sorting, which can put favorites first
	if (isSortableFolder(path)) Directory_sort(self);
	Directory_index(self);
	return self;
}
static void Directory_reindex(Directory* self) { // after adding, removing, or re-sorting entries
	self->alphas->count = 0;
	for (int i=0; i<self->entries->count; i++) {
		Entry* entry = self->entries->items[i];
		entry->alpha = 0;
	}
	Directory_index(self);

	// keep the selection in range and on screen
	int total = self->entries->count;
	if (self->selected>=total) self->selected = total>0 ? total-1 : 0;
	int rows = MIN(total, MAIN_ROW_COUNT);
	if (self->selected<self->start) self->start = self->selected;
	if (self->selected>=self->start+rows) self->start = self->selected-rows+1;
	if (self->start+rows>total) self->start = total-rows;
	if (self->start<0) self->start = 0;
	self->end = self->start + rows;
}
static void Directory_free(Directory* self) {
	free(self->path);
	free(self->name);
	EntryArray_free(self->entries);
	IntArray_free(self->alphas);
	free(self);
}

static void DirectoryArray_pop(Array* self) {
	Directory_free(Array_pop(self));
}
static void DirectoryArray_free(Array* self) {
	for (int i=0; i<self->count; i++) {
		Directory_free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////

typedef struct Recent {
	char* path; // NOTE: this is without the SDCARD_PATH prefix!
	char* alias;
	int available;
} Recent;
 // yiiikes
static char* recent_alias = NULL;

static int hasEmu(char* emu_name);
static Recent* Recent_new(char* path, char* alias) {
	Recent* self = malloc(sizeof(Recent));

	char sd_path[256]; // only need to get emu name
	sprintf(sd_path, "%s%s", SDCARD_PATH, path);

	char emu_name[256];
	getEmuName(sd_path, emu_name);
	
	self->path = strdup(path);
	self->alias = alias ? strdup(alias) : NULL;
	self->available = hasEmu(emu_name);
	return self;
}
static void Recent_free(Recent* self) {
	free(self->path);
	if (self->alias) free(self->alias);
	free(self);
}

static int RecentArray_indexOf(Array* self, char* str) {
	for (int i=0; i<self->count; i++) {
		Recent* item = self->items[i];
		if (exactMatch(item->path, str)) return i;
	}
	return -1;
}
static void RecentArray_free(Array* self) {
	for (int i=0; i<self->count; i++) {
		Recent_free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////

static Directory* top;
static Array* stack; // DirectoryArray

// the game switcher: holding MENU shows the games pinned to it, as screenshots from where each was left off
static int inSwitcher(void) {
	return top && exactMatch(top->path, FAUX_SWITCHER_PATH);
}
// lists of games can switch to the carousel; the main menu and Tools are always lists
static int canCarousel(void) {
	if (!top || stack->count<2) return 0;
	return prefixMatch(ROMS_PATH, top->path) || prefixMatch(COLLECTIONS_PATH, top->path)
		|| exactMatch(top->path, FAUX_RECENT_PATH) || exactMatch(top->path, FAUX_FAVORITES_PATH) || exactMatch(top->path, FAUX_MOSTPLAYED_PATH);
}
static int showsCarousel(void) {
	return inSwitcher() || (view_mode==VIEW_CAROUSEL && canCarousel());
}
static Array* recents; // RecentArray
static Array* favorites; // RecentArray

static int quit = 0;
static int can_resume = 0;
static int should_resume = 0; // set to 1 on BTN_RESUME but only if can_resume==1
static int simple_mode = 0;
static char slot_path[256];
static int resume_slot = -1; // overrides the slot in slot_path when resuming, -1 to use it

static int restore_depth = -1;
static int restore_relative = -1;
static int restore_selected = 0;
static int restore_start = 0;
static int restore_end = 0;

///////////////////////////////////////

#define MAX_RECENTS 24 // a multiple of all menu rows
static void RecentArray_save(Array* self, char* path) {
	FILE* file = fopen(path, "w");
	if (file) {
		for (int i=0; i<self->count; i++) {
			Recent* recent = self->items[i];
			fputs(recent->path, file);
			if (recent->alias) {
				fputs("\t", file);
				fputs(recent->alias, file);
			}
			putc('\n', file);
		}
		fclose(file);
	}
}
static void saveRecents(void) {
	RecentArray_save(recents, RECENT_PATH);
}
static void addRecent(char* path, char* alias) {
	path += strlen(SDCARD_PATH); // makes paths platform agnostic
	int id = RecentArray_indexOf(recents, path);
	if (id==-1) { // add
		while (recents->count>=MAX_RECENTS) {
			Recent_free(Array_pop(recents));
		}
		Array_unshift(recents, Recent_new(path, alias));
	}
	else if (id>0) { // bump to top
		for (int i=id; i>0; i--) {
			void* tmp = recents->items[i-1];
			recents->items[i-1] = recents->items[i];
			recents->items[i] = tmp;
		}
	}
	saveRecents();
}

static int hasEmu(char* emu_name) {
	// asked for every recent, favorite and system on each start, so each answer is remembered
	#define EMU_CACHE_SIZE 32
	static struct { char name[64]; int has; } known[EMU_CACHE_SIZE];
	static int known_count = 0;
	for (int i=0; i<known_count; i++) if (exactMatch(known[i].name, emu_name)) return known[i].has;

	char pak_path[256];
	sprintf(pak_path, "%s/Emus/%s.pak/launch.sh", PAKS_PATH, emu_name);
	int has = exists(pak_path);
	if (!has) {
		sprintf(pak_path, "%s/Emus/%s/%s.pak/launch.sh", SDCARD_PATH, PLATFORM, emu_name);
		has = exists(pak_path);
	}
	if (known_count<EMU_CACHE_SIZE && strlen(emu_name)<sizeof(known[0].name)) {
		strcpy(known[known_count].name, emu_name);
		known[known_count++].has = has;
	}
	return has;
}
static int hasCue(char* dir_path, char* cue_path) { // NOTE: dir_path not rom_path
	char* tmp = strrchr(dir_path, '/') + 1; // folder name
	sprintf(cue_path, "%s/%s.cue", dir_path, tmp);
	return exists(cue_path);
}
static int hasM3u(char* rom_path, char* m3u_path) { // NOTE: rom_path not dir_path
	char* tmp;
	
	strcpy(m3u_path, rom_path);
	tmp = strrchr(m3u_path, '/') + 1;
	tmp[0] = '\0';
	
	// path to parent directory
	char base_path[256];
	strcpy(base_path, m3u_path);
	
	tmp = strrchr(m3u_path, '/');
	tmp[0] = '\0';
	
	// get parent directory name
	char dir_name[256];
	tmp = strrchr(m3u_path, '/');
	strcpy(dir_name, tmp);
	
	// dir_name is also our m3u file name
	tmp = m3u_path + strlen(m3u_path); 
	strcpy(tmp, dir_name);

	// add extension
	tmp = m3u_path + strlen(m3u_path);
	strcpy(tmp, ".m3u");
	
	return exists(m3u_path);
}

static int hasRecents(void) {
	LOG_info("hasRecents %s\n", RECENT_PATH);
	int has = 0;
	
	Array* parent_paths = Array_new();
	if (exists(CHANGE_DISC_PATH)) {
		char sd_path[256];
		getFile(CHANGE_DISC_PATH, sd_path, 256);
		if (exists(sd_path)) {
			char* disc_path = sd_path + strlen(SDCARD_PATH); // makes path platform agnostic
			Recent* recent = Recent_new(disc_path, NULL);
			if (recent->available) has += 1;
			Array_push(recents, recent);
		
			char parent_path[256];
			strcpy(parent_path, disc_path);
			char* tmp = strrchr(parent_path, '/') + 1;
			tmp[0] = '\0';
			Array_push(parent_paths, strdup(parent_path));
		}
		unlink(CHANGE_DISC_PATH);
	}
	
	FILE* file = fopen(RECENT_PATH, "r"); // newest at top
	if (file) {
		char line[256];
		while (fgets(line,256,file)!=NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line)==0) continue; // skip empty lines
			
			// LOG_info("line: %s\n", line);
			
			char* path = line;
			char* alias = NULL;
			char* tmp = strchr(line,'\t');
			if (tmp) {
				tmp[0] = '\0';
				alias = tmp+1;
			}
			
			char sd_path[256];
			sprintf(sd_path, "%s%s", SDCARD_PATH, path);
			if (exists(sd_path)) {
				if (recents->count<MAX_RECENTS) {
					// this logic replaces an existing disc from a multi-disc game with the last used
					char m3u_path[256];
					if (hasM3u(sd_path, m3u_path)) { // TODO: this might tank launch speed
						char parent_path[256];
						strcpy(parent_path, path);
						char* tmp = strrchr(parent_path, '/') + 1;
						tmp[0] = '\0';
						
						int found = 0;
						for (int i=0; i<parent_paths->count; i++) {
							char* path = parent_paths->items[i];
							if (prefixMatch(path, parent_path)) {
								found = 1;
								break;
							}
						}
						if (found) continue;
						
						Array_push(parent_paths, strdup(parent_path));
					}
					
					// LOG_info("path:%s alias:%s\n", path, alias);
					
					Recent* recent = Recent_new(path, alias);
					if (recent->available) has += 1;
					Array_push(recents, recent);
				}
			}
		}
		fclose(file);
	}
	
	saveRecents();
	
	StringArray_free(parent_paths);
	return has>0;
}
static int hasCollections(void) {
	int has = 0;
	if (!exists(COLLECTIONS_PATH)) return has;
	
	DIR *dh = opendir(COLLECTIONS_PATH);
	struct dirent *dp;
	while((dp = readdir(dh)) != NULL) {
		if (hide(dp->d_name)) continue;
		has = 1;
		break;
	}
	closedir(dh);
	return has;
}

///////////////////////////////////////

#define FAVORITE_DOT " \xE2\x80\xA2" // U+2022, the menu font has no star

static void getSystemName(char* path, char* out_name) { // eg. "Game Boy Advance" for anything in "Roms/Game Boy Advance (MGBA)/"
	char folder[256];
	strcpy(folder, path + strlen(ROMS_PATH "/"));
	char* tmp = strchr(folder, '/');
	if (tmp) tmp[0] = '\0';
	getDisplayName(folder, out_name);
}
static int EntryArray_sortBySystem(const void* a, const void* b) {
	Entry* item1 = *(Entry**)a;
	Entry* item2 = *(Entry**)b;
	char system1[256];
	char system2[256];
	getSystemName(item1->path, system1);
	getSystemName(item2->path, system2);
	int order = strcasecmp(system1, system2);
	return order ? order : strcasecmp(item1->name, item2->name);
}

static void loadFavorites(void) {
	FILE* file = fopen(FAVORITES_PATH, "r");
	if (file) {
		char line[256];
		while (fgets(line,256,file)!=NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line)==0) continue; // skip empty lines

			char* path = line;
			char* alias = NULL;
			char* tmp = strchr(line,'\t');
			if (tmp) {
				tmp[0] = '\0';
				alias = tmp+1;
			}

			// NOTE: missing roms are kept (but hidden) so swapping cards doesn't lose them
			if (RecentArray_indexOf(favorites, path)!=-1) continue;
			Array_push(favorites, Recent_new(path, alias));
		}
		fclose(file);
	}
}
static void saveFavorites(void) {
	RecentArray_save(favorites, FAVORITES_PATH);
}
static int hasM3uCached(char* rom_path, char* m3u_path) { // hasM3u() only depends on the parent folder
	// several slots because a collated list (eg. GBA + MGBA) alternates between folders
	#define M3U_CACHE_SIZE 8
	static struct {
		char dir[256];
		char m3u[256];
		int has;
	} cache[M3U_CACHE_SIZE];
	static int next_slot = 0;

	char dir_path[256];
	strcpy(dir_path, rom_path);
	char* tmp = strrchr(dir_path, '/');
	if (tmp) tmp[0] = '\0';

	for (int i=0; i<M3U_CACHE_SIZE; i++) {
		if (cache[i].dir[0] && exactMatch(cache[i].dir, dir_path)) {
			strcpy(m3u_path, cache[i].m3u);
			return cache[i].has;
		}
	}

	int i = next_slot;
	next_slot = (next_slot + 1) % M3U_CACHE_SIZE;
	strcpy(cache[i].dir, dir_path);
	cache[i].has = hasM3u(rom_path, cache[i].m3u);
	strcpy(m3u_path, cache[i].m3u);
	return cache[i].has;
}
static int getFavoritePath(Entry* entry, char* out_path) {
	// stores the same path addRecent() would so recents,
	// resume states, and thumbnails all line up
	if (!prefixMatch(ROMS_PATH "/", entry->path)) return 0;

	char path[256];
	if (entry->type==ENTRY_DIR) {
		// only folders that launch a game directly
		if (!hasCue(entry->path, path)) {
			char* tmp = strrchr(path, '.') + 1; // extension
			strcpy(tmp, "m3u"); // replace with m3u
			if (!exists(path)) return 0;
		}
	}
	else strcpy(path, entry->path);

	if (entry->type!=ENTRY_PAK) {
		char m3u_path[256];
		if (hasM3uCached(path, m3u_path)) strcpy(path, m3u_path);
	}

	strcpy(out_path, path + strlen(SDCARD_PATH)); // makes paths platform agnostic
	return 1;
}
static int isFavoriteAvailable(Recent* favorite) {
	if (!favorite->available) return 0;
	char sd_path[256];
	sprintf(sd_path, "%s%s", SDCARD_PATH, favorite->path);
	return exists(sd_path);
}
static int hasFavorites(void) {
	for (int i=0; i<favorites->count; i++) {
		if (isFavoriteAvailable(favorites->items[i])) return 1;
	}
	return 0;
}
static Array* getFavorites(void) {
	Array* entries = Array_new();
	for (int n=0; n<favorites->count; n++) {
		// favorites.txt is in the order games were added
		int i = favorites_sort==FAVORITES_SORT_RECENT ? favorites->count-1-n : n;
		Recent* favorite = favorites->items[i];
		if (!isFavoriteAvailable(favorite)) continue;

		char sd_path[256];
		sprintf(sd_path, "%s%s", SDCARD_PATH, favorite->path);
		int type = suffixMatch(".pak", sd_path) ? ENTRY_PAK : ENTRY_ROM;
		Entry* entry = Entry_new(sd_path, type);
		if (favorite->alias) {
			free(entry->name);
			entry->name = strdup(favorite->alias);
		}
		Array_push(entries, entry);
	}
	if (favorites_sort==FAVORITES_SORT_NAME) EntryArray_sort(entries);
	else if (favorites_sort==FAVORITES_SORT_SYSTEM) qsort(entries->items, entries->count, sizeof(void*), EntryArray_sortBySystem);
	return entries;
}
static int EntryArray_sortReverse(const void* a, const void* b) {
	return EntryArray_sortEntry(b, a);
}
static int EntryArray_sortByLastPlayed(const void* a, const void* b) {
	Entry* item1 = *(Entry**)a;
	Entry* item2 = *(Entry**)b;
	if (item1->play.last_played!=item2->play.last_played) return item1->play.last_played<item2->play.last_played ? 1 : -1;
	return strcasecmp(item1->name, item2->name);
}
static int EntryArray_sortFavoritesFirst(const void* a, const void* b) {
	Entry* item1 = *(Entry**)a;
	Entry* item2 = *(Entry**)b;
	if (item1->favorite!=item2->favorite) return item2->favorite - item1->favorite;
	return strcasecmp(item1->name, item2->name);
}
// fills each entry's play.last_played from play time, falling back to its place in Recently Played
static void Directory_loadLastPlayed(Directory* self) {
	Hash* last = Hash_new();
	FILE* file = fopen(PLAYTIME_PATH, "r");
	if (file) {
		char line[MAX_PATH];
		while (fgets(line, sizeof(line), file)) {
			char* path;
			PlayTime time;
			if (!parsePlayTimeLine(line, &path, &time) || !time.last_played) continue;
			char value[16];
			sprintf(value, "%d", time.last_played);
			Hash_set(last, path, value);
		}
		fclose(file);
	}
	int sd_len = strlen(SDCARD_PATH);
	for (int i=0; i<self->entries->count; i++) {
		Entry* entry = self->entries->items[i];
		char* path = entry->path + sd_len; // play time and recents leave off SDCARD_PATH
		char* value = Hash_get(last, path);
		entry->play.last_played = value ? atoi(value) : 0;
		if (!entry->play.last_played) {
			int r = RecentArray_indexOf(recents, path); // played before play time was tracked
			if (r!=-1) entry->play.last_played = recents->count - r; // tiny, so after anything with a real time
		}
	}
	Hash_free(last);
}
static void Directory_sort(Directory* self) {
	switch (folder_sort) {
		case FOLDER_SORT_ZA:
			qsort(self->entries->items, self->entries->count, sizeof(void*), EntryArray_sortReverse);
			break;
		case FOLDER_SORT_RECENT:
			Directory_loadLastPlayed(self);
			qsort(self->entries->items, self->entries->count, sizeof(void*), EntryArray_sortByLastPlayed);
			break;
		case FOLDER_SORT_FAVORITES:
			qsort(self->entries->items, self->entries->count, sizeof(void*), EntryArray_sortFavoritesFirst);
			break;
		default:
			EntryArray_sort(self->entries);
			break;
	}
}

static void Directory_markFavorites(Directory* self) {
	if (exactMatch(self->path, SDCARD_PATH) || exactMatch(self->path, FAUX_FAVORITES_PATH)) return;

	char path[256];
	for (int i=0; i<self->entries->count; i++) {
		Entry* entry = self->entries->items[i];
		entry->favorite = favorites->count && getFavoritePath(entry, path) && RecentArray_indexOf(favorites, path)!=-1;
	}
}
static void updateRootFavorites(void) { // show or hide Favorites in the root without rebuilding it
	Directory* root = stack->items[0];
	int i = EntryArray_indexOf(root->entries, FAUX_FAVORITES_PATH);
	int has = hasFavorites();
	if (has && i==-1) {
		int at = EntryArray_indexOf(root->entries, FAUX_RECENT_PATH) + 1; // after Recently Played (or first)
		Array_insert(root->entries, at, Entry_new(FAUX_FAVORITES_PATH, ENTRY_DIR));
		if (root->selected>=at) root->selected += 1;
	}
	else if (!has && i!=-1) {
		Entry_free(Array_remove(root->entries, i));
		if (root->selected>i) root->selected -= 1;
	}
	else return;
	Directory_reindex(root);
}
static void toggleFavorite(void) {
	Entry* entry = top->entries->items[top->selected];

	char path[256];
	if (!getFavoritePath(entry, path)) return;

	int i = RecentArray_indexOf(favorites, path);
	if (i==-1) {
		// disc names ("Disc 1") make bad aliases, fall back to the m3u name
		char* alias = suffixMatch(".m3u", top->path) ? NULL : entry->name;
		Array_push(favorites, Recent_new(path, alias));
	}
	else Recent_free(Array_remove(favorites, i));
	saveFavorites();

	if (exactMatch(top->path, FAUX_FAVORITES_PATH)) {
		Entry_free(Array_remove(top->entries, top->selected));
		Directory_reindex(top);
	}
	else Directory_markFavorites(top); // also catches other discs of the same game

	updateRootFavorites();
}

///////////////////////////////////////

#define MOST_PLAYED_COUNT 10
static char info_top_name[256] = ""; // shown on the info screen's Most played row
static int most_played_from_info = 0; // B in Most Played returns to the info screen

static void formatShortPlayTime(int seconds, char* out) { // minutes stop mattering past 10 hours and would crowd the hint bar
	if (seconds>=10*3600) sprintf(out, "%ih", seconds/3600);
	else if (seconds>=3600) sprintf(out, "%ih%im", seconds/3600, (seconds/60)%60); // no space, same reason
	else formatPlayTime(seconds, out);
}
static int EntryArray_sortByPlayTime(const void* a, const void* b) {
	Entry* item1 = *(Entry**)a;
	Entry* item2 = *(Entry**)b;
	return item2->play.seconds - item1->play.seconds;
}
static Array* getMostPlayed(void) {
	Array* entries = Array_new();
	FILE* file = fopen(PLAYTIME_PATH, "r");
	if (!file) return entries;
	
	char line[MAX_PATH];
	while (fgets(line, sizeof(line), file)) {
		char* path;
		PlayTime time;
		if (!parsePlayTimeLine(line, &path, &time) || time.seconds<=0) continue;
		
		char sd_path[MAX_PATH];
		sprintf(sd_path, "%s%s", SDCARD_PATH, path);
		if (!exists(sd_path)) continue;
		char emu_name[256];
		getEmuName(sd_path, emu_name);
		if (!hasEmu(emu_name)) continue;
		
		Entry* entry = Entry_new(sd_path, suffixMatch(".pak", sd_path) ? ENTRY_PAK : ENTRY_ROM);
		entry->play = time;
		
		// use the name it was launched or favorited under (map.txt aliases) when there is one
		Recent* named = NULL;
		int i = RecentArray_indexOf(recents, path);
		if (i!=-1) named = recents->items[i];
		if (!named || !named->alias) {
			i = RecentArray_indexOf(favorites, path);
			if (i!=-1) named = favorites->items[i];
		}
		if (named && named->alias) {
			free(entry->name);
			entry->name = strdup(named->alias);
		}
		Array_push(entries, entry);
	}
	fclose(file);
	
	qsort(entries->items, entries->count, sizeof(void*), EntryArray_sortByPlayTime);
	while (entries->count>MOST_PLAYED_COUNT) Entry_free(Array_pop(entries));
	
	for (int i=0; i<entries->count; i++) {
		Entry* entry = entries->items[i];
		char ranked[256];
		snprintf(ranked, sizeof(ranked), "%i. %s", i+1, entry->name);
		free(entry->name);
		entry->name = strdup(ranked);
	}
	return entries;
}

static int hasRoms(char* dir_name) {
	int has = 0;
	char emu_name[256];
	char rom_path[256];

	getEmuName(dir_name, emu_name);
	
	// check for emu pak
	if (!hasEmu(emu_name)) return has;
	
	// check for at least one non-hidden file (we're going to assume it's a rom)
	sprintf(rom_path, "%s/%s/", ROMS_PATH, dir_name);
	DIR *dh = opendir(rom_path);
	if (dh!=NULL) {
		struct dirent *dp;
		while((dp = readdir(dh)) != NULL) {
			if (hide(dp->d_name)) continue;
			has = 1;
			break;
		}
		closedir(dh);
	}
	// if (!has) printf("No roms for %s!\n", dir_name);
	return has;
}
static Array* getSwitcher(void) {
	Array* entries = Array_new();
	char paths[SWITCHER_MAX][MAX_PATH];
	int count = Switcher_load(paths);
	for (int i=0; i<count; i++) {
		char sd_path[MAX_PATH];
		sprintf(sd_path, "%s%s", SDCARD_PATH, paths[i]);
		if (!exists(sd_path)) continue;
		char emu_name[256];
		getEmuName(sd_path, emu_name);
		if (!hasEmu(emu_name)) continue;
		Entry* entry = Entry_new(sd_path, ENTRY_ROM);
		// use the name it was launched or favorited under (map.txt aliases) when there is one
		Recent* named = NULL;
		int r = RecentArray_indexOf(recents, paths[i]);
		if (r!=-1) named = recents->items[r];
		if (!named || !named->alias) {
			r = RecentArray_indexOf(favorites, paths[i]);
			if (r!=-1) named = favorites->items[r];
		}
		if (named && named->alias) {
			free(entry->name);
			entry->name = strdup(named->alias);
		}
		Array_push(entries, entry);
	}
	return entries;
}
// the path an entry is pinned to the switcher under, relative to SDCARD_PATH, returns 0 if it can't be pinned
static int getSwitcherPath(Entry* entry, char* path) {
	if (!prefixMatch(ROMS_PATH, entry->path)) return 0;
	char sd_path[MAX_PATH];
	strcpy(sd_path, entry->path);
	if (entry->type==ENTRY_DIR) { // a multi-disc folder pins its m3u, like minarch does
		char* name = strrchr(entry->path, '/');
		sprintf(sd_path, "%s%s.m3u", entry->path, name);
		if (!exists(sd_path)) return 0;
	}
	else if (entry->type!=ENTRY_ROM) return 0;
	strcpy(path, sd_path + strlen(SDCARD_PATH));
	return 1;
}

static Array* getRoot(void) {
	Array* root = Array_new();
	
	if (hasRecents()) Array_push(root, Entry_new(FAUX_RECENT_PATH, ENTRY_DIR));
	if (hasFavorites()) Array_push(root, Entry_new(FAUX_FAVORITES_PATH, ENTRY_DIR));

	Array* entries = Array_new();
	DIR* dh = opendir(ROMS_PATH);
	if (dh!=NULL) {
		struct dirent *dp;
		char* tmp;
		char full_path[256];
		sprintf(full_path, "%s/", ROMS_PATH);
		tmp = full_path + strlen(full_path);
		Array* emus = Array_new();
		while((dp = readdir(dh)) != NULL) {
			if (hide(dp->d_name)) continue;
			if (hasRoms(dp->d_name)) {
				strcpy(tmp, dp->d_name);
				Array_push(emus, Entry_new(full_path, ENTRY_DIR));
			}
		}
		EntryArray_sort(emus);
		Entry* prev_entry = NULL;
		for (int i=0; i<emus->count; i++) {
			Entry* entry = emus->items[i];
			if (prev_entry!=NULL) {
				if (exactMatch(prev_entry->name, entry->name)) {
					Entry_free(entry);
					continue;
				}
			}
			Array_push(entries, entry);
			prev_entry = entry;
		}
		Array_free(emus); // just free the array part, entries now owns emus entries
		closedir(dh);
	}
	
	// copied/modded from Directory_index
	// we don't support hidden remaps here
	char map_path[256];
	sprintf(map_path, "%s/map.txt", ROMS_PATH);
	if (entries->count>0 && exists(map_path)) {
		FILE* file = fopen(map_path, "r");
		if (file) {
			Hash* map = Hash_new();
			char line[256];
			while (fgets(line,256,file)!=NULL) {
				normalizeNewline(line);
				trimTrailingNewlines(line);
				if (strlen(line)==0) continue; // skip empty lines

				char* tmp = strchr(line,'\t');
				if (tmp) {
					tmp[0] = '\0';
					char* key = line;
					char* value = tmp+1;
					Hash_set(map, key, value);
				}
			}
			fclose(file);
			
			int resort = 0;
			for (int i=0; i<entries->count; i++) {
				Entry* entry = entries->items[i];
				char* filename = strrchr(entry->path, '/')+1;
				char* alias = Hash_get(map, filename);
				if (alias) {
					free(entry->name);
					entry->name = strdup(alias);
					resort = 1;
				}
			} 
			if (resort) EntryArray_sort(entries);
			Hash_free(map);
		}
	}
	
	if (hasCollections()) {
		if (entries->count) Array_push(root, Entry_new(COLLECTIONS_PATH, ENTRY_DIR));
		else { // no visible systems, promote collections to root
			dh = opendir(COLLECTIONS_PATH);
			if (dh!=NULL) {
				struct dirent *dp;
				char* tmp;
				char full_path[256];
				sprintf(full_path, "%s/", COLLECTIONS_PATH);
				tmp = full_path + strlen(full_path);
				Array* collections = Array_new();
				while((dp = readdir(dh)) != NULL) {
					if (hide(dp->d_name)) continue;
					strcpy(tmp, dp->d_name);
					Array_push(collections, Entry_new(full_path, ENTRY_DIR)); // yes, collections are fake directories
				}
				EntryArray_sort(collections);
				for (int i=0; i<collections->count; i++) {
					Array_push(entries, collections->items[i]);
				}
				Array_free(collections); // just free the array part, entries now owns collections entries
				closedir(dh);
			}
		}
	}
	
	// add systems to root
	for (int i=0; i<entries->count; i++) {
		Array_push(root, entries->items[i]);
	}
	Array_free(entries); // root now owns entries' entries
	
	char* tools_path = SDCARD_PATH "/Tools/" PLATFORM;
	if (exists(tools_path) && !simple_mode) Array_push(root, Entry_new(tools_path, ENTRY_DIR));
	
	return root;
}
static Array* getRecents(void) {
	Array* entries = Array_new();
	for (int i=0; i<recents->count; i++) {
		Recent* recent = recents->items[i];
		if (!recent->available) continue;
		
		char sd_path[256];
		sprintf(sd_path, "%s%s", SDCARD_PATH, recent->path);
		int type = suffixMatch(".pak", sd_path) ? ENTRY_PAK : ENTRY_ROM; // ???
		Entry* entry = Entry_new(sd_path, type);
		if (recent->alias) {
			free(entry->name);
			entry->name = strdup(recent->alias);
		}
		Array_push(entries, entry);
	}
	return entries;
}
static Array* getCollection(char* path) {
	Array* entries = Array_new();
	FILE* file = fopen(path, "r");
	if (file) {
		char line[256];
		while (fgets(line,256,file)!=NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line)==0) continue; // skip empty lines
			
			char sd_path[256];
			sprintf(sd_path, "%s%s", SDCARD_PATH, line);
			if (exists(sd_path)) {
				int type = suffixMatch(".pak", sd_path) ? ENTRY_PAK : ENTRY_ROM; // ???
				Array_push(entries, Entry_new(sd_path, type));
				
				// char emu_name[256];
				// getEmuName(sd_path, emu_name);
				// if (hasEmu(emu_name)) {
					// Array_push(entries, Entry_new(sd_path, ENTRY_ROM));
				// }
			}
		}
		fclose(file);
	}
	return entries;
}
static Array* getDiscs(char* path){
	
	// TODO: does path have SDCARD_PATH prefix?
	
	Array* entries = Array_new();
	
	char base_path[256];
	strcpy(base_path, path);
	char* tmp = strrchr(base_path, '/') + 1;
	tmp[0] = '\0';
	
	// TODO: limit number of discs supported (to 9?)
	FILE* file = fopen(path, "r");
	if (file) {
		char line[256];
		int disc = 0;
		while (fgets(line,256,file)!=NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line)==0) continue; // skip empty lines
			
			char disc_path[256];
			sprintf(disc_path, "%s%s", base_path, line);
						
			if (exists(disc_path)) {
				disc += 1;
				Entry* entry = Entry_new(disc_path, ENTRY_ROM);
				free(entry->name);
				char name[16];
				sprintf(name, "Disc %i", disc);
				entry->name = strdup(name);
				Array_push(entries, entry);
			}
		}
		fclose(file);
	}
	return entries;
}
static int getFirstDisc(char* m3u_path, char* disc_path) { // based on getDiscs() natch
	int found = 0;

	char base_path[256];
	strcpy(base_path, m3u_path);
	char* tmp = strrchr(base_path, '/') + 1;
	tmp[0] = '\0';
	
	FILE* file = fopen(m3u_path, "r");
	if (file) {
		char line[256];
		while (fgets(line,256,file)!=NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line)==0) continue; // skip empty lines
			
			sprintf(disc_path, "%s%s", base_path, line);
						
			if (exists(disc_path)) found = 1;
			break;
		}
		fclose(file);
	}
	return found;
}

static void addEntries(Array* entries, char* path) {
	DIR *dh = opendir(path);
	if (dh!=NULL) {
		struct dirent *dp;
		char* tmp;
		char full_path[256];
		sprintf(full_path, "%s/", path);
		tmp = full_path + strlen(full_path);
		while((dp = readdir(dh)) != NULL) {
			if (hide(dp->d_name)) continue;
			strcpy(tmp, dp->d_name);
			int is_dir = dp->d_type==DT_DIR;
			int type;
			if (is_dir) {
				// TODO: this should make sure launch.sh exists
				if (suffixMatch(".pak", dp->d_name)) {
					type = ENTRY_PAK;
				}
				else {
					type = ENTRY_DIR;
				}
			}
			else {
				if (prefixMatch(COLLECTIONS_PATH, full_path)) {
					type = ENTRY_DIR; // :shrug:
				}
				else {
					type = ENTRY_ROM;
				}
			}
			Array_push(entries, Entry_new(full_path, type));
		}
		closedir(dh);
	}
}

static int isConsoleDir(char* path) {
	char* tmp;
	char parent_dir[256];
	strcpy(parent_dir, path);
	tmp = strrchr(parent_dir, '/');
	tmp[0] = '\0';
	
	return exactMatch(parent_dir, ROMS_PATH);
}

static Array* getEntries(char* path){
	Array* entries = Array_new();

	if (isConsoleDir(path)) { // top-level console folder, might collate
		char collated_path[256];
		strcpy(collated_path, path);
		char* tmp = strrchr(collated_path, '(');
		// 1 because we want to keep the opening parenthesis to avoid collating "Game Boy Color" and "Game Boy Advance" into "Game Boy"
		// but conditional so we can continue to support a bare tag name as a folder name
		if (tmp) tmp[1] = '\0'; 
		
		DIR *dh = opendir(ROMS_PATH);
		if (dh!=NULL) {
			struct dirent *dp;
			char full_path[256];
			sprintf(full_path, "%s/", ROMS_PATH);
			tmp = full_path + strlen(full_path);
			// while loop so we can collate paths, see above
			while((dp = readdir(dh)) != NULL) {
				if (hide(dp->d_name)) continue;
				if (dp->d_type!=DT_DIR) continue;
				strcpy(tmp, dp->d_name);
			
				if (!prefixMatch(collated_path, full_path)) continue;
				addEntries(entries, full_path);
			}
			closedir(dh);
		}
	}
	else addEntries(entries, path); // just a subfolder
	
	EntryArray_sort(entries);
	return entries;
}

///////////////////////////////////////

static void queueNext(char* cmd) {
	LOG_info("cmd: %s\n", cmd);
	putFile("/tmp/next", cmd);
	quit = 1;
}

// based on https://stackoverflow.com/a/31775567/145965
static int replaceString(char *line, const char *search, const char *replace) {
   char *sp; // start of pattern
   if ((sp = strstr(line, search)) == NULL) {
      return 0;
   }
   int count = 1;
   int sLen = strlen(search);
   int rLen = strlen(replace);
   if (sLen > rLen) {
      // move from right to left
      char *src = sp + sLen;
      char *dst = sp + rLen;
      while((*dst = *src) != '\0') { dst++; src++; }
   } else if (sLen < rLen) {
      // move from left to right
      int tLen = strlen(sp) - sLen;
      char *stop = sp + rLen;
      char *src = sp + sLen + tLen;
      char *dst = sp + rLen + tLen;
      while(dst >= stop) { *dst = *src; dst--; src--; }
   }
   memcpy(sp, replace, rLen);
   count += replaceString(sp + rLen, search, replace);
   return count;
}
static char* escapeSingleQuotes(char* str) {
	// why not call replaceString directly?
	// call points require the modified string be returned
	// but replaceString is recursive and depends on its
	// own return value (but does it need to?)
	replaceString(str, "'", "'\\''");
	return str;
}

///////////////////////////////////////

// where minarch notes a game's last used save slot, returns whether there is one
static int getSlotPath(char* rom_path, int type, char* slot_path) {
	char* tmp;
	slot_path[0] = '\0';
	char path[256];
	strcpy(path, rom_path);
	
	if (!prefixMatch(ROMS_PATH, path)) return 0;
	
	char auto_path[256];
	if (type==ENTRY_DIR) {
		if (!hasCue(path, auto_path)) { // no cue?
			tmp = strrchr(auto_path, '.') + 1; // extension
			strcpy(tmp, "m3u"); // replace with m3u
			if (!exists(auto_path)) return 0; // no m3u
		}
		strcpy(path, auto_path); // cue or m3u if one exists
	}
	
	if (!suffixMatch(".m3u", path)) {
		char m3u_path[256];
		if (hasM3u(path, m3u_path)) {
			// change path to m3u path
			strcpy(path, m3u_path);
		}
	}
	
	char emu_name[256];
	getEmuName(path, emu_name);
	
	char rom_file[256];
	tmp = strrchr(path, '/') + 1;
	strcpy(rom_file, tmp);
	
	sprintf(slot_path, "%s/.minui/%s/%s.txt", SHARED_USERDATA_PATH, emu_name, rom_file); // /.userdata/.minui/<EMU>/<romname>.ext.txt
	
	return exists(slot_path);
}
static void readyResumePath(char* rom_path, int type) {
	can_resume = getSlotPath(rom_path, type, slot_path);
}
static void readyResume(Entry* entry) {
	readyResumePath(entry->path, entry->type);
}

static void saveLast(char* path);
static void loadLast(void);

static int getGameCore(char* sd_path, char* tag);
static int autoResume(void) {
	// NOTE: bypasses recents

	if (!exists(AUTO_RESUME_PATH)) return 0;
	
	char path[256];
	getFile(AUTO_RESUME_PATH, path, 256);
	unlink(AUTO_RESUME_PATH);
	sync();
	
	// make sure rom still exists
	char sd_path[256];
	sprintf(sd_path, "%s%s", SDCARD_PATH, path);
	if (!exists(sd_path)) return 0;
	
	// make sure emu still exists
	char emu_name[256];
	getEmuName(sd_path, emu_name);
	
	char emu_path[256];
	char core_tag[64];
	getEmuPath(getGameCore(sd_path, core_tag) ? core_tag : emu_name, emu_path);
	
	if (!exists(emu_path)) return 0;
	
	// putFile(LAST_PATH, FAUX_RECENT_PATH); // saveLast() will crash here because top is NULL
	
	char cmd[256];
	sprintf(cmd, "'%s' '%s'", escapeSingleQuotes(emu_path), escapeSingleQuotes(sd_path));
	putInt(RESUME_SLOT_PATH, AUTO_RESUME_SLOT);
	queueNext(cmd);
	return 1;
}

static void openPak(char* path) {
	// NOTE: escapeSingleQuotes() modifies the passed string 
	// so we need to save the path before we call that
	if (prefixMatch(ROMS_PATH, path)) {
		addRecent(path, NULL);
	}
	saveLast(path);
	
	char cmd[256];
	sprintf(cmd, "'%s/launch.sh'", escapeSingleQuotes(path));
	queueNext(cmd);
}
// per-game cores: a pak with an alternate.txt naming a system's tag (eg. SFC10.pak/alternate.txt: "SFC") can run
// that system's games too, and GAME_CORES_PATH remembers which games use one instead of their folder's own pak

// the game's chosen pak tag, if it has one (and that pak is still there)
static int getGameCore(char* sd_path, char* tag) {
	char rel[MAX_PATH];
	snprintf(rel, sizeof(rel), "%s", sd_path + (prefixMatch(SDCARD_PATH, sd_path) ? strlen(SDCARD_PATH) : 0));
	char m3u_path[MAX_PATH];
	char m3u_rel[MAX_PATH] = "";
	if (!suffixMatch(".m3u", rel) && hasM3u(sd_path, m3u_path)) snprintf(m3u_rel, sizeof(m3u_rel), "%s", m3u_path + strlen(SDCARD_PATH)); // a disc of a multi-disc game

	FILE* file = fopen(GAME_CORES_PATH, "r");
	if (!file) return 0;
	int found = 0;
	char line[MAX_PATH+64];
	while (!found && fgets(line, sizeof(line), file)) {
		normalizeNewline(line);
		trimTrailingNewlines(line);
		char* tab = strrchr(line, '\t');
		if (!tab) continue;
		*tab = '\0';
		if (!exactMatch(line, rel) && !(m3u_rel[0] && exactMatch(line, m3u_rel))) continue;
		char pak[MAX_PATH];
		getEmuPath(tab+1, pak);
		if (exists(pak)) {
			strcpy(tag, tab+1);
			found = 1;
		}
	}
	fclose(file);
	return found;
}
// tag NULL goes back to the system's own core
static void setGameCore(char* rel_path, char* tag) {
	static char out[32*1024];
	out[0] = '\0';
	FILE* file = fopen(GAME_CORES_PATH, "r");
	if (file) {
		char line[MAX_PATH+64];
		while (fgets(line, sizeof(line), file)) {
			char copy[MAX_PATH+64];
			strcpy(copy, line);
			normalizeNewline(copy);
			trimTrailingNewlines(copy);
			char* tab = strrchr(copy, '\t');
			if (!tab) continue;
			*tab = '\0';
			if (exactMatch(copy, rel_path)) continue; // replaced below
			*tab = '\t';
			if (strlen(out)+strlen(copy)+2<sizeof(out)) { strcat(out, copy); strcat(out, "\n"); }
		}
		fclose(file);
	}
	if (tag) {
		char line[MAX_PATH+64];
		snprintf(line, sizeof(line), "%s\t%s\n", rel_path, tag);
		if (strlen(out)+strlen(line)<sizeof(out)) strcat(out, line);
	}
	putFile(GAME_CORES_PATH, out);
	sync();
}

typedef struct Core {
	char tag[64]; // the pak's
	char name[64]; // shown
	const char* about; // a line or two on what it's good at, for the known ones
} Core;
#define CORE_MAX 8
// the core a pak's launch.sh runs, by a friendlier name when it's a known one
static void getCoreName(char* tag, char* name, const char** about) {
	snprintf(name, 64, "%s", tag);
	*about = NULL;
	char pak[MAX_PATH];
	getEmuPath(tag, pak);
	FILE* file = fopen(pak, "r");
	if (!file) return;
	char line[256];
	char exe[64] = "";
	while (fgets(line, sizeof(line), file)) {
		if (!prefixMatch("EMU_EXE=", line)) continue;
		trimTrailingNewlines(line);
		snprintf(exe, sizeof(exe), "%s", line + 8);
		break;
	}
	fclose(file);
	if (!exe[0]) return;
	static const char* known[][3] = {
		{"snes9x2005_plus","Snes9x 2005 Plus",	"The fastest. Runs nearly every game at full speed,\nwith a few accuracy fixes over the original 2005."},
		{"snes9x2005","Snes9x 2005",			"Very fast, made for low-powered handhelds."},
		{"snes9x2010","Snes9x 2010",			"More accurate than 2005, a little slower.\nA good pick when a game glitches on the default."},
		{"snes9x","Snes9x",						"The latest Snes9x: the most accurate and compatible,\nand the most demanding."},
		{"mednafen_supafaust","Supafaust",		"Accurate, Mednafen-based, and uses both CPU cores.\nGood for games with special chips."},
		{"gpsp","gpSP",							"The fastest, made for low-powered handhelds.\nSome games and rom hacks have glitches."},
		{"mgba","mGBA",							"Accurate, with the best compatibility (rom hacks too).\nSlower than gpSP."},
		{"gambatte","Gambatte",					"Accurate Game Boy and Game Boy Color emulation."},
		{"fceumm","FCEUmm",						"Fast NES emulation that runs almost every cartridge."},
		{"picodrive","PicoDrive",				"Fast Genesis, Master System and Game Gear emulation."},
		{"pcsx_rearmed","PCSX ReARMed",			"PlayStation emulation tuned for ARM handhelds."},
		{"mednafen_pce_fast","Beetle PCE Fast",	"Fast, accurate PC Engine / TurboGrafx-16 emulation."},
	};
	for (int i=0; i<(int)(sizeof(known)/sizeof(known[0])); i++) {
		if (exactMatch((char*)known[i][0], exe)) {
			snprintf(name, 64, "%s", known[i][1]);
			*about = known[i][2];
			return;
		}
	}
	snprintf(name, 64, "%s", exe);
}
// paks in dir whose alternate.txt names system_tag
static void findAlternates(char* dir, char* system_tag, Core* cores, int* count) {
	DIR* d = opendir(dir);
	if (!d) return;
	struct dirent* e;
	while ((e = readdir(d)) && *count<CORE_MAX) {
		if (!suffixMatch(".pak", e->d_name)) continue;
		char alt_path[MAX_PATH];
		snprintf(alt_path, sizeof(alt_path), "%s/%s/alternate.txt", dir, e->d_name);
		FILE* file = fopen(alt_path, "r");
		if (!file) continue;
		char line[64];
		int match = 0;
		while (!match && fgets(line, sizeof(line), file)) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			match = exactMatch(line, system_tag);
		}
		fclose(file);
		if (!match) continue;
		char tag[64];
		snprintf(tag, sizeof(tag), "%.*s", (int)(strlen(e->d_name)-4), e->d_name);
		int seen = 0;
		for (int i=0; i<*count; i++) if (exactMatch(cores[i].tag, tag)) seen = 1;
		if (seen) continue;
		strcpy(cores[*count].tag, tag);
		getCoreName(tag, cores[*count].name, &cores[*count].about);
		*count += 1;
	}
	closedir(d);
}
static int compareCores(const void* a, const void* b) {
	return strcasecmp(((Core*)a)->name, ((Core*)b)->name);
}
// every core that can run a system's games, its own first
static int getCores(char* system_tag, Core* cores) {
	int count = 0;
	char pak[MAX_PATH];
	getEmuPath(system_tag, pak);
	if (exists(pak)) {
		strcpy(cores[count].tag, system_tag);
		getCoreName(system_tag, cores[count].name, &cores[count].about);
		count += 1;
	}
	char dir[MAX_PATH];
	snprintf(dir, sizeof(dir), "%s/Emus/%s", SDCARD_PATH, PLATFORM);
	findAlternates(dir, system_tag, cores, &count);
	snprintf(dir, sizeof(dir), "%s/Emus", PAKS_PATH);
	findAlternates(dir, system_tag, cores, &count);
	int first = exists(pak) ? 1 : 0; // the system's own stays on top, the rest by name
	if (count-first>1) qsort(cores+first, count-first, sizeof(Core), compareCores);
	return count;
}

static void openRom(char* path, char* last) {
	LOG_info("openRom(%s,%s)\n", path, last);
	
	char sd_path[256];
	strcpy(sd_path, path);
	
	char m3u_path[256];
	int has_m3u = hasM3u(sd_path, m3u_path);
	
	char recent_path[256];
	strcpy(recent_path, has_m3u ? m3u_path : sd_path);
	
	if (has_m3u && suffixMatch(".m3u", sd_path)) {
		getFirstDisc(m3u_path, sd_path);
	}

	char emu_name[256];
	getEmuName(sd_path, emu_name);

	if (should_resume) {
		char slot[16];
		if (resume_slot>=0) sprintf(slot, "%d", resume_slot); // the switcher picked a specific save
		else getFile(slot_path, slot, 16);
		resume_slot = -1;
		putFile(RESUME_SLOT_PATH, slot);
		should_resume = 0;

		if (has_m3u) {
			char rom_file[256];
			strcpy(rom_file, strrchr(m3u_path, '/') + 1);
			
			// get disc for state
			char disc_path_path[256];
			sprintf(disc_path_path, "%s/.minui/%s/%s.%s.txt", SHARED_USERDATA_PATH, emu_name, rom_file, slot); // /.userdata/arm-480/.minui/<EMU>/<romname>.ext.0.txt

			if (exists(disc_path_path)) {
				// switch to disc path
				char disc_path[256];
				getFile(disc_path_path, disc_path, 256);
				if (disc_path[0]=='/') strcpy(sd_path, disc_path); // absolute
				else { // relative
					strcpy(sd_path, m3u_path);
					char* tmp = strrchr(sd_path, '/') + 1;
					strcpy(tmp, disc_path);
				}
			}
		}
	}
	else putInt(RESUME_SLOT_PATH,8); // resume hidden default state
	
	char emu_path[256];
	char core_tag[64];
	getEmuPath(getGameCore(sd_path, core_tag) ? core_tag : emu_name, emu_path); // the game's own choice of core, if it has one
	
	// NOTE: escapeSingleQuotes() modifies the passed string 
	// so we need to save the path before we call that
	addRecent(recent_path, recent_alias); // yiiikes
	saveLast(last==NULL ? sd_path : last);
	
	char cmd[256];
	sprintf(cmd, "'%s' '%s'", escapeSingleQuotes(emu_path), escapeSingleQuotes(sd_path));
	queueNext(cmd);
}
static void openDirectory(char* path, int auto_launch) {
	char auto_path[256];
	if (hasCue(path, auto_path) && auto_launch) {
		openRom(auto_path, path);
		return;
	}

	char m3u_path[256];
	strcpy(m3u_path, auto_path);
	char* tmp = strrchr(m3u_path, '.') + 1; // extension
	strcpy(tmp, "m3u"); // replace with m3u
	if (exists(m3u_path) && auto_launch) {
		auto_path[0] = '\0';
		if (getFirstDisc(m3u_path, auto_path)) {
			openRom(auto_path, path);
			return;
		}
		// TODO: doesn't handle empty m3u files
	}
	
	int selected = 0;
	int start = selected;
	int end = 0;
	if (top && top->entries->count>0) {
		if (restore_depth==stack->count && top->selected==restore_relative) {
			selected = restore_selected;
			start = restore_start;
			end = restore_end;
		}
	}
	
	top = Directory_new(path, selected);
	top->start = start;
	top->end = end ? end : ((top->entries->count<MAIN_ROW_COUNT) ? top->entries->count : MAIN_ROW_COUNT);

	Array_push(stack, top);
}
static void closeDirectory(void) {
	restore_selected = top->selected;
	restore_start = top->start;
	restore_end = top->end;
	DirectoryArray_pop(stack);
	restore_depth = stack->count;
	top = stack->items[stack->count-1];
	restore_relative = top->selected;
}

static void Entry_open(Entry* self) {
	recent_alias = self->name;  // yiiikes
	if (self->type==ENTRY_ROM) {
		char *last = NULL;
		if (prefixMatch(COLLECTIONS_PATH, top->path) || exactMatch(FAUX_FAVORITES_PATH, top->path) || exactMatch(FAUX_MOSTPLAYED_PATH, top->path)) {
			char* tmp;
			char filename[256];
			
			tmp = strrchr(self->path, '/');
			if (tmp) strcpy(filename, tmp+1);
			
			char last_path[256];
			sprintf(last_path, "%s/%s", top->path, filename);
			last = last_path;
		}
		openRom(self->path, last);
	}
	else if (self->type==ENTRY_PAK) {
		openPak(self->path);
	}
	else if (self->type==ENTRY_DIR) {
		openDirectory(self->path, 1);
	}
}

///////////////////////////////////////

static void saveLast(char* path) {
	// special case for recently played
	if (exactMatch(top->path, FAUX_RECENT_PATH)) {
		// NOTE: that we don't have to save the file because
		// your most recently played game will always be at
		// the top which is also the default selection
		path = FAUX_RECENT_PATH;
	}
	putFile(LAST_PATH, path);
	putFile(LAST_BACKUP_PATH, path);
}
static void loadLast(void) { // call after loading root directory
	char* from = exists(LAST_PATH) ? LAST_PATH : (exists(LAST_BACKUP_PATH) ? LAST_BACKUP_PATH : NULL);
	if (!from) {
		LOG_info("last place: none saved\n");
		return;
	}

	char last_path[256];
	getFile(from, last_path, 256);
	LOG_info("last place: %s%s\n", last_path, from==LAST_BACKUP_PATH ? " (from the card copy: /tmp had lost it)" : "");
	
	if (prefixMatch(FAUX_MOSTPLAYED_PATH "/", last_path)) { // not in the root, so reopen it directly
		openDirectory(FAUX_MOSTPLAYED_PATH, 0);
		most_played_from_info = 1;
		char* filename = strrchr(last_path, '/');
		for (int i=0; i<top->entries->count; i++) {
			Entry* entry = top->entries->items[i];
			if (suffixMatch(filename, entry->path)) {
				top->selected = i; // rankings may have changed after playing
				break;
			}
		}
		Directory_reindex(top);
		return;
	}
	
	char full_path[256];
	strcpy(full_path, last_path);
	
	char* tmp;
	char filename[256];
	tmp = strrchr(last_path, '/');
	if (tmp) strcpy(filename, tmp);
	
	Array* last = Array_new();
	while (!exactMatch(last_path, SDCARD_PATH)) {
		Array_push(last, strdup(last_path));
		
		char* slash = strrchr(last_path, '/');
		last_path[(slash-last_path)] = '\0';
	}
	
	while (last->count>0) {
		char* path = Array_pop(last);
		if (!exactMatch(path, ROMS_PATH)) { // romsDir is effectively root as far as restoring state after a game
			char collated_path[256];
			collated_path[0] = '\0';
			if (suffixMatch(")", path) && isConsoleDir(path)) {
				strcpy(collated_path, path);
				tmp = strrchr(collated_path, '(');
				if (tmp) tmp[1] = '\0'; // 1 because we want to keep the opening parenthesis to avoid collating "Game Boy Color" and "Game Boy Advance" into "Game Boy"
			}
			
			for (int i=0; i<top->entries->count; i++) {
				Entry* entry = top->entries->items[i];
			
				// NOTE: strlen() is required for collated_path, '\0' wasn't reading as NULL for some reason
				if (exactMatch(entry->path, path) || (strlen(collated_path) && prefixMatch(collated_path, entry->path)) || ((prefixMatch(COLLECTIONS_PATH, full_path) || prefixMatch(FAUX_FAVORITES_PATH "/", full_path)) && suffixMatch(filename, entry->path))) {
					top->selected = i;
					if (i>=top->end) {
						top->start = i;
						top->end = top->start + MAIN_ROW_COUNT;
						if (top->end>top->entries->count) {
							top->end = top->entries->count;
							top->start = top->end - MAIN_ROW_COUNT;
						}
					}
					if (last->count==0 && !exactMatch(entry->path, FAUX_RECENT_PATH) && !(!exactMatch(entry->path, COLLECTIONS_PATH) && prefixMatch(COLLECTIONS_PATH, entry->path))) break; // don't show contents of auto-launch dirs
				
					if (entry->type==ENTRY_DIR) {
						openDirectory(entry->path, 0);
						break;
					}
				}
			}
		}
		free(path); // we took ownership when we popped it
	}
	
	StringArray_free(last);
}

///////////////////////////////////////

// multi-disc fix: games split into "Name (Disc 1)", "Name (Disc 2)"... get a folder and an m3u so they show
// (and save) as one game. Runs at startup, only touching folders that need it.

#define DISC_MAX 9
static const char* disc_exts[] = { ".cue", ".ccd", ".chd", ".pbp", ".iso", ".img", ".bin", NULL }; // what an m3u lists, best first

// finds "(Disc N)", "(Disk N)" or "(CD N)" (any case, "of M" allowed): copies the name before it into base, returns N or 0
static int getDiscNumber(char* name, char* base) {
	char* lower = strdup(name);
	for (char* c=lower; *c; c++) *c = tolower(*c);
	int number = 0;
	char* at = NULL;
	char* starts[] = { "(disc", "(disk", "(cd", NULL };
	for (int i=0; starts[i] && !number; i++) {
		char* hit = lower;
		while ((hit = strstr(hit, starts[i]))) {
			char* digits = hit + strlen(starts[i]);
			while (*digits==' ') digits++;
			if (*digits>='1' && *digits<='9') {
				number = atoi(digits);
				at = hit;
				break;
			}
			hit += 1;
		}
	}
	if (number>0 && number<=DISC_MAX && at>lower) {
		int len = at - lower;
		strncpy(base, name, len);
		base[len] = '\0';
		while (len>0 && base[len-1]==' ') base[--len] = '\0';
	}
	else number = 0;
	free(lower);
	return number;
}
static int discExtRank(char* name) { // lower is better, -1 when it's not a disc image
	for (int i=0; disc_exts[i]; i++) if (suffixMatch((char*)disc_exts[i], name)) return i;
	return -1;
}
static void copyFileIfMissing(char* from, char* to) {
	if (!exists(from) || exists(to)) return;
	FILE* in = fopen(from, "rb");
	if (!in) return;
	FILE* out = fopen(to, "wb");
	if (out) {
		char buffer[16384];
		size_t n;
		while ((n = fread(buffer, 1, sizeof(buffer), in))>0) fwrite(buffer, 1, n, out);
		fclose(out);
	}
	fclose(in);
}
static time_t modifiedAt(char* path) {
	struct stat st;
	return stat(path, &st)==0 ? st.st_mtime : 0;
}
// replaces lines starting with old_path (followed by a tab or the end) with new_path in a list file
// when new_path is already listed the old line is just dropped, so several discs of one game become one entry
static void replaceListPath(char* list_path, char* old_path, char* new_path) {
	static char before[64*1024];
	FILE* file = fopen(list_path, "r");
	if (!file) return;
	size_t size = fread(before, 1, sizeof(before)-1, file);
	before[size] = '\0';
	fclose(file);

	int len = strlen(old_path);
	char listed_mid[MAX_PATH+2];
	sprintf(listed_mid, "\n%s", new_path);
	int listed = prefixMatch(new_path, before) || strstr(before, listed_mid)!=NULL;

	static char out[64*1024];
	out[0] = '\0';
	int changed = 0;
	char* line = before;
	while (*line) {
		char* end = strchr(line, '\n');
		int line_len = end ? end - line + 1 : (int)strlen(line);
		char text[MAX_PATH*2];
		snprintf(text, sizeof(text), "%.*s", line_len, line);
		line += line_len;
		if (!strncmp(text, old_path, len) && (text[len]=='\t' || text[len]=='\n' || text[len]=='\0')) {
			changed = 1;
			if (listed) continue;
			listed = 1;
			char fixed[MAX_PATH*2];
			snprintf(fixed, sizeof(fixed), "%s%s", new_path, text+len);
			strcpy(text, fixed);
		}
		if (strlen(out)+strlen(text)<sizeof(out)) strcat(out, text);
	}
	if (changed) putFile(list_path, out);
}

// copies the progress kept under each disc's name to the m3u's, without touching the originals
// discs are filenames inside dir, m3u_name is the m3u's filename
static void moveDiscSaves(char* dir, char discs[DISC_MAX][MAX_PATH], int count, char* m3u_name) {
	char probe[MAX_PATH];
	sprintf(probe, "%s/%s", dir, discs[0]);
	char tag[MAX_PATH];
	getEmuName(probe, tag);

	// memory card (and clock): the newest one, that's where the player got to
	char* exts[] = { ".sav", ".rtc", NULL };
	for (int e=0; exts[e]; e++) {
		char newest[MAX_PATH] = "";
		time_t newest_at = 0;
		for (int d=0; d<count; d++) {
			char path[MAX_PATH];
			sprintf(path, "%s/Saves/%s/%s%s", SDCARD_PATH, tag, discs[d], exts[e]);
			time_t at = modifiedAt(path);
			if (at>newest_at) { newest_at = at; strcpy(newest, path); }
		}
		if (newest[0]) {
			char to[MAX_PATH];
			sprintf(to, "%s/Saves/%s/%s%s", SDCARD_PATH, tag, m3u_name, exts[e]);
			copyFileIfMissing(newest, to);
		}
	}

	// save states (in every core's folder for this system) with their screenshots, each noting which disc it was on
	DIR* dh = opendir(SHARED_USERDATA_PATH);
	if (!dh) return;
	char prefix[MAX_PATH];
	sprintf(prefix, "%s-", tag);
	struct dirent* dp;
	while ((dp = readdir(dh))) {
		if (!prefixMatch(prefix, dp->d_name)) continue;
		for (int slot=0; slot<=AUTO_RESUME_SLOT; slot++) {
			char to[MAX_PATH];
			sprintf(to, "%s/%s/%s.st%i", SHARED_USERDATA_PATH, dp->d_name, m3u_name, slot);
			if (exists(to)) continue;
			int best = -1;
			time_t best_at = 0;
			for (int d=0; d<count; d++) {
				char from[MAX_PATH];
				sprintf(from, "%s/%s/%s.st%i", SHARED_USERDATA_PATH, dp->d_name, discs[d], slot);
				time_t at = modifiedAt(from);
				if (at>best_at) { best_at = at; best = d; }
			}
			if (best==-1) continue;
			char from[MAX_PATH];
			sprintf(from, "%s/%s/%s.st%i", SHARED_USERDATA_PATH, dp->d_name, discs[best], slot);
			copyFileIfMissing(from, to);

			char minui_dir[MAX_PATH];
			sprintf(minui_dir, "%s/.minui/%s", SHARED_USERDATA_PATH, tag);
			char bmp_from[MAX_PATH], bmp_to[MAX_PATH], disc_txt[MAX_PATH];
			sprintf(bmp_from, "%s/%s.%i.bmp", minui_dir, discs[best], slot);
			sprintf(bmp_to, "%s/%s.%i.bmp", minui_dir, m3u_name, slot);
			copyFileIfMissing(bmp_from, bmp_to);
			sprintf(disc_txt, "%s/%s.%i.txt", minui_dir, m3u_name, slot);
			if (!exists(disc_txt)) {
				mkdir(minui_dir, 0755);
				putFile(disc_txt, discs[best]); // loading this state starts the right disc
			}
		}
	}
	closedir(dh);
}

// writes folder/<folder name>.m3u listing the best image of each disc (names relative to folder)
static void writeDiscPlaylist(char* folder, char discs[DISC_MAX][MAX_PATH], int count) {
	char* name = strrchr(folder, '/') + 1;
	char m3u_path[MAX_PATH];
	sprintf(m3u_path, "%s/%s.m3u", folder, name);
	char text[DISC_MAX*MAX_PATH] = "";
	for (int d=0; d<count; d++) {
		strcat(text, discs[d]);
		strcat(text, "\n");
	}
	putFile(m3u_path, text);
}

// groups a folder's disc images by game: discs[game][disc number-1] = filename, returns how many games have 2+ discs
typedef struct DiscGroup {
	char base[MAX_PATH];
	char discs[DISC_MAX][MAX_PATH]; // best image per disc number, "" when missing
	int ranks[DISC_MAX];
	int count; // discs found
} DiscGroup;
#define DISC_GROUP_MAX 64
static int findDiscGroups(char* dir, DiscGroup* groups) {
	int group_count = 0;
	DIR* dh = opendir(dir);
	if (!dh) return 0;
	struct dirent* dp;
	while ((dp = readdir(dh))) {
		if (dp->d_name[0]=='.' || dp->d_type==DT_DIR) continue;
		int rank = discExtRank(dp->d_name);
		if (rank<0) continue;
		char base[MAX_PATH];
		int number = getDiscNumber(dp->d_name, base);
		if (!number) continue;
		DiscGroup* group = NULL;
		for (int g=0; g<group_count; g++) if (exactMatch(groups[g].base, base)) group = &groups[g];
		if (!group) {
			if (group_count==DISC_GROUP_MAX) continue;
			group = &groups[group_count++];
			memset(group, 0, sizeof(DiscGroup));
			strcpy(group->base, base);
		}
		int i = number - 1;
		if (!group->discs[i][0] || rank<group->ranks[i]) {
			if (!group->discs[i][0]) group->count += 1;
			strcpy(group->discs[i], dp->d_name);
			group->ranks[i] = rank;
		}
	}
	closedir(dh);
	return group_count;
}
// the discs in order with gaps closed up, returns how many
static int orderedDiscs(DiscGroup* group, char discs[DISC_MAX][MAX_PATH]) {
	int count = 0;
	for (int i=0; i<DISC_MAX; i++) if (group->discs[i][0]) strcpy(discs[count++], group->discs[i]);
	return count;
}

static void fixDiscFolder(char* system_dir) {
	DiscGroup* groups = calloc(DISC_GROUP_MAX, sizeof(DiscGroup));

	// loose discs: into a folder of their own, with every file that belongs to them (eg. a cue's bin tracks)
	int group_count = findDiscGroups(system_dir, groups);
	for (int g=0; g<group_count; g++) {
		DiscGroup* group = &groups[g];
		if (group->count<2) continue;
		char folder[MAX_PATH];
		sprintf(folder, "%s/%s", system_dir, group->base);
		char m3u_check[MAX_PATH];
		sprintf(m3u_check, "%s/%s.m3u", folder, group->base);
		if (exists(m3u_check)) continue; // already set up, leave the loose copies alone
		mkdir(folder, 0755);

		char discs[DISC_MAX][MAX_PATH];
		int count = orderedDiscs(group, discs);
		DIR* dh = opendir(system_dir);
		if (!dh) continue;
		struct dirent* dp;
		while ((dp = readdir(dh))) {
			if (dp->d_name[0]=='.' || dp->d_type==DT_DIR) continue;
			char base[MAX_PATH];
			if (!getDiscNumber(dp->d_name, base) || !exactMatch(base, group->base)) continue;
			char from[MAX_PATH], to[MAX_PATH];
			sprintf(from, "%s/%s", system_dir, dp->d_name);
			sprintf(to, "%s/%s", folder, dp->d_name);
			rename(from, to);
			// lists that pointed at a moved disc follow it to the m3u
			char m3u_rel[MAX_PATH];
			sprintf(m3u_rel, "%s/%s.m3u", folder + strlen(SDCARD_PATH), group->base);
			char* old_rel = from + strlen(SDCARD_PATH);
			replaceListPath(RECENT_PATH, old_rel, m3u_rel);
			replaceListPath(FAVORITES_PATH, old_rel, m3u_rel);
			replaceListPath(SWITCHER_PATH, old_rel, m3u_rel);
		}
		closedir(dh);
		writeDiscPlaylist(folder, discs, count);
		char m3u_name[MAX_PATH];
		sprintf(m3u_name, "%s.m3u", group->base);
		moveDiscSaves(folder, discs, count, m3u_name);
		LOG_info("multi-disc: grouped %i discs into %s\n", count, folder);
	}

	// disc folders without an m3u
	DIR* dh = opendir(system_dir);
	if (dh) {
		struct dirent* dp;
		while ((dp = readdir(dh))) {
			if (dp->d_name[0]=='.' || dp->d_type!=DT_DIR || hide(dp->d_name)) continue;
			char folder[MAX_PATH];
			sprintf(folder, "%s/%s", system_dir, dp->d_name);
			char m3u_check[MAX_PATH];
			sprintf(m3u_check, "%s/%s.m3u", folder, dp->d_name);
			if (exists(m3u_check)) continue;
			int count_here = findDiscGroups(folder, groups);
			if (count_here!=1 || groups[0].count<2) continue; // one game's discs, nothing ambiguous
			char discs[DISC_MAX][MAX_PATH];
			int count = orderedDiscs(&groups[0], discs);
			writeDiscPlaylist(folder, discs, count);
			char m3u_name[MAX_PATH];
			sprintf(m3u_name, "%s.m3u", dp->d_name);
			moveDiscSaves(folder, discs, count, m3u_name);
			LOG_info("multi-disc: added %s\n", m3u_check);
		}
		closedir(dh);
	}
	free(groups);
}
#define DISC_CHECK_PATH "/tmp/minui_disc_check.txt" // the system folders' modified times at the last check this boot
static void getDiscStamp(char* stamp, size_t size) { // each system folder's name and modified time
	stamp[0] = '\0';
	DIR* dh = opendir(ROMS_PATH);
	if (!dh) return;
	struct dirent* dp;
	while ((dp = readdir(dh))) {
		if (dp->d_name[0]=='.' || dp->d_type!=DT_DIR) continue;
		char system_dir[MAX_PATH], line[MAX_PATH+32];
		sprintf(system_dir, "%s/%s", ROMS_PATH, dp->d_name);
		snprintf(line, sizeof(line), "%s %ld\n", dp->d_name, (long)modifiedAt(system_dir));
		if (strlen(stamp)+strlen(line)<size) strcat(stamp, line);
	}
	closedir(dh);
}
static void fixMultiDisc(void) {
	// minui restarts after every game: only look again when a system folder changed (a file added, moved or removed)
	char stamp[4096], last[4096] = "";
	getDiscStamp(stamp, sizeof(stamp));
	if (exists(DISC_CHECK_PATH)) getFile(DISC_CHECK_PATH, last, sizeof(last));
	if (exactMatch(stamp, last)) return;

	DIR* dh = opendir(ROMS_PATH);
	if (!dh) return;
	struct dirent* dp;
	while ((dp = readdir(dh))) {
		if (dp->d_name[0]=='.' || dp->d_type!=DT_DIR) continue;
		char system_dir[MAX_PATH];
		sprintf(system_dir, "%s/%s", ROMS_PATH, dp->d_name);
		fixDiscFolder(system_dir);
	}
	closedir(dh);
	sync();

	getDiscStamp(stamp, sizeof(stamp)); // after fixing, which may itself have changed folders
	putFile(DISC_CHECK_PATH, stamp);
}

///////////////////////////////////////

static void Menu_init(void) {
	fixMultiDisc(); // before anything reads folders or the lists it might update
	stack = Array_new(); // array of open Directories
	recents = Array_new();
	favorites = Array_new();
	loadFavorites();
	favorites_sort = getInt(FAVORITES_SORT_PATH);
	if (favorites_sort<0 || favorites_sort>=FAVORITES_SORT_COUNT) favorites_sort = FAVORITES_SORT_NAME;
	folder_sort = getInt(FOLDER_SORT_PATH);
	if (folder_sort<0 || folder_sort>=FOLDER_SORT_COUNT) folder_sort = FOLDER_SORT_AZ;
	view_mode = getInt(VIEW_MODE_PATH)==VIEW_CAROUSEL ? VIEW_CAROUSEL : VIEW_LIST;

	openDirectory(SDCARD_PATH, 0);
	loadLast(); // restore state when available
}
static void Menu_quit(void) {
	RecentArray_free(recents);
	RecentArray_free(favorites);
	DirectoryArray_free(stack);
}

///////////////////////////////////////

// the selected entry's art, decoded once and pre-flattened onto black in
// the screen's format so redraws are a straight copy instead of a PNG decode
// the list's art: the last few decoded (missing art too), so moving back and forth doesn't decode again
#define THUMB_CACHE_SIZE 8
static struct { char path[MAX_PATH]; SDL_Surface* art; uint32_t used; int loaded; } thumbs[THUMB_CACHE_SIZE];
static uint32_t thumb_clock = 0;
static void getResPath(Entry* entry, char* res_path) {
	char res_root[MAX_PATH];
	strcpy(res_root, entry->path);
	char* res_name = strrchr(entry->path, '/') + 1;
	char* tmp = strrchr(res_root, '/');
	tmp[0] = '\0';
	sprintf(res_path, "%s/.res/%s.png", res_root, res_name);
}
// decodes art flattened onto black in the screen's format, NULL when there's none
static SDL_Surface* loadArt(char* res_path, SDL_Surface* screen) {
	SDL_Surface* image = exists(res_path) ? IMG_Load(res_path) : NULL;
	if (!image) return NULL;
	// screen is cleared to black before art is drawn so this matches blending at draw time
	SDL_Surface* art = SDL_CreateRGBSurface(0, image->w, image->h, screen->format->BitsPerPixel, screen->format->Rmask, screen->format->Gmask, screen->format->Bmask, screen->format->Amask);
	if (art) {
		SDL_FillRect(art, NULL, 0);
		SDL_BlitSurface(image, NULL, art, NULL);
	}
	SDL_FreeSurface(image);
	return art;
}
static int Decoder_take(char* path, SDL_Surface** art, SDL_Surface** grey, int wait);
// the art, from the cache or the decoding thread; when it's neither and can_decode is off, NULL for now
static SDL_Surface* getThumb(Entry* entry, SDL_Surface* screen, int can_decode) {
	char res_path[MAX_PATH];
	getResPath(entry, res_path);

	int oldest = 0;
	for (int i=0; i<THUMB_CACHE_SIZE; i++) {
		if (thumbs[i].loaded && exactMatch(thumbs[i].path, res_path)) {
			thumbs[i].used = ++thumb_clock;
			return thumbs[i].art;
		}
		if (!thumbs[i].loaded || (thumbs[oldest].loaded && thumbs[i].used<thumbs[oldest].used)) oldest = i;
	}
	SDL_Surface* art = NULL;
	SDL_Surface* grey = NULL;
	int got = Decoder_take(res_path, &art, &grey, can_decode);
	if (grey) SDL_FreeSurface(grey);
	if (!got && !can_decode) return NULL;
	if (thumbs[oldest].art) SDL_FreeSurface(thumbs[oldest].art);
	strcpy(thumbs[oldest].path, res_path);
	thumbs[oldest].art = got ? art : loadArt(res_path, screen);
	thumbs[oldest].loaded = 1;
	thumbs[oldest].used = ++thumb_clock;
	return thumbs[oldest].art;
}
static int hasThumb(Entry* entry) { // decoded already (or known to be missing)
	char res_path[MAX_PATH];
	getResPath(entry, res_path);
	for (int i=0; i<THUMB_CACHE_SIZE; i++) if (thumbs[i].loaded && exactMatch(thumbs[i].path, res_path)) return 1;
	return 0;
}
// the list asks the decoding thread for the selected row's art and the rows either side (so holding a direction
// finds them ready), and takes in whatever it has finished; the selected one is taken last so it stays newest
#define THUMB_PREFETCH 3
static void Decoder_request(char* path, int priority, int grey_too, SDL_Surface* screen);
static void Decoder_beginFrame(void);
static void Decoder_endFrame(void);
static void requestThumbs(Directory* dir, SDL_Surface* screen) {
	int total = dir->entries->count;
	if (!total) return;
	Decoder_beginFrame();
	for (int place=0; place<=THUMB_PREFETCH; place++) {
		for (int side=1; side>=-1; side-=2) {
			if (!place && side<0) continue;
			int i = dir->selected + side * place;
			if (i<0 || i>=total) continue;
			Entry* entry = dir->entries->items[i];
			if (hasThumb(entry)) continue;
			char res_path[MAX_PATH];
			getResPath(entry, res_path);
			Decoder_request(res_path, place, 0, screen);
		}
	}
	Decoder_endFrame();
	for (int place=THUMB_PREFETCH; place>=0; place--) {
		for (int side=1; side>=-1; side-=2) {
			int i = dir->selected + side * place;
			if (i>=0 && i<total) getThumb(dir->entries->items[i], screen, 0);
		}
	}
}
static int preloadThumbs(Directory* dir, SDL_Surface* screen) {
	requestThumbs(dir, screen);
	return 0; // the thread does the work, so the loop can still wait for the next frame
}
static void freeThumbs(void) {
	for (int i=0; i<THUMB_CACHE_SIZE; i++) {
		if (thumbs[i].art) SDL_FreeSurface(thumbs[i].art);
		memset(&thumbs[i], 0, sizeof(thumbs[i]));
	}
}

///////////////////////////////////////

// carousel view: the selected game's art large in the middle, two smaller and darker on each side
#define CAROUSEL_LEVELS 3 // selected, next to it, outermost
static const int carousel_scale[CAROUSEL_LEVELS] = { 100, 60, 40 }; // percent of the selected art's size
static const int carousel_shade[CAROUSEL_LEVELS] = { 256, 170, 110 }; // brightness out of 256
static const int carousel_color[CAROUSEL_LEVELS] = { 256, 150, 80 }; // saturation out of 256, the rest goes grey
#define CAROUSEL_TUCK 35 // percent of each side art's width hidden behind its nearer neighbor
#define CAROUSEL_ART_WIDTH 160 // unscaled box the selected art is fit into
#define CAROUSEL_ART_HEIGHT 135
#define CAROUSEL_ART_BOTTOM 156 // unscaled baseline every art stands on, just above the name

#define CAROUSEL_CACHE_SIZE 16 // the five shown, the ones being preloaded either side, and a few behind
typedef struct {
	char path[MAX_PATH];
	SDL_Surface* art; // decoded, NULL when missing
	SDL_Surface* sized[CAROUSEL_LEVELS]; // software drawing
	GFX_Texture* color; // GPU drawing: the art as is, and greyed
	GFX_Texture* grey;
	GFX_Texture* label; // GPU drawing, no art: the name for its card
	int label_w, label_h;
	int fit_w, fit_h; // the art's size as the selected one
	int loaded;
	uint32_t used;
} CarouselArt;
static CarouselArt carousel_cache[CAROUSEL_CACHE_SIZE];
static uint32_t carousel_clock = 0;

static void CarouselArt_clear(CarouselArt* slot) {
	if (slot->art) SDL_FreeSurface(slot->art);
	for (int i=0; i<CAROUSEL_LEVELS; i++) {
		if (slot->sized[i]) SDL_FreeSurface(slot->sized[i]);
	}
	PLAT_destroyTexture(slot->color);
	PLAT_destroyTexture(slot->grey);
	PLAT_destroyTexture(slot->label);
	memset(slot, 0, sizeof(CarouselArt));
}
static void Carousel_quit(void) {
	for (int i=0; i<CAROUSEL_CACHE_SIZE; i++) CarouselArt_clear(&carousel_cache[i]);
}

// bilinear resize, then faded toward grey (color/256 saturation) and darkened (shade/256)
// handles 16-bit RGB565 and any 32-bit format (where only darkening applies)
static SDL_Surface* scaleArt(SDL_Surface* src, int dw, int dh, int shade, int color) {
	if (dw>1024) dw = 1024; // the column tables below
	SDL_Surface* dst = SDL_CreateRGBSurface(0, dw, dh, src->format->BitsPerPixel, src->format->Rmask, src->format->Gmask, src->format->Bmask, src->format->Amask);
	if (!dst) return NULL;
	int bpp = src->format->BytesPerPixel;
	// each column's two source columns and blend, worked out once (divisions are slow on the device's CPU)
	static int col0[1024], col1[1024], colf[1024];
	for (int x=0; x<dw; x++) {
		// 16.16 fixed point source position of this pixel's center (64-bit: the shift overflows 32)
		int sx = (int)(((int64_t)(x * 2 + 1) * src->w << 15) / dw) - (1 << 15);
		if (sx<0) sx = 0;
		col0[x] = sx >> 16;
		col1[x] = col0[x] + 1 < src->w ? col0[x] + 1 : col0[x];
		colf[x] = (sx >> 8) & 0xFF;
	}
	for (int y=0; y<dh; y++) {
		int sy = (int)(((int64_t)(y * 2 + 1) * src->h << 15) / dh) - (1 << 15);
		if (sy<0) sy = 0;
		int y0 = sy >> 16;
		int y1 = y0 + 1 < src->h ? y0 + 1 : y0;
		int fy = (sy >> 8) & 0xFF;
		uint8_t* row0 = (uint8_t*)src->pixels + y0 * src->pitch;
		uint8_t* row1 = (uint8_t*)src->pixels + y1 * src->pitch;
		uint8_t* out = (uint8_t*)dst->pixels + y * dst->pitch;
		for (int x=0; x<dw; x++) {
			int x0 = col0[x];
			int x1 = col1[x];
			int fx = colf[x];
			if (bpp==2) {
				uint16_t p[4] = { ((uint16_t*)row0)[x0], ((uint16_t*)row0)[x1], ((uint16_t*)row1)[x0], ((uint16_t*)row1)[x1] };
				int c[3];
				int shifts[3] = { 11, 5, 0 };
				int masks[3] = { 0x1F, 0x3F, 0x1F };
				for (int k=0; k<3; k++) {
					int a = (p[0] >> shifts[k]) & masks[k];
					int b = (p[1] >> shifts[k]) & masks[k];
					int d = (p[2] >> shifts[k]) & masks[k];
					int e = (p[3] >> shifts[k]) & masks[k];
					int top = a * (256 - fx) + b * fx;
					int bottom = d * (256 - fx) + e * fx;
					c[k] = (top * (256 - fy) + bottom * fy) >> 16;
				}
				if (color<256 || shade<256) {
					int r = c[0] << 3, g = c[1] << 2, b = c[2] << 3; // 8-bit
					int grey = (r * 77 + g * 151 + b * 28) >> 8;
					r = ((r * color + grey * (256 - color)) >> 8) * shade >> 8;
					g = ((g * color + grey * (256 - color)) >> 8) * shade >> 8;
					b = ((b * color + grey * (256 - color)) >> 8) * shade >> 8;
					c[0] = r >> 3; c[1] = g >> 2; c[2] = b >> 3;
				}
				((uint16_t*)out)[x] = (c[0] << 11) | (c[1] << 5) | c[2];
			}
			else {
				for (int k=0; k<bpp; k++) {
					int top = row0[x0*bpp+k] * (256 - fx) + row0[x1*bpp+k] * fx;
					int bottom = row1[x0*bpp+k] * (256 - fx) + row1[x1*bpp+k] * fx;
					out[x*bpp+k] = ((top * (256 - fy) + bottom * fy) >> 16) * shade >> 8;
				}
			}
		}
	}
	return dst;
}

static int getSlotPath(char* rom_path, int type, char* slot_path);
// the newer of the switcher's quick save and the game's last manual save: its screenshot and slot
// returns 0 when there's neither. Remembered per game while the switcher is open, it's asked every frame
#define SWITCHER_SAVE_CACHE (SWITCHER_MAX+2)
static struct { char path[MAX_PATH]; char preview[256]; int slot; int found; } switcher_saves[SWITCHER_SAVE_CACHE];
static int switcher_save_count = 0;
static void forgetSwitcherSaves(void) {
	switcher_save_count = 0;
}
static int lookUpSwitcherSave(Entry* entry, char* preview_path, int* slot);
static int getSwitcherSave(Entry* entry, char* preview_path, int* slot) {
	for (int i=0; i<switcher_save_count; i++) {
		if (!exactMatch(switcher_saves[i].path, entry->path)) continue;
		if (!switcher_saves[i].found) return 0;
		strcpy(preview_path, switcher_saves[i].preview);
		*slot = switcher_saves[i].slot;
		return 1;
	}
	int found = lookUpSwitcherSave(entry, preview_path, slot);
	if (switcher_save_count<SWITCHER_SAVE_CACHE) {
		int i = switcher_save_count++;
		strcpy(switcher_saves[i].path, entry->path);
		switcher_saves[i].found = found;
		if (found) {
			strcpy(switcher_saves[i].preview, preview_path);
			switcher_saves[i].slot = *slot;
		}
	}
	return found;
}
static int lookUpSwitcherSave(Entry* entry, char* preview_path, int* slot) {
	char slot_txt[256];
	int has_manual = getSlotPath(entry->path, entry->type, slot_txt);
	if (!slot_txt[0]) return 0;
	char base[256]; // .../<rom>.ext without the .txt
	strcpy(base, slot_txt);
	base[strlen(base) - strlen(".txt")] = '\0';

	char quick_path[256];
	sprintf(quick_path, "%s.%d.bmp", base, AUTO_RESUME_SLOT);
	struct stat quick_stat;
	int has_quick = stat(quick_path, &quick_stat)==0;

	char manual_path[256] = "";
	struct stat manual_stat;
	int manual_slot = has_manual ? getInt(slot_txt) : -1;
	if (has_manual) {
		sprintf(manual_path, "%s.%d.bmp", base, manual_slot);
		if (stat(manual_path, &manual_stat)!=0) has_manual = 0;
	}

	if (has_quick && (!has_manual || quick_stat.st_mtime>=manual_stat.st_mtime)) {
		strcpy(preview_path, quick_path);
		*slot = AUTO_RESUME_SLOT;
		return 1;
	}
	if (has_manual) {
		strcpy(preview_path, manual_path);
		*slot = manual_slot;
		return 1;
	}
	return 0;
}
// the art for entry at a carousel level, sized and shaded; NULL when there's none (or it can't be decoded yet)
static SDL_Surface* getCarouselArt(Entry* entry, int level, SDL_Surface* screen, int can_decode) {
	char res_path[MAX_PATH];
	if (inSwitcher()) {
		int slot;
		if (!getSwitcherSave(entry, res_path, &slot)) return NULL; // a card with its name, not box art
	}
	else getResPath(entry, res_path);

	CarouselArt* slot = NULL;
	CarouselArt* oldest = &carousel_cache[0];
	for (int i=0; i<CAROUSEL_CACHE_SIZE; i++) {
		CarouselArt* it = &carousel_cache[i];
		if (it->loaded && exactMatch(it->path, res_path)) {
			slot = it;
			break;
		}
		if (!it->loaded || (oldest->loaded && it->used<oldest->used)) oldest = it;
	}
	if (!slot) {
		if (!can_decode) return NULL;
		slot = oldest;
		CarouselArt_clear(slot);
		strcpy(slot->path, res_path);
		slot->art = loadArt(res_path, screen);
		slot->loaded = 1;
	}
	slot->used = ++carousel_clock;
	if (!slot->art) return NULL;

	if (!slot->sized[level]) {
		int box_w = SCALE1(CAROUSEL_ART_WIDTH) * carousel_scale[level] / 100;
		int box_h = SCALE1(CAROUSEL_ART_HEIGHT) * carousel_scale[level] / 100;
		// fit inside the box, keeping the art's shape
		int w = box_w;
		int h = slot->art->h * box_w / slot->art->w;
		if (h>box_h) {
			h = box_h;
			w = slot->art->w * box_h / slot->art->h;
		}
		if (w<1) w = 1; // very thin art would otherwise scale to nothing (and divide by zero)
		if (h<1) h = 1;
		slot->sized[level] = scaleArt(slot->art, w, h, carousel_shade[level], carousel_color[level]);
	}
	return slot->sized[level];
}

// nearest neighbor stretch of src into rect on dst, clipped to dst: for the in-between frames of a slide
// (SDL_BlitScaled on the device's SDL can misbehave with art hanging off the screen's edges)
static void blitStretched(SDL_Surface* src, SDL_Surface* dst, SDL_Rect* rect) {
	if (rect->w<=0 || rect->h<=0 || src->format->BytesPerPixel!=dst->format->BytesPerPixel) return;
	int bpp = dst->format->BytesPerPixel;
	int x0 = MAX(rect->x, 0), x1 = MIN(rect->x + rect->w, dst->w);
	int y0 = MAX(rect->y, 0), y1 = MIN(rect->y + rect->h, dst->h);
	if (x0>=x1 || y0>=y1) return;
	static int columns[1024]; // source column for each destination one
	if (x1-x0>1024) return;
	for (int x=x0; x<x1; x++) {
		int sx = (x - rect->x) * src->w / rect->w;
		columns[x-x0] = MIN(sx, src->w-1) * bpp;
	}
	for (int y=y0; y<y1; y++) {
		int sy = MIN((y - rect->y) * src->h / rect->h, src->h-1);
		uint8_t* in = (uint8_t*)src->pixels + sy * src->pitch;
		uint8_t* out = (uint8_t*)dst->pixels + y * dst->pitch + x0 * bpp;
		if (bpp==2) for (int x=0; x<x1-x0; x++) ((uint16_t*)out)[x] = *(uint16_t*)(in + columns[x]);
		else if (bpp==4) for (int x=0; x<x1-x0; x++) ((uint32_t*)out)[x] = *(uint32_t*)(in + columns[x]);
		else for (int x=0; x<x1-x0; x++) memcpy(out + x*bpp, in + columns[x], bpp);
	}
}

// whether an entry's art at a level is already decoded and sized (or known to be missing), without doing either
static int Carousel_isReady(Entry* entry, int level) {
	char res_path[MAX_PATH];
	if (inSwitcher()) {
		int slot;
		if (!getSwitcherSave(entry, res_path, &slot)) return 1; // a card, nothing to load
	}
	else getResPath(entry, res_path);
	for (int i=0; i<CAROUSEL_CACHE_SIZE; i++) {
		CarouselArt* it = &carousel_cache[i];
		if (it->loaded && exactMatch(it->path, res_path)) return !it->art || it->sized[level];
	}
	return 0;
}
// while idle, gets the art one step ahead ready: each neighbor at the size it'll be after a step towards it,
// and the next ones out decoded. Does one piece per call so input stays responsive, returns whether it did any
static int Carousel_preload(Directory* dir, SDL_Surface* screen) {
	int total = dir->entries->count;
	for (int place=1; place<=CAROUSEL_LEVELS+1; place++) {
		int level = MIN(place-1, CAROUSEL_LEVELS-1); // one closer than it is now
		for (int side=1; side>=-1; side-=2) {
			int i = dir->selected + side * place;
			if (i<0 || i>=total) continue;
			Entry* entry = dir->entries->items[i];
			if (Carousel_isReady(entry, level)) continue;
			getCarouselArt(entry, level, screen, 1);
			return 1;
		}
	}
	return 0;
}

// stepping slides the art over instead of jumping, easing out over CAROUSEL_SLIDE_MS
#define CAROUSEL_SLIDE_MS 90 // quick, so flicking through a long list stays snappy
static float carousel_slide_from = 0; // places the art started from, 0 when still
static uint32_t carousel_slide_at = 0;
static float Carousel_shift(uint32_t now) {
	if (!carousel_slide_from) return 0;
	float t = (float)(now - carousel_slide_at) / CAROUSEL_SLIDE_MS;
	if (t>=1) return 0;
	float left = 1 - t;
	return carousel_slide_from * left * left * left;
}
static void Carousel_slide(int step, uint32_t now) { // after moving one place, step is -1 or 1
	float from = Carousel_shift(now) + step; // a step mid-slide carries on from where the art is
	if (from>2) from = 2;
	if (from<-2) from = -2;
	carousel_slide_from = from;
	carousel_slide_at = now;
}

// shift slides everything by that many places (0 at rest), for the scroll animation
static void blitCarousel(SDL_Surface* screen, Directory* dir, int can_decode, float shift) {
	int total = dir->entries->count;
	int cx = screen->w / 2;
	int bottom = SCALE1(CAROUSEL_ART_BOTTOM);

	// art (or a card size when there's none) for the selected game and three either side, index 0 is the leftmost
	// the third ones are only seen sliding in or out while animating
	#define CAROUSEL_REACH CAROUSEL_LEVELS // places either side, one past the last level shown at rest
	#define CAROUSEL_SLOTS (CAROUSEL_REACH*2+1)
	static const int reach_scale = 25; // the place past the outermost level
	int mid = CAROUSEL_REACH;
	SDL_Surface* arts[CAROUSEL_SLOTS];
	int widths[CAROUSEL_SLOTS];
	int scales[CAROUSEL_SLOTS]; // percent the art in each slot is shown at
	int xs[CAROUSEL_SLOTS]; // centers at rest
	for (int n=0; n<CAROUSEL_SLOTS; n++) {
		int i = dir->selected + n - mid;
		int place = abs(n - mid);
		int level = MIN(place, CAROUSEL_LEVELS-1);
		scales[n] = place<CAROUSEL_LEVELS ? carousel_scale[place] : reach_scale;
		arts[n] = NULL;
		widths[n] = 0;
		if (i<0 || i>=total) continue;
		if (place>=CAROUSEL_LEVELS && !shift) continue;
		arts[n] = getCarouselArt(dir->entries->items[i], level, screen, can_decode);
		int level_w = arts[n] ? arts[n]->w : SCALE1(CAROUSEL_ART_HEIGHT) * carousel_scale[level] / 100;
		widths[n] = level_w * scales[n] / carousel_scale[level];
	}
	// each side art tucks partly behind the one nearer the middle, so spacing follows the art's shape
	// empty places still get a spot so art sliding through them has somewhere to go
	xs[mid] = cx;
	for (int side=-1; side<=1; side+=2) {
		int edge = cx + side * widths[mid] / 2;
		for (int place=1; place<=CAROUSEL_REACH; place++) {
			int n = mid + side * place;
			int w = widths[n] ? widths[n] : SCALE1(CAROUSEL_ART_HEIGHT) * scales[n] / 100;
			xs[n] = edge + side * w * (50 - CAROUSEL_TUCK) / 100;
			edge = xs[n] + side * w / 2;
		}
	}

	// where each art is right now: between its old place and its new one while sliding
	typedef struct { int n; float place; } Shown;
	Shown order[CAROUSEL_SLOTS];
	int count = 0;
	for (int n=0; n<CAROUSEL_SLOTS; n++) {
		if (!widths[n]) continue;
		float place = (n - mid) + shift;
		if (place<=-CAROUSEL_REACH || place>=CAROUSEL_REACH) continue;
		if (!shift && abs(n - mid)>=CAROUSEL_LEVELS) continue;
		order[count++] = (Shown){ n, place };
	}
	// outermost first so nearer art overlaps it
	for (int a=0; a<count; a++) {
		for (int b=a+1; b<count; b++) {
			if (fabsf(order[b].place)>fabsf(order[a].place)) {
				Shown tmp = order[a];
				order[a] = order[b];
				order[b] = tmp;
			}
		}
	}

	for (int k=0; k<count; k++) {
		int n = order[k].n;
		float place = order[k].place;
		float distance = fabsf(place);
		int near = (int)distance; // the two places it's between
		int far = MIN(near + 1, CAROUSEL_REACH);
		float t = distance - near;
		int near_n = mid + (place<0 ? -near : near);
		int far_n = mid + (place<0 ? -far : far);
		int x = xs[near_n] + (xs[far_n] - xs[near_n]) * t;
		float scale = scales[near_n] + (scales[far_n] - scales[near_n]) * t;
		int level = MIN((int)(distance + 0.5f), CAROUSEL_LEVELS-1); // the closest look for its shading

		Entry* entry = dir->entries->items[dir->selected + n - mid];
		int rest_level = MIN(abs(n - mid), CAROUSEL_LEVELS-1); // what arts[n] was fetched at
		SDL_Surface* art = level==rest_level ? arts[n] : getCarouselArt(entry, level, screen, can_decode);
		if (art) {
			int w = art->w * scale / carousel_scale[level] + 0.5f;
			int h = art->h * scale / carousel_scale[level] + 0.5f;
			SDL_Rect rect = { x - w/2, bottom - h, w, h };
			if (w==art->w && h==art->h) SDL_BlitSurface(art, NULL, screen, &rect);
			else blitStretched(art, screen, &rect); // only mid-slide, the resting frame is the smooth one
		}
		else {
			// no art: a card with the name on it
			int size = SCALE1(CAROUSEL_ART_HEIGHT) * scale / 100;
			SDL_Rect card = { x - size/2, bottom - size, size, size };
			GFX_blitRect(ASSET_DARK_GRAY_PILL, screen, &card);
			if (shift) continue; // text can't keep up mid-slide
			if (!level && !inSwitcher()) continue; // the selected game's name is already under it
			char* name = entry->name;
			trimSortingMeta(&name);
			if (!level) name = "No screenshot yet"; // switcher: one appears after leaving the game by holding MENU
			char label[256];
			GFX_truncateText(font.small, name, label, size - SCALE1(BUTTON_PADDING), 0);
			SDL_Surface* text = label[0] ? TTF_RenderUTF8_Blended(font.small, label, COLOR_DARK_TEXT) : NULL; // NULL for "", eg. when nothing fits
			if (!text) continue;
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ x - text->w/2, bottom - size/2 - text->h/2 });
			SDL_FreeSurface(text);
		}
	}

	// the selected game's name under its art
	Entry* entry = dir->entries->items[dir->selected];
	char* name = entry->unique ? entry->unique : entry->name;
	trimSortingMeta(&name);
	int dot_width = 0;
	if (entry->favorite) TTF_SizeUTF8(font.large, FAVORITE_DOT, &dot_width, NULL);
	char shown[256];
	GFX_truncateText(font.large, name, shown, screen->w - SCALE1(PADDING*4) - dot_width, 0);
	if (entry->favorite) strcat(shown, FAVORITE_DOT);
	SDL_Surface* text = shown[0] ? TTF_RenderUTF8_Blended(font.large, shown, COLOR_WHITE) : NULL;
	if (text) {
		SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ cx - text->w/2, bottom + SCALE1(8) });
		SDL_FreeSurface(text);
	}
}

// list text that hasn't changed since the last frame isn't measured or rendered again
#define TEXT_CACHE_SIZE 32
static struct { TTF_Font* font; uint32_t color; char text[256]; SDL_Surface* surface; uint32_t used; } text_cache[TEXT_CACHE_SIZE];
static struct { TTF_Font* font; int max_width; int padding; char text[256]; char shown[256]; int width; uint32_t used; } fit_cache[TEXT_CACHE_SIZE];
static uint32_t text_clock = 0;
// rendered text, owned by the cache (don't free it), NULL for ""
static SDL_Surface* renderText(TTF_Font* font, const char* text, SDL_Color color) {
	if (!text[0]) return NULL;
	uint32_t key = (color.r<<24) | (color.g<<16) | (color.b<<8) | color.a;
	int oldest = 0;
	for (int i=0; i<TEXT_CACHE_SIZE; i++) {
		if (text_cache[i].surface && text_cache[i].font==font && text_cache[i].color==key && exactMatch(text_cache[i].text, (char*)text)) {
			text_cache[i].used = ++text_clock;
			return text_cache[i].surface;
		}
		if (!text_cache[i].surface || (text_cache[oldest].surface && text_cache[i].used<text_cache[oldest].used)) oldest = i;
	}
	if (text_cache[oldest].surface) SDL_FreeSurface(text_cache[oldest].surface);
	text_cache[oldest].font = font;
	text_cache[oldest].color = key;
	snprintf(text_cache[oldest].text, sizeof(text_cache[oldest].text), "%s", text);
	text_cache[oldest].surface = TTF_RenderUTF8_Blended(font, text, color);
	text_cache[oldest].used = ++text_clock;
	return text_cache[oldest].surface;
}
// GFX_truncateText, remembered
static int fitText(TTF_Font* font, const char* text, char* shown, int max_width, int padding) {
	int oldest = 0;
	for (int i=0; i<TEXT_CACHE_SIZE; i++) {
		if (fit_cache[i].font==font && fit_cache[i].max_width==max_width && fit_cache[i].padding==padding && exactMatch(fit_cache[i].text, (char*)text)) {
			fit_cache[i].used = ++text_clock;
			strcpy(shown, fit_cache[i].shown);
			return fit_cache[i].width;
		}
		if (!fit_cache[i].font || (fit_cache[oldest].font && fit_cache[i].used<fit_cache[oldest].used)) oldest = i;
	}
	int width = GFX_truncateText(font, (char*)text, shown, max_width, padding);
	if (strlen(text)<sizeof(fit_cache[0].text)) {
		fit_cache[oldest].font = font;
		fit_cache[oldest].max_width = max_width;
		fit_cache[oldest].padding = padding;
		strcpy(fit_cache[oldest].text, text);
		snprintf(fit_cache[oldest].shown, sizeof(fit_cache[oldest].shown), "%s", shown);
		fit_cache[oldest].width = width;
		fit_cache[oldest].used = ++text_clock;
	}
	return width;
}
static void freeTextCache(void) {
	for (int i=0; i<TEXT_CACHE_SIZE; i++) if (text_cache[i].surface) SDL_FreeSurface(text_cache[i].surface);
	memset(text_cache, 0, sizeof(text_cache));
	memset(fit_cache, 0, sizeof(fit_cache));
}

///////////////////////////////////////

// the carousel on the GPU: each art is uploaded once (as is, and greyed) and the GPU draws it at any size,
// darkness and greyness, so sliding is smooth and nothing is scaled in software. Used when the platform has a GPU path
static int carousel_gpu = -1; // not tried yet
static int Carousel_useGPU(void) {
	if (carousel_gpu==-1) {
		SDL_Surface* probe = SDL_CreateRGBSurface(0, 1, 1, 32, 0x00FF0000,0x0000FF00,0x000000FF,0xFF000000);
		GFX_Texture* texture = PLAT_createTexture(probe);
		carousel_gpu = texture!=NULL;
		PLAT_destroyTexture(texture);
		SDL_FreeSurface(probe);
	}
	return carousel_gpu;
}

// a rounded rectangle in rgb with transparent corners, eg. a card, for the GPU to draw over art
static SDL_Surface* roundedSurface(int w, int h, int r, Uint8 cr, Uint8 cg, Uint8 cb) {
	SDL_Surface* surface = SDL_CreateRGBSurface(0, w, h, 32, 0x00FF0000,0x0000FF00,0x000000FF,0xFF000000);
	if (!surface) return NULL;
	for (int y=0; y<h; y++) {
		uint32_t* row = (uint32_t*)((uint8_t*)surface->pixels + y * surface->pitch);
		for (int x=0; x<w; x++) {
			int dx = x<r ? r-x : (x>=w-r ? x-(w-r-1) : 0);
			int dy = y<r ? r-y : (y>=h-r ? y-(h-r-1) : 0);
			row[x] = dx*dx + dy*dy > r*r ? 0 : SDL_MapRGBA(surface->format, cr,cg,cb,255);
		}
	}
	return surface;
}
// the brightness/volume pill, already drawn top right, lifted onto the GPU so the carousel's art can't cover it
static GFX_Texture* setting_texture = NULL;
static void queueHardwareGroup(SDL_Surface* screen, int ow) {
	SDL_Rect rect = { screen->w - SCALE1(PADDING) - ow, SCALE1(PADDING), ow, SCALE1(PILL_SIZE) };
	SDL_Surface* pill = SDL_CreateRGBSurface(0, rect.w, rect.h, 32, 0x00FF0000,0x0000FF00,0x000000FF,0xFF000000);
	if (!pill) return;
	SDL_BlitSurface(screen, &rect, pill, NULL); // opaque, then the corners cut away
	int r = rect.h / 2;
	for (int y=0; y<rect.h; y++) {
		uint32_t* row = (uint32_t*)((uint8_t*)pill->pixels + y * pill->pitch);
		for (int x=0; x<rect.w; x++) {
			int dx = x<r ? r-x : (x>=rect.w-r ? x-(rect.w-r-1) : 0);
			int dy = y<r ? r-y : (y>=rect.h-r ? y-(rect.h-r-1) : 0);
			if (dx*dx + dy*dy > r*r) row[x] = 0;
		}
	}
	PLAT_destroyTexture(setting_texture);
	setting_texture = PLAT_createTexture(pill);
	SDL_FreeSurface(pill);
	if (setting_texture) PLAT_queueTexture(setting_texture, &rect, 255, 255);
}
static void darkGray(SDL_Surface* screen, Uint8* r, Uint8* g, Uint8* b) { // the theme's card color
	SDL_GetRGB(RGB_DARK_GRAY, screen->format, r, g, b);
}
// a greyscale copy of RGB565 art, for fading the side art to grey on the GPU
static SDL_Surface* greySurface(SDL_Surface* src) {
	SDL_Surface* dst = SDL_ConvertSurface(src, src->format, 0);
	if (!dst || dst->format->BytesPerPixel!=2) return dst;
	for (int y=0; y<dst->h; y++) {
		uint16_t* row = (uint16_t*)((uint8_t*)dst->pixels + y * dst->pitch);
		for (int x=0; x<dst->w; x++) {
			uint16_t p = row[x];
			int r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
			int grey = (r * 77 + g * 151 + b * 28) >> 8;
			row[x] = ((grey >> 3) << 11) | ((grey >> 2) << 5) | (grey >> 3);
		}
	}
	return dst;
}

// the cache slot for an entry with its textures ready (art NULL means a card), NULL when it can't be decoded yet
// the GPU carousel's art is decoded on a thread (PNG decoding is what made scrolling stutter), nearest first;
// the main thread only uploads what's ready, and waits for (or decodes) just the selected game's art
#define DECODE_JOBS 12
#define CAROUSEL_UPLOADS 2 // decoded art moved to the GPU per frame (two textures each), nearest first
static int carousel_uploads_left = CAROUSEL_UPLOADS;
// what the last carousel frame spent its time on, for the slow frame log in minui.txt
static int carousel_uploads = 0;
static int carousel_decoded = 0; // decoded on the main thread (the selected game's, when the thread hadn't got to it)
static uint32_t carousel_waited = 0; // ms spent waiting for or decoding the selected game's art
static uint32_t frame_polled_at, frame_drawn_at, frame_flipped_at; // phases of the last frame, for the same log
static uint32_t frame_started_at, frame_draw_started_at;
enum { JOB_FREE, JOB_WAITING, JOB_WORKING, JOB_DONE };
typedef struct {
	char path[MAX_PATH];
	int state;
	int priority; // places from the selection: lower goes first
	int wanted; // asked for again this frame (waiting jobs nobody wants any more are dropped)
	int grey_too; // the carousel's side art needs a grey copy, the list's art doesn't
	SDL_Surface* art;
	SDL_Surface* grey;
} DecodeJob;
static DecodeJob decode_jobs[DECODE_JOBS];
static pthread_mutex_t decode_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t decode_wake = PTHREAD_COND_INITIALIZER; // a job to do, or time to stop
static pthread_cond_t decode_done = PTHREAD_COND_INITIALIZER; // a job finished
static pthread_t decode_thread;
static int decode_started = 0;
static int decode_stop = 0;
static SDL_Surface* decode_screen = NULL; // only its pixel format is read

static SDL_Surface* greySurface(SDL_Surface* src);
static void* Decoder_run(void* arg) {
#ifdef __linux__
	setpriority(PRIO_PROCESS, syscall(SYS_gettid), 10); // below the menu, so drawing goes first on a busy core
#endif
	pthread_mutex_lock(&decode_lock);
	while (!decode_stop) {
		DecodeJob* job = NULL;
		for (int i=0; i<DECODE_JOBS; i++) {
			DecodeJob* it = &decode_jobs[i];
			if (it->state==JOB_WAITING && (!job || it->priority<job->priority)) job = it;
		}
		if (!job) {
			pthread_cond_wait(&decode_wake, &decode_lock);
			continue;
		}
		job->state = JOB_WORKING;
		char path[MAX_PATH];
		strcpy(path, job->path);
		int grey_too = job->grey_too;
		pthread_mutex_unlock(&decode_lock);
		SDL_Surface* art = loadArt(path, decode_screen);
		SDL_Surface* grey = art && grey_too ? greySurface(art) : NULL;
		pthread_mutex_lock(&decode_lock);
		job->art = art;
		job->grey = grey;
		job->state = JOB_DONE;
		pthread_cond_broadcast(&decode_done);
	}
	pthread_mutex_unlock(&decode_lock);
	return NULL;
}
static void DecodeJob_free(DecodeJob* job) {
	if (job->art) SDL_FreeSurface(job->art);
	if (job->grey) SDL_FreeSurface(job->grey);
	memset(job, 0, sizeof(DecodeJob));
}
static void Decoder_request(char* path, int priority, int grey_too, SDL_Surface* screen) {
	if (prefixMatch("card:", path)) return;
	pthread_mutex_lock(&decode_lock);
	if (!decode_started) {
		decode_screen = screen;
		if (pthread_create(&decode_thread, NULL, Decoder_run, NULL)==0) decode_started = 1;
	}
	DecodeJob* job = NULL;
	for (int i=0; i<DECODE_JOBS && !job; i++) if (decode_jobs[i].state!=JOB_FREE && exactMatch(decode_jobs[i].path, path)) job = &decode_jobs[i];
	if (job) {
		job->wanted = 1;
		if (job->state==JOB_WAITING && priority<job->priority) job->priority = priority;
		if (job->state==JOB_WAITING) job->grey_too |= grey_too;
	}
	else {
		for (int i=0; i<DECODE_JOBS && !job; i++) if (decode_jobs[i].state==JOB_FREE) job = &decode_jobs[i];
		for (int i=0; i<DECODE_JOBS && !job; i++) if (decode_jobs[i].state==JOB_DONE && !decode_jobs[i].wanted) job = &decode_jobs[i]; // art nobody's looking at now
		DecodeJob* furthest = NULL;
		for (int i=0; i<DECODE_JOBS && !job; i++) {
			DecodeJob* it = &decode_jobs[i];
			if (it->state==JOB_WAITING && it->priority>priority && (!furthest || it->priority>furthest->priority)) furthest = it;
		}
		if (!job) job = furthest;
		if (job) {
			DecodeJob_free(job);
			strcpy(job->path, path);
			job->priority = priority;
			job->grey_too = grey_too;
			job->wanted = 1;
			job->state = JOB_WAITING;
			pthread_cond_signal(&decode_wake);
		}
	}
	pthread_mutex_unlock(&decode_lock);
}
// before a frame's requests: what isn't asked for again by the end of it isn't needed
static void Decoder_beginFrame(void) {
	pthread_mutex_lock(&decode_lock);
	for (int i=0; i<DECODE_JOBS; i++) decode_jobs[i].wanted = 0;
	pthread_mutex_unlock(&decode_lock);
}
static void Decoder_endFrame(void) {
	pthread_mutex_lock(&decode_lock);
	for (int i=0; i<DECODE_JOBS; i++) if (decode_jobs[i].state==JOB_WAITING && !decode_jobs[i].wanted) DecodeJob_free(&decode_jobs[i]);
	pthread_mutex_unlock(&decode_lock);
}
// the decoded art for path when it's done (1), waiting for it first when wait is set and it's being decoded
static int Decoder_take(char* path, SDL_Surface** art, SDL_Surface** grey, int wait) {
	int got = 0;
	pthread_mutex_lock(&decode_lock);
	for (int i=0; i<DECODE_JOBS; i++) {
		DecodeJob* job = &decode_jobs[i];
		if (job->state==JOB_FREE || !exactMatch(job->path, path)) continue;
		while (wait && job->state==JOB_WORKING) pthread_cond_wait(&decode_done, &decode_lock);
		if (job->state==JOB_DONE) {
			*art = job->art;
			*grey = job->grey;
			job->art = job->grey = NULL;
			DecodeJob_free(job);
			got = 1;
		}
		else if (wait && job->state==JOB_WAITING) DecodeJob_free(job); // the caller decodes it now instead
		break;
	}
	pthread_mutex_unlock(&decode_lock);
	return got;
}
static int Decoder_hasDone(void) {
	if (!decode_started) return 0;
	int done = 0;
	pthread_mutex_lock(&decode_lock);
	for (int i=0; i<DECODE_JOBS && !done; i++) done = decode_jobs[i].state==JOB_DONE && decode_jobs[i].wanted && decode_jobs[i].priority<CAROUSEL_LEVELS; // on screen, not prefetched
	pthread_mutex_unlock(&decode_lock);
	return done;
}
static void Decoder_quit(void) {
	if (decode_started) {
		pthread_mutex_lock(&decode_lock);
		decode_stop = 1;
		pthread_cond_broadcast(&decode_wake);
		pthread_mutex_unlock(&decode_lock);
		pthread_join(decode_thread, NULL);
		decode_started = 0;
	}
	for (int i=0; i<DECODE_JOBS; i++) DecodeJob_free(&decode_jobs[i]);
}

static int Carousel_has(char* res_path) {
	for (int i=0; i<CAROUSEL_CACHE_SIZE; i++) if (carousel_cache[i].loaded && exactMatch(carousel_cache[i].path, res_path)) return 1;
	return 0;
}
// where the carousel's picture for an entry comes from: its box art, its switcher screenshot, or "card:" for none
static void getCarouselPath(Entry* entry, char* res_path) {
	if (inSwitcher()) {
		int slot;
		if (!getSwitcherSave(entry, res_path, &slot)) sprintf(res_path, "card:%s", entry->path);
	}
	else getResPath(entry, res_path);
}
static CarouselArt* getCarouselSlot(Entry* entry, SDL_Surface* screen, int can_decode) {
	char res_path[MAX_PATH];
	getCarouselPath(entry, res_path);

	CarouselArt* slot = NULL;
	CarouselArt* oldest = &carousel_cache[0];
	for (int i=0; i<CAROUSEL_CACHE_SIZE; i++) {
		CarouselArt* it = &carousel_cache[i];
		if (it->loaded && exactMatch(it->path, res_path)) {
			slot = it;
			break;
		}
		if (!it->loaded || (oldest->loaded && it->used<oldest->used)) oldest = it;
	}
	SDL_Surface* decoded_grey = NULL;
	if (!slot) {
		int card = prefixMatch("card:", res_path);
		if (!card && !can_decode && carousel_uploads_left<=0) return NULL; // next frame: spreads the uploads out
		uint32_t started = SDL_GetTicks();
		SDL_Surface* art = NULL;
		int got = !card && Decoder_take(res_path, &art, &decoded_grey, can_decode); // the thread's, if it's done
		if (!got && !card && !can_decode) return NULL;
		slot = oldest;
		CarouselArt_clear(slot);
		strcpy(slot->path, res_path);
		slot->art = got ? art : (card ? NULL : loadArt(res_path, screen));
		slot->loaded = 1;
		if (!card) {
			carousel_uploads_left -= 1;
			carousel_uploads += 1;
			if (!got) carousel_decoded += 1;
			if (can_decode) carousel_waited += SDL_GetTicks() - started;
		}
	}
	slot->used = ++carousel_clock;
	if (slot->art && !slot->color) { // uploading is quick, the decoding was the slow part
		slot->color = PLAT_createTexture(slot->art);
		SDL_Surface* grey = decoded_grey ? decoded_grey : greySurface(slot->art);
		slot->grey = PLAT_createTexture(grey);
		if (grey) SDL_FreeSurface(grey);
		// fit the selected size, keeping the art's shape
		int box_w = SCALE1(CAROUSEL_ART_WIDTH), box_h = SCALE1(CAROUSEL_ART_HEIGHT);
		slot->fit_w = box_w;
		slot->fit_h = slot->art->h * box_w / slot->art->w;
		if (slot->fit_h>box_h) {
			slot->fit_h = box_h;
			slot->fit_w = slot->art->w * box_h / slot->art->h;
		}
		if (slot->fit_w<1) slot->fit_w = 1;
		if (slot->fit_h<1) slot->fit_h = 1;
	}
	if (!slot->art && !slot->label) {
		// a card: the name to put on it
		char* name = entry->name;
		trimSortingMeta(&name);
		char label[256];
		int card = SCALE1(CAROUSEL_ART_HEIGHT) * carousel_scale[1] / 100;
		GFX_truncateText(font.small, name, label, card - SCALE1(BUTTON_PADDING), 0);
		SDL_Surface* text = label[0] ? TTF_RenderUTF8_Blended(font.small, label, COLOR_DARK_TEXT) : NULL;
		slot->label = PLAT_createTexture(text);
		if (text) {
			slot->label_w = text->w;
			slot->label_h = text->h;
			SDL_FreeSurface(text);
		}
		slot->fit_w = slot->fit_h = SCALE1(CAROUSEL_ART_HEIGHT);
	}
	return slot;
}

static GFX_Texture* card_texture = NULL; // shared by every card, redone if the theme changes
static int card_theme = -1;
static GFX_Texture* no_shot_texture = NULL; // the switcher's selected card
static int no_shot_w, no_shot_h;
static void Carousel_freeGPU(void) {
	PLAT_destroyTexture(card_texture);
	PLAT_destroyTexture(no_shot_texture);
	card_texture = no_shot_texture = NULL;
}

#define CAROUSEL_SCROLL_DECODES 1 // art the frame may wait for while a direction is held: just the selected game's
#define CAROUSEL_PREFETCH 3 // places past the edges the thread decodes ahead
// decodes is how many art it may load this frame (the GPU makes the rest cheap, so even while scrolling fast)
static void blitCarouselGPU(SDL_Surface* screen, Directory* dir, int decodes, float shift) {
	int total = dir->entries->count;
	int cx = screen->w / 2;
	int bottom = SCALE1(CAROUSEL_ART_BOTTOM);
	int mid = CAROUSEL_REACH;
	static const int reach_scale = 25;

	if (!card_texture || card_theme!=GFX_getTheme()) {
		PLAT_destroyTexture(card_texture);
		Uint8 r,g,b;
		darkGray(screen, &r,&g,&b);
		int size = SCALE1(CAROUSEL_ART_HEIGHT);
		SDL_Surface* card = roundedSurface(size, size, SCALE1(15), r,g,b);
		card_texture = PLAT_createTexture(card);
		if (card) SDL_FreeSurface(card);
		card_theme = GFX_getTheme();
	}
	if (!no_shot_texture) {
		SDL_Surface* text = TTF_RenderUTF8_Blended(font.small, "No screenshot yet", COLOR_DARK_TEXT);
		no_shot_texture = PLAT_createTexture(text);
		if (text) {
			no_shot_w = text->w;
			no_shot_h = text->h;
			SDL_FreeSurface(text);
		}
	}

	Decoder_beginFrame();
	carousel_uploads_left = CAROUSEL_UPLOADS;
	carousel_uploads = carousel_decoded = 0;
	carousel_waited = 0;
	// each place's art, and where places sit at rest (spacing follows the art's shape, like the software carousel)
	CarouselArt* slots[CAROUSEL_SLOTS];
	int scales[CAROUSEL_SLOTS], widths[CAROUSEL_SLOTS], xs[CAROUSEL_SLOTS];
	for (int k=0; k<CAROUSEL_SLOTS; k++) { // middle first, then outwards, so what loads this frame is what matters most
		int place = (k + 1) / 2;
		int n = mid + (k%2 ? -place : place);
		int i = dir->selected + n - mid;
		scales[n] = place<CAROUSEL_LEVELS ? carousel_scale[place] : reach_scale;
		slots[n] = NULL;
		widths[n] = 0;
		if (i<0 || i>=total) continue;
		if (place>=CAROUSEL_LEVELS && !shift) continue;
		Entry* entry = dir->entries->items[i];
		slots[n] = getCarouselSlot(entry, screen, 0);
		if (!slots[n] && decodes>0) {
			slots[n] = getCarouselSlot(entry, screen, 1);
			decodes -= 1;
		}
		if (!slots[n]) { // for the thread, to show in a frame or two
			char res_path[MAX_PATH];
			getCarouselPath(entry, res_path);
			Decoder_request(res_path, place, 1, screen);
		}
		int fit_w = slots[n] ? slots[n]->fit_w : SCALE1(CAROUSEL_ART_HEIGHT);
		widths[n] = fit_w * scales[n] / 100;
	}
	// and the next few past either edge, so a held direction finds them decoded
	for (int place=CAROUSEL_REACH+1; place<=CAROUSEL_REACH+CAROUSEL_PREFETCH; place++) {
		for (int side=-1; side<=1; side+=2) {
			int i = dir->selected + side * place;
			if (i<0 || i>=total) continue;
			Entry* entry = dir->entries->items[i];
			char res_path[MAX_PATH];
			getCarouselPath(entry, res_path);
			if (!Carousel_has(res_path)) Decoder_request(res_path, place, 1, screen); // decoded, but left off the GPU until it's on screen
		}
	}
	Decoder_endFrame();
	xs[mid] = cx;
	for (int side=-1; side<=1; side+=2) {
		int edge = cx + side * widths[mid] / 2;
		for (int place=1; place<=CAROUSEL_REACH; place++) {
			int n = mid + side * place;
			int w = widths[n] ? widths[n] : SCALE1(CAROUSEL_ART_HEIGHT) * scales[n] / 100;
			xs[n] = edge + side * w * (50 - CAROUSEL_TUCK) / 100;
			edge = xs[n] + side * w / 2;
		}
	}

	typedef struct { int n; float place; } Shown;
	Shown order[CAROUSEL_SLOTS];
	int count = 0;
	for (int n=0; n<CAROUSEL_SLOTS; n++) {
		if (!widths[n]) continue;
		float place = (n - mid) + shift;
		if (place<=-CAROUSEL_REACH || place>=CAROUSEL_REACH) continue;
		if (!shift && abs(n - mid)>=CAROUSEL_LEVELS) continue;
		order[count++] = (Shown){ n, place };
	}
	for (int a=0; a<count; a++) { // outermost first so nearer art overlaps it
		for (int b=a+1; b<count; b++) {
			if (fabsf(order[b].place)>fabsf(order[a].place)) {
				Shown tmp = order[a];
				order[a] = order[b];
				order[b] = tmp;
			}
		}
	}

	for (int k=0; k<count; k++) {
		int n = order[k].n;
		float place = order[k].place;
		float distance = fabsf(place);
		int near = (int)distance;
		int far = MIN(near + 1, CAROUSEL_REACH);
		float t = distance - near;
		int near_n = mid + (place<0 ? -near : near);
		int far_n = mid + (place<0 ? -far : far);
		int x = xs[near_n] + (xs[far_n] - xs[near_n]) * t;
		float scale = scales[near_n] + (scales[far_n] - scales[near_n]) * t;
		// darkness and greyness blend smoothly with distance too (the place past the last level looks like the last)
		int near_level = MIN(near, CAROUSEL_LEVELS-1), far_level = MIN(far, CAROUSEL_LEVELS-1);
		float shade = carousel_shade[near_level] + (carousel_shade[far_level] - carousel_shade[near_level]) * t;
		float color = carousel_color[near_level] + (carousel_color[far_level] - carousel_color[near_level]) * t;
		int gpu_shade = MIN((int)(shade * 255 / 256), 255);
		int grey_alpha = MIN((int)((256 - color) * 255 / 256), 255);

		CarouselArt* slot = slots[n];
		if (slot && slot->art) {
			int w = slot->fit_w * scale / 100 + 0.5f;
			int h = slot->fit_h * scale / 100 + 0.5f;
			SDL_Rect rect = { x - w/2, bottom - h, w, h };
			PLAT_queueTexture(slot->color, &rect, 255, gpu_shade);
			if (grey_alpha>0) PLAT_queueTexture(slot->grey, &rect, grey_alpha, gpu_shade);
		}
		else {
			// no art (or not decoded yet while passing through): a card, with the name on the side ones
			int size = SCALE1(CAROUSEL_ART_HEIGHT) * scale / 100;
			SDL_Rect card = { x - size/2, bottom - size, size, size };
			PLAT_queueTexture(card_texture, &card, 255, gpu_shade);
			GFX_Texture* label = NULL;
			int lw = 0, lh = 0;
			if (distance>=0.5f && slot) { label = slot->label; lw = slot->label_w; lh = slot->label_h; }
			else if (inSwitcher() && slot) { label = no_shot_texture; lw = no_shot_w; lh = no_shot_h; }
			if (label) {
				SDL_Rect text = { x - lw/2, bottom - size/2 - lh/2, lw, lh };
				PLAT_queueTexture(label, &text, 255, 255);
			}
		}
	}

	// the selected game's name under its art, drawn with the rest of the menu
	Entry* entry = dir->entries->items[dir->selected];
	char* name = entry->unique ? entry->unique : entry->name;
	trimSortingMeta(&name);
	int dot_width = 0;
	if (entry->favorite) TTF_SizeUTF8(font.large, FAVORITE_DOT, &dot_width, NULL);
	char shown[256];
	fitText(font.large, name, shown, screen->w - SCALE1(PADDING*4) - dot_width, 0);
	if (entry->favorite) strcat(shown, FAVORITE_DOT);
	SDL_Surface* text = renderText(font.large, shown, COLOR_WHITE);
	if (text) SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ cx - text->w/2, bottom + SCALE1(8) });
}
// GPU version of Carousel_preload: decodes and uploads the art either side, one per call
static int Carousel_preloadGPU(Directory* dir, SDL_Surface* screen) {
	int total = dir->entries->count;
	carousel_uploads_left = 1;
	for (int place=1; place<=CAROUSEL_LEVELS+1; place++) {
		for (int side=1; side>=-1; side-=2) {
			int i = dir->selected + side * place;
			if (i<0 || i>=total) continue;
			Entry* entry = dir->entries->items[i];
			CarouselArt* slot = getCarouselSlot(entry, screen, 0); // what's there already (or the thread finished)
			if (slot) continue;
			char res_path[MAX_PATH];
			getCarouselPath(entry, res_path);
			Decoder_request(res_path, place, 1, screen);
		}
	}
	return 0; // the thread does the work, so the loop can still wait for the next frame
}

// a selected name too long for the space left of its art scrolls back and forth instead of covering the art
#define MARQUEE_PAUSE_MS 700 // still at each end
#define MARQUEE_SPEED 70 // unscaled pixels a second
#define MARQUEE_FRAME_MS 33
#define MARQUEE_ONLY 2 // a dirty value: only the scrolling name needs redrawing
static char marquee_key[MAX_PATH];
static uint32_t marquee_start = 0;
static int marquee_active = 0; // the last frame drew a scrolling name, so keep redrawing
static uint32_t marquee_drawn_at = 0;
// so a frame that only moves the name doesn't redraw the whole list: everything else, and the name and where it goes
static SDL_Surface* marquee_base = NULL;
static SDL_Surface* marquee_text = NULL;
static SDL_Rect marquee_dst; // w is the visible width
static int marquee_overflow = 0;
static int marqueeOffset(char* key, int overflow, uint32_t now);
static void blitMarquee(SDL_Surface* screen, uint32_t now) {
	SDL_BlitSurface(marquee_text, &(SDL_Rect){ marqueeOffset(marquee_key, marquee_overflow, now), 0, marquee_dst.w, marquee_text->h }, screen, &(SDL_Rect){ marquee_dst.x, marquee_dst.y });
}
static int marqueeOffset(char* key, int overflow, uint32_t now) {
	if (!exactMatch(key, marquee_key)) {
		strcpy(marquee_key, key);
		marquee_start = now;
	}
	marquee_active = 1;
	int scroll_ms = overflow * 1000 / SCALE1(MARQUEE_SPEED);
	int cycle = MARQUEE_PAUSE_MS + scroll_ms + MARQUEE_PAUSE_MS;
	int t = (now - marquee_start) % cycle;
	if (t<MARQUEE_PAUSE_MS) return 0;
	if (t<MARQUEE_PAUSE_MS+scroll_ms) return (t - MARQUEE_PAUSE_MS) * overflow / scroll_ms;
	return overflow;
}

// a short message centered over the list: the current letter while jumping with L1/R1, a new sort or view, pinning
// timed from when it's first on screen, not from the press, since loading new art can hold up that first frame
#define FLASH_LETTER_MS 900
#define FLASH_TEXT_MS 1000
#define FLASH_WAITING UINT32_MAX // shown, but not drawn yet
#define FLASH_FADE_MS 150 // its last moments, fading out
static char flash_text[64];
static int flash_big = 0;
static uint32_t flash_until = 0;
static TTF_Font* flash_font = NULL; // opened on first use
static int flash_gpu = 0; // drawn as a GPU texture this frame (eg. over the carousel)
static GFX_Texture* flash_texture = NULL;
static char flash_texture_text[64];
static int flash_texture_theme = -1;
static void showFlash(char* text, int big) {
	snprintf(flash_text, sizeof(flash_text), "%s", text);
	flash_big = big;
	flash_until = FLASH_WAITING;
}
static void blitFlash(SDL_Surface* screen) {
	if (!flash_until) return;
	if (flash_until==FLASH_WAITING) flash_until = SDL_GetTicks() + (flash_big ? FLASH_LETTER_MS : FLASH_TEXT_MS);
	int left = (int32_t)(flash_until - SDL_GetTicks());
	int alpha = left>=FLASH_FADE_MS ? 255 : (left>0 ? 255 * left / FLASH_FADE_MS : 0);
	if (flash_big && !flash_font) flash_font = TTF_OpenFont(FONT_PATH, SCALE1(FONT_LARGE*3));
	TTF_Font* f = flash_big && flash_font ? flash_font : font.large;
	
	SDL_Surface* text = TTF_RenderUTF8_Blended(f, flash_text, COLOR_WHITE);
	if (!text) return;
	int pad = SCALE1(flash_big ? PADDING*2 : BUTTON_PADDING*2);
	int w = text->w + pad*2;
	int h = text->h + pad*(flash_big ? 2 : 1);
	if (flash_big && w<h) w = h; // square for single letters
	SDL_Rect rect = { (screen->w-w)/2, (screen->h-h)/2, w, h };
	if (flash_big && showsCarousel()) rect.y = SCALE1(CAROUSEL_ART_BOTTOM - CAROUSEL_ART_HEIGHT/2) - h/2; // over the art, clear of the name under it
	if (flash_gpu) {
		// made once per message and drawn by the GPU on top of everything
		if (!flash_texture || !exactMatch(flash_texture_text, flash_text) || flash_texture_theme!=GFX_getTheme()) {
			PLAT_destroyTexture(flash_texture);
			Uint8 r,g,b;
			SDL_GetRGB(RGB_DARK_GRAY, screen->format, &r,&g,&b);
			SDL_Surface* box = roundedSurface(w, h, SCALE1(PILL_SIZE)/2, r,g,b);
			if (box) {
				SDL_BlitSurface(text, NULL, box, &(SDL_Rect){ (w-text->w)/2, (h-text->h)/2 });
				flash_texture = PLAT_createTexture(box);
				SDL_FreeSurface(box);
			}
			strcpy(flash_texture_text, flash_text);
			flash_texture_theme = GFX_getTheme();
		}
		PLAT_queueTexture(flash_texture, &rect, alpha, 255);
		SDL_FreeSurface(text);
		return;
	}
	// fading: what was under it goes back over it, more each frame
	SDL_Surface* under = alpha<255 ? SDL_CreateRGBSurfaceWithFormat(0, rect.w, rect.h, screen->format->BitsPerPixel, screen->format->format) : NULL;
	if (under) SDL_BlitSurface(screen, &rect, under, NULL);
	GFX_blitRect(ASSET_DARK_GRAY_PILL, screen, &rect);
	SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ rect.x + (w-text->w)/2, rect.y + (h-text->h)/2 });
	SDL_FreeSurface(text);
	if (under) {
		SDL_SetSurfaceBlendMode(under, SDL_BLENDMODE_BLEND);
		SDL_SetSurfaceAlphaMod(under, 255 - alpha);
		SDL_BlitSurface(under, NULL, screen, &(SDL_Rect){ rect.x, rect.y });
		SDL_FreeSurface(under);
	}
}
static void showLetter(Entry* entry) {
	int i = getIndexChar(entry->name);
	char letter[2] = { i ? 'A'+i-1 : '#', '\0' };
	showFlash(letter, 1);
}

// select and start double as volume/brightness modifiers on some devices
#define CAN_SWITCH_VIEW (BTN_MOD_VOLUME!=BTN_SELECT && BTN_MOD_BRIGHTNESS!=BTN_SELECT)
#define CAN_START (BTN_MOD_VOLUME!=BTN_START && BTN_MOD_BRIGHTNESS!=BTN_START) // picks a random game

// keeps the list's visible rows around selected, so switching back from the carousel lands in the right place
static void centerRows(Directory* dir, int selected) {
	int total = dir->entries->count;
	dir->start = selected - MAIN_ROW_COUNT/2;
	if (dir->start>total-MAIN_ROW_COUNT) dir->start = total-MAIN_ROW_COUNT;
	if (dir->start<0) dir->start = 0;
	dir->end = MIN(total, dir->start + MAIN_ROW_COUNT);
}
// deleting a game (MENU + X in a system's list): asks first, then whether its saves and box art go too

// a yes/no screen, 1 for A and 0 for B
static int askDelete(SDL_Surface* screen, char* title, char* detail, char* yes, char* no) {
	int dirty = 1;
	int result = -1;
	while (result<0) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_A)) result = 1;
		else if (PAD_justPressed(BTN_B)) result = 0;
		PWR_update(&dirty, NULL, NULL, NULL);
		if (dirty) {
			GFX_clear(screen);
			char shown[256];
			GFX_truncateText(font.large, title, shown, screen->w - SCALE1(PADDING*4), 0);
			SDL_Surface* big = TTF_RenderUTF8_Blended(font.large, shown, COLOR_WHITE);
			SDL_Surface* small = detail && detail[0] ? TTF_RenderUTF8_Blended(font.small, detail, COLOR_LIGHT_TEXT) : NULL;
			int h = big->h + (small ? SCALE1(4) + small->h : 0);
			int y = (screen->h - SCALE1(PILL_SIZE+PADDING) - h) / 2;
			SDL_BlitSurface(big, NULL, screen, &(SDL_Rect){ (screen->w - big->w) / 2, y });
			if (small) SDL_BlitSurface(small, NULL, screen, &(SDL_Rect){ (screen->w - small->w) / 2, y + big->h + SCALE1(4) });
			SDL_FreeSurface(big);
			if (small) SDL_FreeSurface(small);
			GFX_blitButtonGroup((char*[]){ "B",no, "A",yes, NULL }, 1, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}
	SFX_play(result ? SFX_CONFIRM : SFX_BACK);
	return result;
}

// a file of game name: the name, then '.' or '-' (eg. "Zelda.gb.sav", "Zelda.gb-a30.cfg" but not "Zelda.gbc.sav")
static int isFileOf(const char* file, const char* name) {
	int len = strlen(name);
	return !strncmp(file, name, len) && (file[len]=='.' || file[len]=='-');
}
#define DELETE_MAX 128
static char delete_saves[DELETE_MAX][MAX_PATH];
static int delete_save_count = 0;
static void findSaves(char* dir, char names[][MAX_PATH], int name_count, int only_cfg) {
	DIR* d = opendir(dir);
	if (!d) return;
	struct dirent* e;
	while ((e = readdir(d)) && delete_save_count<DELETE_MAX) {
		if (e->d_name[0]=='.' || e->d_type==DT_DIR) continue;
		if (only_cfg && !suffixMatch(".cfg", e->d_name)) continue;
		for (int i=0; i<name_count; i++) {
			if (!isFileOf(e->d_name, names[i])) continue;
			snprintf(delete_saves[delete_save_count++], MAX_PATH, "%s/%s", dir, e->d_name);
			break;
		}
	}
	closedir(d);
}
// every <tag>-<core> folder under root (states live in one per core)
static void findCoreSaves(char* root, char* tag, char names[][MAX_PATH], int name_count, int only_cfg) {
	DIR* d = opendir(root);
	if (!d) return;
	struct dirent* e;
	char prefix[MAX_PATH];
	snprintf(prefix, sizeof(prefix), "%s-", tag);
	while ((e = readdir(d))) {
		if (e->d_type!=DT_DIR || !prefixMatch(prefix, e->d_name)) continue;
		char dir[MAX_PATH];
		snprintf(dir, sizeof(dir), "%s/%s", root, e->d_name);
		findSaves(dir, names, name_count, only_cfg);
	}
	closedir(d);
}
// a multi-disc game's folder and everything in it
static void removeTree(char* path) {
	DIR* d = opendir(path);
	if (d) {
		struct dirent* e;
		while ((e = readdir(d))) {
			if (exactMatch(e->d_name, ".") || exactMatch(e->d_name, "..")) continue;
			char child[MAX_PATH];
			snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
			if (e->d_type==DT_DIR) removeTree(child);
			else unlink(child);
		}
		closedir(d);
		rmdir(path);
	}
	else unlink(path);
}
// a .cue's own tracks (FILE "x.bin" lines), which sit next to it
static void removeCueTracks(char* cue_path) {
	FILE* file = fopen(cue_path, "r");
	if (!file) return;
	char dir[MAX_PATH];
	strcpy(dir, cue_path);
	*strrchr(dir, '/') = '\0';
	char line[MAX_PATH];
	while (fgets(line, sizeof(line), file)) {
		char* start = strstr(line, "FILE \"");
		if (!start) continue;
		start += 6;
		char* end = strchr(start, '"');
		if (!end || end==start || memchr(start, '/', end-start)) continue; // only files beside the cue
		char track[MAX_PATH];
		snprintf(track, sizeof(track), "%s/%.*s", dir, (int)(end-start), start);
		unlink(track);
	}
	fclose(file);
}
static void removeFromRecents(Array* list, char* path) {
	int i = RecentArray_indexOf(list, path);
	if (i>=0) Recent_free(Array_remove(list, i));
}
static void deleteSelected(SDL_Surface* screen) {
	if (!isSortableFolder(top->path) || !top->entries->count) return; // a system's own list, not Recents etc.
	Entry* entry = top->entries->items[top->selected];
	char rel_path[MAX_PATH]; // what recents, favorites, play time and the switcher know it by
	if (entry->type==ENTRY_PAK || !getFavoritePath(entry, rel_path)) {
		showFlash("Only games can be deleted", 0);
		return;
	}
	char launch_path[MAX_PATH];
	snprintf(launch_path, sizeof(launch_path), "%s%s", SDCARD_PATH, rel_path);

	char title[256];
	snprintf(title, sizeof(title), "Delete %s?", entry->name);
	if (!askDelete(screen, title, "This can't be undone.", "DELETE", "CANCEL")) return;

	// its saves go by the name it's launched by (the .m3u for multi-disc), and by each disc's own name
	char names[DISC_MAX+1][MAX_PATH];
	int name_count = 0;
	strcpy(names[name_count++], strrchr(launch_path, '/')+1);
	if (entry->type==ENTRY_DIR) {
		DIR* d = opendir(entry->path);
		struct dirent* e;
		while (d && (e = readdir(d)) && name_count<DISC_MAX+1) {
			if (e->d_name[0]=='.' || e->d_type==DT_DIR || exactMatch(e->d_name, names[0])) continue;
			strcpy(names[name_count++], e->d_name);
		}
		if (d) closedir(d);
	}
	char tag[MAX_PATH];
	getEmuName(entry->path, tag);
	char dir[MAX_PATH];
	delete_save_count = 0;
	snprintf(dir, sizeof(dir), "%s/Saves/%s", SDCARD_PATH, tag);
	findSaves(dir, names, name_count, 0); // in-game saves, clock
	int game_saves = delete_save_count;
	findCoreSaves(SHARED_USERDATA_PATH, tag, names, name_count, 0); // save states
	int states = 0;
	for (int i=game_saves; i<delete_save_count; i++) if (strstr(strrchr(delete_saves[i], '/'), ".st")) states += 1;
	snprintf(dir, sizeof(dir), "%s/.minui/%s", SHARED_USERDATA_PATH, tag);
	findSaves(dir, names, name_count, 0); // save state pictures, last slot used
	findCoreSaves(USERDATA_PATH, tag, names, name_count, 1); // this game's own settings

	int drop_saves = 0;
	if (delete_save_count) {
		char detail[128];
		if (game_saves && states) snprintf(detail, sizeof(detail), "%i save file%s and %i save state%s", game_saves, game_saves==1?"":"s", states, states==1?"":"s");
		else if (states) snprintf(detail, sizeof(detail), "%i save state%s", states, states==1?"":"s");
		else if (game_saves) snprintf(detail, sizeof(detail), "%i save file%s", game_saves, game_saves==1?"":"s");
		else snprintf(detail, sizeof(detail), "Its settings");
		drop_saves = askDelete(screen, "Also delete its saves?", detail, "DELETE", "KEEP");
	}

	char art_path[MAX_PATH];
	getResPath(entry, art_path);
	char border_path[MAX_PATH];
	snprintf(border_path, sizeof(border_path), "%s/%s/%s.png", BORDERS_PATH, tag, names[0]);
	int drop_art = 0;
	if (exists(art_path) || exists(border_path)) drop_art = askDelete(screen, "Also delete its box art?", NULL, "DELETE", "KEEP");

	// the game itself
	if (entry->type==ENTRY_DIR) removeTree(entry->path);
	else {
		if (suffixMatch(".cue", entry->path)) removeCueTracks(entry->path);
		unlink(entry->path);
	}
	if (drop_saves) for (int i=0; i<delete_save_count; i++) unlink(delete_saves[i]);
	if (drop_art) {
		unlink(art_path);
		unlink(border_path);
	}

	// and every list that points at it
	removeFromRecents(recents, rel_path);
	saveRecents();
	if (RecentArray_indexOf(favorites, rel_path)>=0) {
		removeFromRecents(favorites, rel_path);
		saveFavorites();
		updateRootFavorites();
	}
	Switcher_remove(rel_path);
	removePlayTime(rel_path);
	sync();

	Entry_free(Array_remove(top->entries, top->selected));
	if (top->selected>=top->entries->count) top->selected = top->entries->count - 1;
	if (top->selected<0) top->selected = 0;
	Directory_reindex(top);
	centerRows(top, top->selected);

	// the system's last game: back to the main menu, which (like at startup) no longer lists it
	if (!top->entries->count && stack->count==2) {
		char system_path[MAX_PATH];
		strcpy(system_path, top->path);
		closeDirectory();
		int i = EntryArray_indexOf(top->entries, system_path);
		if (i>=0) {
			Entry_free(Array_remove(top->entries, i));
			if (top->selected>=top->entries->count) top->selected = top->entries->count - 1;
			if (top->selected<0) top->selected = 0;
			Directory_reindex(top);
			centerRows(top, top->selected);
		}
	}
	showFlash("Deleted", 0);
}

// renaming a game (MENU + Y): the file (or a multi-disc folder and its m3u or cue), and everything named after it,
// its box art, saves, states, settings and border, and every list that points at it

// a list file of paths relative to SDCARD_PATH (anything after a tab is kept): old_rel becomes new_rel, and when
// old_dir is set, files inside it (each disc of a multi-disc game) move to new_dir
static void renameInList(char* list_path, char* old_rel, char* new_rel, char* old_dir, char* new_dir) {
	FILE* file = fopen(list_path, "r");
	if (!file) return;
	static char out[64*1024];
	out[0] = '\0';
	int changed = 0;
	int dir_len = old_dir ? strlen(old_dir) : 0;
	char line[MAX_PATH*2];
	while (fgets(line, sizeof(line), file)) {
		char fixed[MAX_PATH*2];
		char* tab = strchr(line, '\t');
		int path_len = tab ? tab - line : (int)strcspn(line, "\r\n");
		if (path_len==(int)strlen(old_rel) && !strncmp(line, old_rel, path_len)) {
			snprintf(fixed, sizeof(fixed), "%s%s", new_rel, line + path_len);
			changed = 1;
		}
		else if (old_dir && path_len>dir_len && !strncmp(line, old_dir, dir_len) && line[dir_len]=='/') {
			snprintf(fixed, sizeof(fixed), "%s%s", new_dir, line + dir_len);
			changed = 1;
		}
		else strcpy(fixed, line);
		if (strlen(out)+strlen(fixed)<sizeof(out)) strcat(out, fixed);
	}
	fclose(file);
	if (changed) putFile(list_path, out);
}
static void renameInRecents(Array* list, char* old_rel, char* new_rel, char* old_dir, char* new_dir, char* new_name) {
	int dir_len = old_dir ? strlen(old_dir) : 0;
	for (int i=0; i<list->count; i++) {
		Recent* recent = list->items[i];
		char path[MAX_PATH];
		if (exactMatch(recent->path, old_rel)) {
			strcpy(path, new_rel);
			if (recent->alias) { // the name it was listed under is the old one
				free(recent->alias);
				recent->alias = strdup(new_name);
			}
		}
		else if (old_dir && prefixMatch(old_dir, recent->path) && recent->path[dir_len]=='/') snprintf(path, sizeof(path), "%s%s", new_dir, recent->path + dir_len);
		else continue;
		free(recent->path);
		recent->path = strdup(path);
	}
}
// drops map.txt's custom name for the old file, the typed name is the one to show now
static void dropMapAlias(char* dir, char* file_name) {
	char map_path[MAX_PATH];
	snprintf(map_path, sizeof(map_path), "%s/map.txt", dir);
	FILE* file = fopen(map_path, "r");
	if (!file) return;
	static char out[32*1024];
	out[0] = '\0';
	int changed = 0;
	int len = strlen(file_name);
	char line[MAX_PATH*2];
	while (fgets(line, sizeof(line), file)) {
		if (!strncmp(line, file_name, len) && line[len]=='\t') {
			changed = 1;
			continue;
		}
		if (strlen(out)+strlen(line)<sizeof(out)) strcat(out, line);
	}
	fclose(file);
	if (changed) putFile(map_path, out);
}
static void renameSelected(SDL_Surface* screen) {
	if (!isSortableFolder(top->path) || !top->entries->count) return;
	Entry* entry = top->entries->items[top->selected];
	char old_rel[MAX_PATH];
	if (entry->type==ENTRY_PAK || !getFavoritePath(entry, old_rel)) {
		showFlash("Only games can be renamed", 0);
		return;
	}
	int is_dir = entry->type==ENTRY_DIR; // a multi-disc game's folder, or a folder with a cue of the same name
	char old_launch[MAX_PATH];
	snprintf(old_launch, sizeof(old_launch), "%s%s", SDCARD_PATH, old_rel);
	if (!is_dir && !exactMatch(old_launch, entry->path)) {
		showFlash("Rename the game, not one of its discs", 0);
		return;
	}

	// what's typed is the file's name without its extension, "(USA)" and all
	char dir[MAX_PATH];
	strcpy(dir, entry->path);
	*strrchr(dir, '/') = '\0';
	char old_file[MAX_PATH];
	strcpy(old_file, strrchr(entry->path, '/')+1);
	char ext[32] = "";
	char name[128];
	snprintf(name, sizeof(name), "%s", old_file);
	char* dot = strrchr(name, '.');
	if (!is_dir && dot && dot>name && strlen(dot)<sizeof(ext)) {
		strcpy(ext, dot);
		*dot = '\0';
	}
	char old_name[128];
	strcpy(old_name, name);
	if (!KB_enterText(screen, "Rename", name, sizeof(name), "RENAME", KB_FILE_NAME)) {
		SFX_play(SFX_BACK);
		return;
	}
	char* start = name;
	while (*start==' ') start++;
	int len = strlen(start);
	while (len>0 && start[len-1]==' ') start[--len] = '\0';
	if (exactMatch(start, old_name)) return;
	if (!start[0] || start[0]=='.') {
		showFlash("That name can't be used", 0);
		return;
	}

	char new_file[MAX_PATH];
	snprintf(new_file, sizeof(new_file), "%s%s", start, ext);
	char new_path[MAX_PATH];
	snprintf(new_path, sizeof(new_path), "%s/%s", dir, new_file);
	if (exists(new_path) && strcasecmp(new_file, old_file)) { // (only the case changing is the same file on the card)
		showFlash("There's already a game by that name", 0);
		return;
	}

	// the name its saves go by: the file, or the m3u or cue inside the folder
	char old_save_name[MAX_PATH], new_save_name[MAX_PATH];
	strcpy(old_save_name, strrchr(old_launch, '/')+1);
	strcpy(new_save_name, old_save_name);
	char new_rel[MAX_PATH];
	if (is_dir) {
		char* launch_ext = strrchr(old_save_name, '.');
		int named_after_folder = launch_ext && (int)(launch_ext - old_save_name)==(int)strlen(old_file) && !strncmp(old_save_name, old_file, strlen(old_file));
		if (named_after_folder) snprintf(new_save_name, sizeof(new_save_name), "%s%s", new_file, launch_ext);
		snprintf(new_rel, sizeof(new_rel), "%s/%s", new_path + strlen(SDCARD_PATH), new_save_name);
	}
	else {
		strcpy(new_save_name, new_file);
		strcpy(new_rel, new_path + strlen(SDCARD_PATH));
	}

	// find the saves first, while they still match the old name
	char names[1][MAX_PATH];
	strcpy(names[0], old_save_name);
	char tag[MAX_PATH];
	getEmuName(entry->path, tag);
	char save_dir[MAX_PATH];
	delete_save_count = 0;
	snprintf(save_dir, sizeof(save_dir), "%s/Saves/%s", SDCARD_PATH, tag);
	findSaves(save_dir, names, 1, 0);
	findCoreSaves(SHARED_USERDATA_PATH, tag, names, 1, 0);
	snprintf(save_dir, sizeof(save_dir), "%s/.minui/%s", SHARED_USERDATA_PATH, tag);
	findSaves(save_dir, names, 1, 0);
	findCoreSaves(USERDATA_PATH, tag, names, 1, 1);
	snprintf(save_dir, sizeof(save_dir), "%s/%s", BORDERS_PATH, tag);
	findSaves(save_dir, names, 1, 0);

	// the game
	if (is_dir && !exactMatch(old_save_name, new_save_name)) {
		char from[MAX_PATH], to[MAX_PATH];
		snprintf(from, sizeof(from), "%s/%s", entry->path, old_save_name);
		snprintf(to, sizeof(to), "%s/%s", entry->path, new_save_name);
		rename(from, to);
	}
	if (rename(entry->path, new_path)) {
		LOG_info("rename %s failed: %s\n", entry->path, strerror(errno));
		if (is_dir && !exactMatch(old_save_name, new_save_name)) { // put the m3u back
			char from[MAX_PATH], to[MAX_PATH];
			snprintf(from, sizeof(from), "%s/%s", entry->path, new_save_name);
			snprintf(to, sizeof(to), "%s/%s", entry->path, old_save_name);
			rename(from, to);
		}
		showFlash("Couldn't rename it", 0);
		return;
	}

	// its box art (replacing any art already there under the new name), saves and the rest
	char old_art[MAX_PATH], new_art[MAX_PATH];
	getResPath(entry, old_art);
	snprintf(new_art, sizeof(new_art), "%s/.res/%s.png", dir, new_file);
	if (exists(old_art)) rename(old_art, new_art);
	int kept = 0;
	for (int i=0; i<delete_save_count; i++) {
		char* file = strrchr(delete_saves[i], '/') + 1;
		char to[MAX_PATH];
		snprintf(to, sizeof(to), "%.*s%s%s", (int)(file - delete_saves[i]), delete_saves[i], new_save_name, file + strlen(old_save_name));
		if (exists(to) && strcasecmp(to, delete_saves[i])) kept += 1; // never over another game's save
		else rename(delete_saves[i], to);
	}
	dropMapAlias(dir, old_file);

	// and every list that points at it
	char old_dir_rel[MAX_PATH], new_dir_rel[MAX_PATH];
	strcpy(old_dir_rel, entry->path + strlen(SDCARD_PATH));
	strcpy(new_dir_rel, new_path + strlen(SDCARD_PATH));
	char* from_dir = is_dir ? old_dir_rel : NULL;
	char* to_dir = is_dir ? new_dir_rel : NULL;
	char display_name[256];
	getDisplayName(new_path, display_name);
	renameInRecents(recents, old_rel, new_rel, from_dir, to_dir, display_name);
	saveRecents();
	renameInRecents(favorites, old_rel, new_rel, from_dir, to_dir, display_name);
	saveFavorites();
	renameInList(PLAYTIME_PATH, old_rel, new_rel, from_dir, to_dir);
	renameInList(SWITCHER_PATH, old_rel, new_rel, from_dir, to_dir);
	renameInList(GAME_CORES_PATH, old_rel, new_rel, from_dir, to_dir);
	DIR* collections = opendir(COLLECTIONS_PATH);
	if (collections) {
		struct dirent* e;
		while ((e = readdir(collections))) {
			if (e->d_name[0]=='.' || !suffixMatch(".txt", e->d_name) || exactMatch(e->d_name, "map.txt")) continue;
			char list_path[MAX_PATH];
			snprintf(list_path, sizeof(list_path), "%s/%s", COLLECTIONS_PATH, e->d_name);
			renameInList(list_path, old_rel, new_rel, from_dir, to_dir);
		}
		closedir(collections);
	}
	forgetSwitcherSaves();
	sync();

	// the list, re-sorted, still on the game
	int type = entry->type;
	Entry_free(top->entries->items[top->selected]);
	top->entries->items[top->selected] = Entry_new(new_path, type);
	Directory_markFavorites(top);
	Directory_sort(top);
	int i = EntryArray_indexOf(top->entries, new_path);
	if (i>=0) top->selected = i;
	Directory_reindex(top);
	centerRows(top, top->selected);
	showFlash(kept ? "Renamed (some old saves were in the way)" : "Renamed", 0);
}

// choosing a game's core (MENU + Y): a list of every core that can run it
static void pickCore(SDL_Surface* screen) {
	Entry* entry = top->entries->items[top->selected];
	char rel_path[MAX_PATH];
	if (entry->type==ENTRY_PAK || !getFavoritePath(entry, rel_path)) return;
	char sd_path[MAX_PATH];
	snprintf(sd_path, sizeof(sd_path), "%s%s", SDCARD_PATH, rel_path);
	char system_tag[MAX_PATH];
	getEmuName(sd_path, system_tag);

	Core cores[CORE_MAX];
	int count = getCores(system_tag, cores);
	if (count<2) {
		showFlash(count ? "This system has one core" : "No core for this game", 0);
		return;
	}
	char current_tag[64];
	if (!getGameCore(sd_path, current_tag)) strcpy(current_tag, system_tag);
	int current = 0;
	for (int i=0; i<count; i++) if (exactMatch(cores[i].tag, current_tag)) current = i;

	int selected = current;
	int dirty = 1;
	int result = -1; // -1 still choosing, 0 cancelled, 1 chosen
	while (result<0) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_B)) result = 0;
		else if (PAD_justPressed(BTN_A)) result = 1;
		else if (PAD_justRepeated(BTN_UP)) { selected = (selected + count - 1) % count; SFX_play(SFX_TICK); dirty = 1; }
		else if (PAD_justRepeated(BTN_DOWN)) { selected = (selected + 1) % count; SFX_play(SFX_TICK); dirty = 1; }
		PWR_update(&dirty, NULL, NULL, NULL);
		if (dirty) {
			GFX_clear(screen);
			char title[256];
			char shown[256];
			snprintf(title, sizeof(title), "Core for %s", entry->name);
			GFX_truncateText(font.large, title, shown, screen->w - SCALE1(PADDING*4), 0);
			SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, shown, COLOR_WHITE);
			int rows_h = SCALE1(PILL_SIZE) * count + SCALE1(PADDING + 32); // and two lines about the highlighted core
			int y = (screen->h - SCALE1(PILL_SIZE+PADDING) - text->h - SCALE1(PADDING) - rows_h) / 2;
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ (screen->w - text->w) / 2, y });
			y += text->h + SCALE1(PADDING);
			SDL_FreeSurface(text);
			int rows_w = SCALE1(260);
			int x = (screen->w - rows_w) / 2;
			for (int i=0; i<count; i++) {
				int is_selected = i==selected;
				if (is_selected) GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){ x - SCALE1(BUTTON_PADDING), y, rows_w + SCALE1(BUTTON_PADDING*2), SCALE1(PILL_SIZE) });
				SDL_Surface* name = TTF_RenderUTF8_Blended(font.large, cores[i].name, is_selected ? COLOR_BLACK : COLOR_WHITE);
				SDL_BlitSurface(name, NULL, screen, &(SDL_Rect){ x, y + (SCALE1(PILL_SIZE) - name->h) / 2 });
				SDL_FreeSurface(name);
				if (i==current || i==0) {
					SDL_Surface* note = TTF_RenderUTF8_Blended(font.small, i==current ? "IN USE" : "DEFAULT", is_selected ? COLOR_BLACK : COLOR_DARK_TEXT);
					SDL_BlitSurface(note, NULL, screen, &(SDL_Rect){ x + rows_w - note->w, y + (SCALE1(PILL_SIZE) - note->h) / 2 });
					SDL_FreeSurface(note);
				}
				y += SCALE1(PILL_SIZE);
			}
			if (cores[selected].about) { // what the highlighted one is good at, under the list
				int w,h;
				GFX_sizeText(font.small, (char*)cores[selected].about, SCALE1(16), &w,&h);
				GFX_blitText(font.small, (char*)cores[selected].about, SCALE1(16), COLOR_LIGHT_TEXT, screen, &(SDL_Rect){ 0, y + SCALE1(PADDING), screen->w, h });
			}
			GFX_blitButtonGroup((char*[]){ "B","CANCEL", "A","USE", NULL }, 1, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}
	SFX_play(result ? SFX_CONFIRM : SFX_BACK);
	if (!result || selected==current) return;
	setGameCore(rel_path, selected==0 ? NULL : cores[selected].tag); // the system's own core needs no entry
	showFlash(cores[selected].name, 0);
}

// the game menu (MENU + Y): change its core, get its box art, rename or delete it
#define BOX_ART_PAK SDCARD_PATH "/Tools/" PLATFORM "/Box Art.pak/launch.sh"
static void gameMenu(SDL_Surface* screen) {
	Entry* entry = top->entries->items[top->selected];
	char rel_path[MAX_PATH];
	if (entry->type==ENTRY_PAK || !getFavoritePath(entry, rel_path)) {
		showFlash("Only games have options", 0);
		return;
	}
	char sd_path[MAX_PATH];
	snprintf(sd_path, sizeof(sd_path), "%s%s", SDCARD_PATH, rel_path);
	char system_tag[MAX_PATH];
	getEmuName(sd_path, system_tag);

	enum { ROW_CORE, ROW_ART, ROW_RENAME, ROW_DELETE };
	int rows[4];
	int count = 0;
	Core cores[CORE_MAX];
	char core_name[64] = "";
	if (getCores(system_tag, cores)>1) {
		char tag[64];
		if (!getGameCore(sd_path, tag)) strcpy(tag, system_tag);
		getCoreName(tag, core_name, &(const char*){NULL});
		rows[count++] = ROW_CORE;
	}
	char art_path[MAX_PATH];
	getResPath(entry, art_path);
	int has_art = exists(art_path);
	if (exists(BOX_ART_PAK)) rows[count++] = ROW_ART;
	if (isSortableFolder(top->path)) {
		rows[count++] = ROW_RENAME;
		rows[count++] = ROW_DELETE;
	}
	if (!count) {
		showFlash("No options for this game", 0);
		return;
	}

	int selected = 0;
	int dirty = 1;
	int result = -1;
	while (result<0) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_B)) result = 0; // (not a MENU tap: letting go of MENU from the combo would count)
		else if (PAD_justPressed(BTN_A)) result = 1;
		else if (PAD_justRepeated(BTN_UP)) { selected = (selected + count - 1) % count; SFX_play(SFX_TICK); dirty = 1; }
		else if (PAD_justRepeated(BTN_DOWN)) { selected = (selected + 1) % count; SFX_play(SFX_TICK); dirty = 1; }
		PWR_update(&dirty, NULL, NULL, NULL);
		if (dirty) {
			GFX_clear(screen);
			char shown[256];
			GFX_truncateText(font.large, entry->name, shown, screen->w - SCALE1(PADDING*4), 0);
			SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, shown, COLOR_WHITE);
			int block_h = text->h + SCALE1(PADDING) + SCALE1(PILL_SIZE) * count + SCALE1(PADDING + 16);
			int y = (screen->h - SCALE1(PILL_SIZE+PADDING) - block_h) / 2;
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ (screen->w - text->w) / 2, y });
			y += text->h + SCALE1(PADDING);
			SDL_FreeSurface(text);
			int rows_w = SCALE1(260);
			int x = (screen->w - rows_w) / 2;
			char* hint = "";
			for (int i=0; i<count; i++) {
				int is_selected = i==selected;
				char* name = "";
				char* value = "";
				if (rows[i]==ROW_CORE) { name = "Change core"; value = core_name; if (is_selected) hint = "Pick which emulator runs this game"; }
				else if (rows[i]==ROW_ART) { name = has_art ? "Replace box art" : "Get box art"; if (is_selected) hint = "Downloads it from the libretro library (needs Wi-Fi)"; }
				else if (rows[i]==ROW_RENAME) { name = "Rename"; if (is_selected) hint = "Its box art and saves keep up with the new name"; }
				else if (rows[i]==ROW_DELETE) { name = "Delete game"; if (is_selected) hint = "Asks first, then about its saves and box art"; }
				if (is_selected) GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){ x - SCALE1(BUTTON_PADDING), y, rows_w + SCALE1(BUTTON_PADDING*2), SCALE1(PILL_SIZE) });
				SDL_Surface* name_txt = TTF_RenderUTF8_Blended(font.large, name, is_selected ? COLOR_BLACK : COLOR_WHITE);
				SDL_BlitSurface(name_txt, NULL, screen, &(SDL_Rect){ x, y + (SCALE1(PILL_SIZE) - name_txt->h) / 2 });
				SDL_FreeSurface(name_txt);
				if (value[0]) {
					SDL_Surface* value_txt = TTF_RenderUTF8_Blended(font.small, value, is_selected ? COLOR_BLACK : COLOR_DARK_TEXT);
					SDL_BlitSurface(value_txt, NULL, screen, &(SDL_Rect){ x + rows_w - value_txt->w, y + (SCALE1(PILL_SIZE) - value_txt->h) / 2 });
					SDL_FreeSurface(value_txt);
				}
				y += SCALE1(PILL_SIZE);
			}
			SDL_Surface* hint_txt = TTF_RenderUTF8_Blended(font.small, hint, COLOR_LIGHT_TEXT);
			if (hint_txt) {
				SDL_BlitSurface(hint_txt, NULL, screen, &(SDL_Rect){ (screen->w - hint_txt->w) / 2, y + SCALE1(PADDING) });
				SDL_FreeSurface(hint_txt);
			}
			GFX_blitButtonGroup((char*[]){ "B","BACK", "A","SELECT", NULL }, 1, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}
	if (!result) {
		SFX_play(SFX_BACK);
		return;
	}
	SFX_play(SFX_CONFIRM);
	int row = rows[selected];
	if (row==ROW_CORE) pickCore(screen);
	else if (row==ROW_RENAME) renameSelected(screen);
	else if (row==ROW_DELETE) deleteSelected(screen);
	else if (row==ROW_ART) {
		// off to the Box Art tool for this one game, then back to this same spot in the list
		char last[MAX_PATH];
		if (exactMatch(FAUX_FAVORITES_PATH, top->path) || exactMatch(FAUX_RECENT_PATH, top->path) || exactMatch(FAUX_MOSTPLAYED_PATH, top->path) || prefixMatch(COLLECTIONS_PATH, top->path)) {
			snprintf(last, sizeof(last), "%s/%s", top->path, strrchr(entry->path, '/')+1);
		}
		else snprintf(last, sizeof(last), "%s", entry->path);
		saveLast(last);
		char pak[MAX_PATH], game[MAX_PATH], cmd[MAX_PATH*4];
		strcpy(pak, BOX_ART_PAK);
		strcpy(game, entry->path);
		snprintf(cmd, sizeof(cmd), "'%s' '%s'", escapeSingleQuotes(pak), escapeSingleQuotes(game));
		queueNext(cmd);
	}
}

static void openSwitcher(void) {
	forgetSwitcherSaves(); // a game may have been saved since
	openDirectory(FAUX_SWITCHER_PATH, 0);
	top->selected = 0; // the game played most recently
	centerRows(top, 0);
	carousel_slide_from = 0;
	if (top->entries->count>0) readyResume(top->entries->items[0]);
}
static int select_held = 0; // this SELECT press already pinned, so its release doesn't also switch views

// on a crash, logs where it happened (and where minui is loaded) to minui.txt, so it can be traced to a line
static void crashHandler(int signal, siginfo_t* info, void* context) {
	char line[160];
	int len;
#if defined(__arm__) && defined(__linux__)
	ucontext_t* uc = context;
	len = snprintf(line, sizeof(line), "crash: signal %i at pc %08lx lr %08lx address %p\n", signal, (unsigned long)uc->uc_mcontext.arm_pc, (unsigned long)uc->uc_mcontext.arm_lr, info->si_addr);
#else
	len = snprintf(line, sizeof(line), "crash: signal %i address %p\n", signal, info->si_addr);
#endif
	// minui.txt is replaced when minui restarts, so this goes to its own file too
	int out = open(USERDATA_PATH "/logs/minui-crash.txt", O_WRONLY|O_CREAT|O_APPEND, 0644);
	write(STDERR_FILENO, line, len);
	if (out>=0) write(out, line, len);
	int maps = open("/proc/self/maps", O_RDONLY);
	if (maps>=0) {
		char buffer[4096];
		int got;
		while ((got = read(maps, buffer, sizeof(buffer)))>0) {
			write(STDERR_FILENO, buffer, got);
			if (out>=0) write(out, buffer, got);
		}
		close(maps);
	}
	if (out>=0) close(out);
	_exit(1);
}

int main (int argc, char *argv[]) {
	struct sigaction crash = {0};
	crash.sa_sigaction = crashHandler;
	crash.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &crash, NULL);
	sigaction(SIGBUS, &crash, NULL);
	sigaction(SIGFPE, &crash, NULL);
	sigaction(SIGILL, &crash, NULL);
	sigaction(SIGABRT, &crash, NULL);

	// LOG_info("time from launch to:\n");
	// unsigned long main_begin = SDL_GetTicks();
	// unsigned long first_draw = 0;
	
	uint64_t main_begin = getMicroseconds(); // for the startup timings in minui.txt
	if (autoResume()) return 0; // nothing to do
	
	simple_mode = exists(SIMPLE_MODE_PATH);
	srand((unsigned)getMicroseconds());

	LOG_info("MinUI\n");
	InitSettings();
	
	SDL_Surface* screen = GFX_init(MODE_MAIN);
	// LOG_info("- graphics init: %lu\n", SDL_GetTicks() - main_begin);
	
	PAD_init();
	// LOG_info("- input init: %lu\n", SDL_GetTicks() - main_begin);
	
	PWR_init();
	if (!HAS_POWER_BUTTON && !simple_mode) PWR_disableSleep();
	// LOG_info("- power init: %lu\n", SDL_GetTicks() - main_begin);
	
	SDL_Surface* version = NULL;
	
	Menu_init();
	if (exists(SWITCHER_OPEN_PATH)) { // left a game by holding MENU
		unlink(SWITCHER_OPEN_PATH);
		openSwitcher();
	}
	// LOG_info("- menu init: %lu\n", SDL_GetTicks() - main_begin);
	
	// the menu runs on one slow core to save battery, except:
	// - for a while after boot, when Wi-Fi and the system's own startup are still busy (both cores at full speed),
	//   and briefly every time it starts (eg. back from a game), while the first art decodes
	// - while buttons are being pressed, so new art decodes quickly (both cores, the second decoding art)
	#define BOOT_STAMP_PATH "/tmp/minui_booted"
	#define BOOT_BOOST_MS 10000
	#define START_BOOST_MS 3000
	#define NAV_BOOST_MS 2500
	int fresh_boot = !exists(BOOT_STAMP_PATH);
	if (fresh_boot) touch(BOOT_STAMP_PATH);
	uint32_t boot_boost_until = SDL_GetTicks() + (fresh_boot ? BOOT_BOOST_MS : START_BOOST_MS);
	int cpu_speed = CPU_SPEED_NORMAL;
	PWR_setCPUSpeed(cpu_speed);
	uint32_t last_input_at = 0;
	GFX_setVsync(VSYNC_STRICT);
	SFX_warmUp(); // the first tick shouldn't have to wait for the audio device
	BGM_start(); // decodes on its own low priority thread, fading in

	PAD_reset();
	int dirty = 1;
	int scrolling = 0;
	int show_version = 0;
	#define INFO_ROW_MOST_PLAYED 0 // then one row per power timer
	#define INFO_ROW_THEME (1 + PWR_TIMER_COUNT)
	#define INFO_ROW_SOUNDS (2 + PWR_TIMER_COUNT)
	#define INFO_ROW_MUSIC (3 + PWR_TIMER_COUNT)
	#define INFO_ROW_COUNT (4 + PWR_TIMER_COUNT)
	#define INFO_ROWS_SHOWN 5 // the rest scroll into view
	int info_row = 0; // selected row on the version screen
	int info_start = 0; // first row shown
	uint32_t clear_armed_at = 0; // first X press on Most played, a second one within CLEAR_CONFIRM_MS clears
	#define CLEAR_CONFIRM_MS 3000
	int show_setting = 0; // 1=brightness,2=volume
	int was_online = PLAT_isOnline();
	
	// LOG_info("- loop start: %lu\n", SDL_GetTicks() - main_begin);
	while (!quit) {
		GFX_startFrame();
		unsigned long now = SDL_GetTicks();
		
		// a slow frame (eg. decoding art nobody could preload) would eat the slide, so it starts again after it
		static uint32_t last_frame_at = 0;
		uint32_t frame_gap = now - last_frame_at;
		if (carousel_slide_from && last_frame_at && frame_gap>40) carousel_slide_at += frame_gap - 16;
		if (last_frame_at && frame_gap>=50 && showsCarousel() && Carousel_useGPU()) { // what a slow carousel frame was doing
			LOG_info("slow carousel frame: %ums (%i uploaded, %i decoded here, %ums on the selected art; input %u, moving %u, drawing %u, to screen %u)\n",
				frame_gap, carousel_uploads, carousel_decoded, carousel_waited,
				frame_polled_at-frame_started_at, frame_draw_started_at ? frame_draw_started_at-frame_polled_at : 0,
				frame_drawn_at ? frame_drawn_at-frame_draw_started_at : 0, frame_flipped_at ? frame_flipped_at-frame_drawn_at : 0);
		}
		frame_started_at = now;
		frame_draw_started_at = frame_drawn_at = frame_flipped_at = 0;
		// the first seconds' hiccups go in minui.txt, to see what a fresh boot is waiting on
		if (last_frame_at && frame_gap>=100 && (getMicroseconds()-main_begin)<20*1000000) {
			LOG_info("slow frame: %ums, %llums after start\n", frame_gap, (unsigned long long)(getMicroseconds()-main_begin)/1000);
		}
		last_frame_at = now;
		
		PAD_poll();
		
		if (PAD_isPressed(BTN_ANY|BTN_MENU) || PAD_justReleased(BTN_ANY|BTN_MENU)) last_input_at = now;
		int wanted_speed = CPU_SPEED_MENU;
		if (boot_boost_until && (int32_t)(boot_boost_until-now)>0) wanted_speed = CPU_SPEED_NORMAL;
		else if (last_input_at && now-last_input_at<NAV_BOOST_MS) wanted_speed = CPU_SPEED_NORMAL; // both cores: the second decodes the carousel's art
		if (wanted_speed!=cpu_speed) {
			cpu_speed = wanted_speed;
			PWR_setCPUSpeedAsync(cpu_speed);
		}
			
		int selected = top->selected;
		int total = top->entries->count;
		
		PWR_update(&dirty, &show_setting, NULL, NULL);
		frame_polled_at = SDL_GetTicks();
		
		// a long MENU press (anywhere) opens the game switcher when it's let go, a tap still opens the info screen
		// any other button pressed meanwhile makes it a combo instead (eg. MENU + volume for brightness)
		static uint32_t menu_down_at = 0;
		static int menu_combo = 0;
		if (PAD_justPressed(BTN_MENU)) {
			menu_down_at = now;
			menu_combo = 0;
		}
		else if (PAD_isPressed(BTN_MENU) && PAD_justPressed(BTN_ANY)) menu_combo = 1;
		if (PAD_justReleased(BTN_MENU) && !menu_combo && now-menu_down_at>=SWITCHER_HOLD_MS) {
			if (show_version) {
				show_version = 0;
				if (!HAS_POWER_BUTTON && !simple_mode) PWR_disableSleep();
			}
			if (!inSwitcher()) {
				openSwitcher();
				SFX_play(SFX_CONFIRM);
			}
			selected = top->selected;
			total = top->entries->count;
			dirty = 1;
		}
		
		int is_online = PLAT_isOnline();
		if (was_online!=is_online) dirty = 1;
		was_online = is_online;
		
		if (show_version) {
			if (clear_armed_at && (now-clear_armed_at>=CLEAR_CONFIRM_MS || info_row!=INFO_ROW_MOST_PLAYED)) {
				clear_armed_at = 0;
				dirty = 1;
			}
			if (PAD_justPressed(BTN_B) || PAD_tappedMenu(now)) {
				show_version = 0;
				dirty = 1;
				if (!HAS_POWER_BUTTON && !simple_mode) PWR_disableSleep();
				SFX_play(SFX_BACK);
			}
			else if (PAD_justRepeated(BTN_UP) && info_row>0) {
				info_row -= 1;
				dirty = 1;
				SFX_play(SFX_TICK);
			}
			else if (PAD_justRepeated(BTN_DOWN) && info_row<INFO_ROW_COUNT-1) {
				info_row += 1;
				dirty = 1;
				SFX_play(SFX_TICK);
			}
			else if (info_row==INFO_ROW_SOUNDS && (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_LEFT) || PAD_justPressed(BTN_RIGHT))) {
				SFX_setMenuSounds(!SFX_menuSoundsEnabled());
				SFX_play(SFX_CONFIRM); // heard when turning them on
				dirty = 1;
			}
			else if (info_row==INFO_ROW_MUSIC && PAD_justPressed(BTN_A)) {
				BGM_setEnabled(!BGM_isEnabled());
				SFX_play(SFX_CONFIRM);
				dirty = 1;
			}
			else if (info_row==INFO_ROW_MUSIC && (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT))) {
				// the volume, and turning it on if it's off
				if (BGM_isEnabled()) BGM_setVolume(BGM_getVolume() + (PAD_justRepeated(BTN_RIGHT) ? 1 : -1));
				else BGM_setEnabled(1);
				SFX_play(SFX_TICK);
				dirty = 1;
			}
			else if (info_row==INFO_ROW_MOST_PLAYED && PAD_justPressed(BTN_X) && info_top_name[0]) {
				if (clear_armed_at && now-clear_armed_at<CLEAR_CONFIRM_MS) {
					// kept as a backup rather than deleted so a mistaken clear can be undone by hand
					rename(PLAYTIME_PATH, PLAYTIME_PATH ".bak");
					sync();
					clear_armed_at = 0;
					info_top_name[0] = '\0';
					if (version) SDL_FreeSurface(version);
					version = NULL; // rebuilt with the cleared play time
				}
				else clear_armed_at = now;
				dirty = 1;
			}
			else if (info_row==INFO_ROW_MOST_PLAYED && PAD_justPressed(BTN_A) && info_top_name[0]) {
				show_version = 0;
				if (!HAS_POWER_BUTTON && !simple_mode) PWR_disableSleep();
				most_played_from_info = 1;
				openDirectory(FAUX_MOSTPLAYED_PATH, 0);
				if (top->entries->count>0) readyResume(top->entries->items[top->selected]);
				dirty = 1;
			}
			else if (info_row==INFO_ROW_THEME && (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT))) {
				// wraps, there's no wrong end to land on
				int count = GFX_getThemeCount();
				GFX_setTheme((GFX_getTheme() + (PAD_justRepeated(BTN_RIGHT) ? 1 : -1) + count) % count);
				SFX_play(SFX_TICK);
				Carousel_quit(); // its cards' text is in the theme's colors
				if (version) SDL_FreeSurface(version);
				version = NULL; // its text is drawn in the theme's colors
				dirty = 1;
			}
			else if (info_row>INFO_ROW_MOST_PLAYED && info_row<INFO_ROW_THEME && (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT))) { // power timers
				// clamped rather than wrapped so holding left/right can't jump to Never by accident
				int timer = info_row - 1;
				int index = PWR_getTimer(timer) + (PAD_justRepeated(BTN_RIGHT) ? 1 : -1);
				if (index>=0 && index<PWR_getTimerCount(timer)) {
					PWR_setTimer(timer, index);
					dirty = 1;
					SFX_play(SFX_TICK);
				}
			}
		}
		else {
			if (PAD_tappedMenu(now)) {
				show_version = 1;
				dirty = 1;
				SFX_play(SFX_CONFIRM);
				if (!HAS_POWER_BUTTON && !simple_mode) PWR_enableSleep();
			}
			else if (total>0 && showsCarousel()) {
				// left/right (and up/down) step one game, wrapping only on a fresh press like the list does
				int step = 0;
				if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_UP)) step = -1;
				else if (PAD_justRepeated(BTN_RIGHT) || PAD_justRepeated(BTN_DOWN)) step = 1;
				if (step) {
					int fresh = PAD_justPressed(BTN_LEFT|BTN_RIGHT|BTN_UP|BTN_DOWN);
					int next = selected + step;
					if (next<0) next = fresh ? total-1 : 0;
					else if (next>=total) next = fresh ? 0 : total-1;
					if (next!=selected) {
						if (next==selected+step) Carousel_slide(step, now);
						else carousel_slide_from = 0; // wrapped around, nothing to slide from
						selected = next;
						centerRows(top, selected);
					}
				}
			}
			else if (total>0) {
				if (PAD_justRepeated(BTN_UP)) {
					if (selected==0 && !PAD_justPressed(BTN_UP)) {
						// stop at top
					}
					else {
						selected -= 1;
						if (selected<0) {
							selected = total-1;
							int start = total - MAIN_ROW_COUNT;
							top->start = (start<0) ? 0 : start;
							top->end = total; 
						}
						else if (selected<top->start) {
							top->start -= 1;
							top->end -= 1;
						}
					}
				}
				else if (PAD_justRepeated(BTN_DOWN)) {
					if (selected==total-1 && !PAD_justPressed(BTN_DOWN)) {
						// stop at bottom
					}
					else {
						selected += 1;
						if (selected>=total) {
							selected = 0;
							top->start = 0;
							top->end = (total<MAIN_ROW_COUNT) ? total : MAIN_ROW_COUNT;
						}
						else if (selected>=top->end) {
							top->start += 1;
							top->end += 1;
						}
					}
				}
				if (PAD_justRepeated(BTN_LEFT)) {
					selected -= MAIN_ROW_COUNT;
					if (selected<0) {
						selected = 0;
						top->start = 0;
						top->end = (total<MAIN_ROW_COUNT) ? total : MAIN_ROW_COUNT;
					}
					else if (selected<top->start) {
						top->start -= MAIN_ROW_COUNT;
						if (top->start<0) top->start = 0;
						top->end = top->start + MAIN_ROW_COUNT;
					}
				}
				else if (PAD_justRepeated(BTN_RIGHT)) {
					selected += MAIN_ROW_COUNT;
					if (selected>=total) {
						selected = total-1;
						int start = total - MAIN_ROW_COUNT;
						top->start = (start<0) ? 0 : start;
						top->end = total;
					}
					else if (selected>=top->end) {
						top->end += MAIN_ROW_COUNT;
						if (top->end>total) top->end = total;
						top->start = top->end - MAIN_ROW_COUNT;
					}
				}
			}
		
			if (total==0) {
				// nothing to jump between (and no entry to read alpha from)
			}
			else if (PAD_justRepeated(BTN_L1) && !PAD_isPressed(BTN_R1) && !PWR_ignoreSettingInput(BTN_L1, show_setting)) { // previous alpha
				Entry* entry = top->entries->items[selected];
				int i = entry->alpha-1;
				if (i>=0) {
					selected = top->alphas->items[i];
					showLetter(top->entries->items[selected]);
					if (total>MAIN_ROW_COUNT) {
						top->start = selected;
						top->end = top->start + MAIN_ROW_COUNT;
						if (top->end>total) top->end = total;
						top->start = top->end - MAIN_ROW_COUNT;
					}
				}
			}
			else if (PAD_justRepeated(BTN_R1) && !PAD_isPressed(BTN_L1) && !PWR_ignoreSettingInput(BTN_R1, show_setting)) { // next alpha
				Entry* entry = top->entries->items[selected];
				int i = entry->alpha+1;
				if (i<top->alphas->count) {
					selected = top->alphas->items[i];
					showLetter(top->entries->items[selected]);
					if (total>MAIN_ROW_COUNT) {
						top->start = selected;
						top->end = top->start + MAIN_ROW_COUNT;
						if (top->end>total) top->end = total;
						top->start = top->end - MAIN_ROW_COUNT;
					}
				}
			}
	
			// holding SELECT on a game pins it to (or unpins it from) the game switcher
			static uint32_t select_down_at = 0;
			if (PAD_justPressed(BTN_SELECT)) {
				select_down_at = now;
				select_held = 0;
			}
			if (CAN_SWITCH_VIEW && !select_held && PAD_isPressed(BTN_SELECT) && now-select_down_at>=SWITCHER_HOLD_MS && stack->count>1 && !inSwitcher() && total>0) {
				select_held = 1;
				char pin_path[MAX_PATH];
				if (!getSwitcherPath(top->entries->items[selected], pin_path)) showFlash("Only games can be pinned", 0);
				else if (Switcher_has(pin_path)) {
					Switcher_remove(pin_path);
					showFlash("Unpinned from game switcher", 0);
				}
				else if (Switcher_add(pin_path)==-1) showFlash("Game switcher is full (10)", 0);
				else showFlash("Pinned to game switcher", 0);
				SFX_play(SFX_CONFIRM);
				dirty = 1;
			}

			if (CAN_START && PAD_justPressed(BTN_START) && canCarousel() && total>1) { // game lists only
				int i = rand() % (total-1);
				selected = i>=selected ? i+1 : i; // never the game you're already on
				centerRows(top, selected);
			}
			else if (CAN_SWITCH_VIEW && PAD_justReleased(BTN_SELECT) && !select_held && canCarousel()) { // a tap, holding pins instead
				view_mode = view_mode==VIEW_CAROUSEL ? VIEW_LIST : VIEW_CAROUSEL;
				putInt(VIEW_MODE_PATH, view_mode);
				centerRows(top, selected);
				showFlash(view_mode==VIEW_CAROUSEL ? "Carousel" : "List", 0);
				SFX_play(SFX_CONFIRM);
				dirty = 1;
			}

			if (selected!=top->selected) {
				top->selected = selected;
				dirty = 1;
				SFX_play(SFX_TICK);
			}

			// holding a direction: skip art and resume lookups until it's released
			int nav_buttons = BTN_UP|BTN_DOWN|BTN_LEFT|BTN_RIGHT|BTN_L1|BTN_R1;
			scrolling = PAD_isPressed(nav_buttons) && !PAD_justPressed(nav_buttons);
			if (PAD_justReleased(nav_buttons)) dirty = 1;

			// the save files can't change while minui is running, so only look when the selection does
			static Entry* resume_checked = NULL;
			if (dirty && total>0) {
				Entry* entry = top->entries->items[top->selected];
				if (scrolling) {
					can_resume = 0;
					resume_checked = NULL;
				}
				else if (entry!=resume_checked) {
					readyResume(entry);
					resume_checked = entry;
				}
			}

			if (total>0 && PAD_isPressed(BTN_MENU) && PAD_justPressed(BTN_Y)) {
				gameMenu(screen);
				total = top->entries->count;
				can_resume = 0;
				if (total>0) readyResume(top->entries->items[top->selected]); // a different core has its own save states
				dirty = 1;
			}
			else if (total>0 && PAD_isPressed(BTN_MENU) && PAD_justPressed(BTN_X) && isSortableFolder(top->path)) {
				deleteSelected(screen);
				total = top->entries->count;
				can_resume = 0;
				if (total>0) readyResume(top->entries->items[top->selected]);
				dirty = 1;
			}
			else if (total>0 && inSwitcher() && PAD_justPressed(BTN_X)) {
				// unpin the highlighted game, staying in the switcher
				char pin_path[MAX_PATH];
				if (getSwitcherPath(top->entries->items[top->selected], pin_path)) Switcher_remove(pin_path);
				int at = top->selected;
				EntryArray_free(top->entries);
				Directory* fresh = Directory_new(FAUX_SWITCHER_PATH, 0);
				top->entries = fresh->entries;
				fresh->entries = Array_new(); // moved, not freed with the rest
				Directory_free(fresh);
				top->selected = MIN(at, top->entries->count-1);
				if (top->selected<0) top->selected = 0;
				Directory_reindex(top);
				carousel_slide_from = 0;
				showFlash("Unpinned from game switcher", 0);
				SFX_play(SFX_CONFIRM);
				total = top->entries->count;
				if (total>0) readyResume(top->entries->items[top->selected]);
				dirty = 1;
			}
			else if (total>0 && can_resume && PAD_justReleased(BTN_RESUME) && !inSwitcher()) {
				SFX_play(SFX_CONFIRM);
				should_resume = 1;
				Entry_open(top->entries->items[top->selected]);
				dirty = 1;
			}
			else if (total>0 && PAD_justPressed(BTN_A)) {
				SFX_play(SFX_CONFIRM);
				if (inSwitcher()) { // the switcher picks up from the newer of its quick save and the last manual save
					char preview[256];
					int slot;
					if (getSwitcherSave(top->entries->items[top->selected], preview, &slot)) {
						should_resume = 1;
						resume_slot = slot;
					}
				}
				Entry_open(top->entries->items[top->selected]);
				total = top->entries->count;
				dirty = 1;

				if (total>0) readyResume(top->entries->items[top->selected]);
			}
			else if (PAD_justPressed(BTN_B) && stack->count>1) {
				SFX_play(SFX_BACK);
				int from_most_played = exactMatch(top->path, FAUX_MOSTPLAYED_PATH);
				closeDirectory();
				if (from_most_played && most_played_from_info) {
					most_played_from_info = 0;
					show_version = 1;
					if (!HAS_POWER_BUTTON && !simple_mode) PWR_enableSleep();
				}
				total = top->entries->count;
				dirty = 1;
				// can_resume = 0;
				if (total>0) readyResume(top->entries->items[top->selected]);
			}
			else if (total>1 && (PAD_justPressed(BTN_L2) || PAD_justPressed(BTN_R2)) && !PWR_ignoreSettingInput(PAD_justPressed(BTN_R2) ? BTN_R2 : BTN_L2, show_setting)
					&& (exactMatch(top->path, FAUX_FAVORITES_PATH) || isSortableFolder(top->path))) {
				// stay on the same game in its new place
				char selected_path[MAX_PATH];
				strcpy(selected_path, ((Entry*)top->entries->items[top->selected])->path);
				int step = PAD_justPressed(BTN_R2) ? 1 : -1;
				char* label;
				if (exactMatch(top->path, FAUX_FAVORITES_PATH)) {
					favorites_sort = (favorites_sort + step + FAVORITES_SORT_COUNT) % FAVORITES_SORT_COUNT;
					putInt(FAVORITES_SORT_PATH, favorites_sort);
					EntryArray_free(top->entries);
					top->entries = getFavorites();
					label = favorites_sort_labels[favorites_sort];
				}
				else {
					folder_sort = (folder_sort + step + FOLDER_SORT_COUNT) % FOLDER_SORT_COUNT;
					putInt(FOLDER_SORT_PATH, folder_sort);
					Directory_sort(top);
					label = folder_sort_labels[folder_sort];
				}
				int i = EntryArray_indexOf(top->entries, selected_path);
				top->selected = i>=0 ? i : 0;
				Directory_reindex(top);
				centerRows(top, top->selected);
				
				showFlash(label, 0);
				SFX_play(SFX_CONFIRM);
				total = top->entries->count;
				dirty = 1;
			}
			else if (total>0 && PAD_justPressed(BTN_Y)) {
				SFX_play(SFX_CONFIRM);
				toggleFavorite();
				total = top->entries->count;
				dirty = 1;
				can_resume = 0;
				if (total>0) readyResume(top->entries->items[top->selected]);
			}
		}
		
		if (flash_until && SDL_GetTicks()>=flash_until) {
			flash_until = 0;
			dirty = 1;
		}
		else if (flash_until && flash_until!=FLASH_WAITING && (int32_t)(flash_until - SDL_GetTicks())<FLASH_FADE_MS) dirty = 1; // fading out
		if (carousel_slide_from) {
			if (!Carousel_shift(now)) carousel_slide_from = 0; // one last still frame
			dirty = 1;
		}
		if (!dirty && !show_version && Decoder_hasDone()) dirty = 1; // art the thread just finished, to put on screen
		if (marquee_active && now-marquee_drawn_at>=MARQUEE_FRAME_MS && !dirty) dirty = MARQUEE_ONLY;
		
		if (dirty==MARQUEE_ONLY && marquee_base && marquee_text && !flash_until) {
			// just the scrolling name moved
			SDL_BlitSurface(marquee_base, NULL, screen, NULL);
			blitMarquee(screen, now);
			marquee_drawn_at = now;
			GFX_flip(screen);
			dirty = 0;
		}
		else if (dirty) {
			frame_draw_started_at = SDL_GetTicks();
			GFX_clear(screen);
			marquee_active = 0; // set again if this frame scrolls a name
			marquee_drawn_at = now;
			if (marquee_text) SDL_FreeSurface(marquee_text);
			marquee_text = NULL;
			
			int ox;
			int oy;
			
			// simple thumbnail support a thumbnail for a file or folder named NAME.EXT needs a corresponding /.res/NAME.EXT.png
			// that is no bigger than platform FIXED_HEIGHT x FIXED_HEIGHT
			int had_thumb = 0;
			int carousel = showsCarousel() && total>0;
			if (!show_version && total>0 && !carousel) {
				// from the thread while scrolling (shown as soon as it's ready), waited for once the list is still
				requestThumbs(top, screen);
				Entry* entry = top->entries->items[top->selected];
				SDL_Surface* art = getThumb(entry, screen, !scrolling);
				static int last_art_x = 0; // where the last art started, so rows keep their width while the next is on its way
				if (art) {
					had_thumb = 1;
					ox = MAX(FIXED_WIDTH - FIXED_HEIGHT, (FIXED_WIDTH - art->w));
					oy = (FIXED_HEIGHT - art->h) / 2;
					SDL_BlitSurface(art, NULL, screen, &(SDL_Rect){ox,oy});
					last_art_x = ox;
				}
				else if (!hasThumb(entry) && last_art_x) { // still decoding (not missing): no jump in the names
					had_thumb = 1;
					ox = last_art_x;
				}
				else last_art_x = 0;
			}
			
			int ow = GFX_blitHardwareGroup(screen, show_setting);
			
			if (show_version) {
				if (!version) {
					char release[256];
					getFile(ROOT_SYSTEM_PATH "/version.txt", release, 256);
					
					char *tmp,*commit;
					commit = strrchr(release, '\n');
					commit[0] = '\0';
					commit = strrchr(release, '\n')+1;
					tmp = strchr(release, '\n');
					tmp[0] = '\0';
					
					
					// minui restarts after every game so reading play time once here stays current
					char played[32] = "None yet";
					char top_path[MAX_PATH];
					int total_seconds, top_seconds;
					if (getPlayTimeTotals(&total_seconds, top_path, &top_seconds)) {
						formatPlayTime(total_seconds, played);
						if (top_path[0]) getDisplayName(top_path, info_top_name);
					}
					
					// TODO: not sure if I want bare PLAT_* calls here
					// the commit in version.txt is the stock release's, not this build's, so it's left out
					char* keys[] = { "Play time" }; // release and model left off to make room for the settings rows
					char* values[] = { played };
					int line_count = 1;
					
					int l_width = 0;
					for (int i=0; i<line_count; i++) {
						int w;
						TTF_SizeUTF8(font.large, keys[i], &w, NULL);
						if (w>l_width) l_width = w;
					}
					int x = l_width + SCALE1(8);
					int max_r_width = screen->w - SCALE1(PADDING*2 + BUTTON_PADDING*2) - x;
					
					SDL_Surface* key_txts[4];
					SDL_Surface* val_txts[4];
					int r_width = 0;
					for (int i=0; i<line_count; i++) {
						char value[256];
						GFX_truncateText(font.large, values[i], value, max_r_width, 0);
						key_txts[i] = TTF_RenderUTF8_Blended(font.large, keys[i], COLOR_DARK_TEXT);
						val_txts[i] = TTF_RenderUTF8_Blended(font.large, value, COLOR_WHITE);
						if (val_txts[i]->w>r_width) r_width = val_txts[i]->w;
					}
					
					#define VERSION_LINE_HEIGHT 18 // two lines plus four rows fill the space between the top and bottom bars
					int w = x + r_width;
					int h = SCALE1(VERSION_LINE_HEIGHT*(line_count-1)) + val_txts[line_count-1]->h; // room for the last line's descenders
					version = SDL_CreateRGBSurface(0,w,h,16,0,0,0,0);
					for (int i=0; i<line_count; i++) {
						SDL_BlitSurface(key_txts[i], NULL, version, &(SDL_Rect){0,SCALE1(VERSION_LINE_HEIGHT*i)});
						SDL_BlitSurface(val_txts[i], NULL, version, &(SDL_Rect){x,SCALE1(VERSION_LINE_HEIGHT*i)});
						SDL_FreeSurface(key_txts[i]);
						SDL_FreeSurface(val_txts[i]);
					}
				}
				// most played and the power timers sit under the version info, the whole block centered
				int rows_w = MAX(version->w, SCALE1(260));
				int rows_gap = SCALE1(4);
				if (info_row<info_start) info_start = info_row;
				if (info_row>=info_start+INFO_ROWS_SHOWN) info_start = info_row - INFO_ROWS_SHOWN + 1;
				int head_h = version->h;
				int rows_h = rows_gap + SCALE1(PILL_SIZE) * INFO_ROWS_SHOWN;
				int top = SCALE1(PADDING); // the hardware group is on the right, the text starts left of it
				int bottom = screen->h - SCALE1(PADDING + PILL_SIZE + 6); // clear of the button hints
				int block_y = top + (bottom - top - head_h - rows_h) / 2;
				int block_x = (screen->w - rows_w) / 2;
				SDL_BlitSurface(version, NULL, screen, &(SDL_Rect){block_x, block_y});

				for (int i=info_start; i<INFO_ROW_COUNT && i<info_start+INFO_ROWS_SHOWN; i++) {
					int y = block_y + head_h + rows_gap + SCALE1(PILL_SIZE) * (i - info_start);
					int selected = i==info_row;
					if (selected) {
						GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){
							block_x - SCALE1(BUTTON_PADDING), y,
							rows_w + SCALE1(BUTTON_PADDING*2), SCALE1(PILL_SIZE)
						});
					}
					char* name;
					char value[256];
					if (i==INFO_ROW_MOST_PLAYED) {
						name = "Most played";
						int name_w;
						TTF_SizeUTF8(font.large, name, &name_w, NULL);
						char* shown = clear_armed_at ? "Press X again" : (info_top_name[0] ? info_top_name : "None yet");
						GFX_truncateText(font.large, shown, value, rows_w - name_w - SCALE1(BUTTON_PADDING*2), 0);
					}
					else if (i==INFO_ROW_THEME) {
						name = "Theme";
						strcpy(value, GFX_getThemeName(GFX_getTheme()));
					}
					else if (i==INFO_ROW_SOUNDS) {
						name = "Menu sounds";
						strcpy(value, SFX_menuSoundsEnabled() ? "On" : "Off");
					}
					else if (i==INFO_ROW_MUSIC) {
						name = "Music";
						if (BGM_isEnabled()) sprintf(value, "%i%%", BGM_getVolume() * 100 / BGM_VOLUME_MAX);
						else strcpy(value, "Off");
					}
					else {
						char* timer_names[PWR_TIMER_COUNT] = { "Sleep after", "Power off after" };
						name = timer_names[i-1];
						strcpy(value, PWR_getTimerLabel(i-1, PWR_getTimer(i-1)));
					}
					SDL_Surface* name_txt = TTF_RenderUTF8_Blended(font.large, name, selected ? COLOR_BLACK : COLOR_DARK_TEXT);
					SDL_Surface* value_txt = TTF_RenderUTF8_Blended(font.large, value, selected ? COLOR_BLACK : COLOR_WHITE);
					int ty = y + (SCALE1(PILL_SIZE) - name_txt->h) / 2;
					SDL_BlitSurface(name_txt, NULL, screen, &(SDL_Rect){block_x, ty});
					SDL_BlitSurface(value_txt, NULL, screen, &(SDL_Rect){block_x + rows_w - value_txt->w, ty});
					SDL_FreeSurface(name_txt);
					SDL_FreeSurface(value_txt);
				}

				// more rows below (no room for a mark above, under the play time)
				int rows_top = block_y + head_h + rows_gap;
				if (info_start+INFO_ROWS_SHOWN<INFO_ROW_COUNT) SDL_FillRect(screen, &(SDL_Rect){ screen->w/2 - SCALE1(8), rows_top + SCALE1(PILL_SIZE) * INFO_ROWS_SHOWN + SCALE1(1), SCALE1(16), SCALE1(2) }, RGB_GRAY);

				// buttons (duped and trimmed from below)
				int right_w = 0;
				if (info_row==INFO_ROW_MOST_PLAYED && info_top_name[0]) right_w = GFX_blitButtonGroup((char*[]){ "B","BACK", "A","VIEW", NULL }, 1, screen, 1);
				else if (info_row==INFO_ROW_MUSIC) right_w = GFX_blitButtonGroup((char*[]){ "B","BACK", "A",BGM_isEnabled() ? "OFF" : "ON", NULL }, 1, screen, 1);
				else right_w = GFX_blitButtonGroup((char*[]){ "B","BACK",  NULL }, 0, screen, 1);

				if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
				else if (info_row==INFO_ROW_MUSIC) { // where the songs go, in place of the left hints
					int songs = BGM_countSongs();
					char hint[64], shown[64];
					if (songs) snprintf(hint, sizeof(hint), "%i song%s in /BGM", songs, songs==1 ? "" : "s");
					else strcpy(hint, "Put songs in /BGM");
					int pill_w = screen->w - SCALE1(PADDING*3) - right_w;
					GFX_truncateText(font.small, hint, shown, pill_w - SCALE1(BUTTON_PADDING*2), 0);
					SDL_Surface* text = TTF_RenderUTF8_Blended(font.small, shown, COLOR_WHITE);
					int pill_y = screen->h - SCALE1(PADDING + PILL_SIZE);
					GFX_blitPill(ASSET_DARK_GRAY_PILL, screen, &(SDL_Rect){ SCALE1(PADDING), pill_y, text->w + SCALE1(BUTTON_PADDING*2), SCALE1(PILL_SIZE) });
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){ SCALE1(PADDING + BUTTON_PADDING), pill_y + (SCALE1(PILL_SIZE) - text->h) / 2 });
					SDL_FreeSurface(text);
				}
				else if (info_row==INFO_ROW_MOST_PLAYED && info_top_name[0]) GFX_blitButtonGroup((char*[]){ "X","CLEAR", NULL }, 0, screen, 0);
				else GFX_blitButtonGroup((char*[]){ BTN_SLEEP==BTN_POWER?"POWER":"MENU","SLEEP",  NULL }, 0, screen, 0);

			}
			else {
				// list
				if (carousel) {
					// passing through: only art that's already decoded
					if (Carousel_useGPU()) blitCarouselGPU(screen, top, CAROUSEL_SCROLL_DECODES, Carousel_shift(now)); // the selected art now, the rest from the thread
					else blitCarousel(screen, top, !scrolling, Carousel_shift(now));
				}
				else if (total>0) {
					int selected_row = top->selected - top->start;
					for (int i=top->start,j=0; i<top->end; i++,j++) {
						Entry* entry = top->entries->items[i];
						char* entry_name = entry->name;
						char* entry_unique = entry->unique;
						int available_width = (had_thumb ? ox : screen->w) - SCALE1(PADDING * 2); // every row stays clear of the art
						if (i==top->start && !had_thumb) available_width -= ow; // 
					
						SDL_Color text_color = COLOR_WHITE;
					
						trimSortingMeta(&entry_name);

						// reserve room so truncation never hides the dot
						int dot_width = 0;
						if (entry->favorite) TTF_SizeUTF8(font.large, FAVORITE_DOT, &dot_width, NULL);

						char display_name[256];
						int text_width = fitText(font.large, entry_unique ? entry_unique : entry_name, display_name, available_width-dot_width, SCALE1(BUTTON_PADDING*2));
						if (entry->favorite) {
							strcat(display_name, FAVORITE_DOT);
							text_width += dot_width;
						}
						int max_width = MIN(available_width, text_width);
						if (j==selected_row && had_thumb) {
							// too long to fit beside the art: show it all by scrolling within the space
							char full_name[256];
							snprintf(full_name, sizeof(full_name), "%s%s", entry_unique ? entry_unique : entry_name, entry->favorite ? FAVORITE_DOT : "");
							int full_width;
							TTF_SizeUTF8(font.large, full_name, &full_width, NULL);
							int room = available_width - SCALE1(BUTTON_PADDING*2);
							if (full_width>room) {
								GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){
									SCALE1(PADDING),
									SCALE1(PADDING+(j*PILL_SIZE)),
									available_width,
									SCALE1(PILL_SIZE)
								});
								// drawn last, once the rest of the frame is kept for the frames that only move it
								if (marquee_text) SDL_FreeSurface(marquee_text);
								marquee_text = TTF_RenderUTF8_Blended(font.large, full_name, COLOR_BLACK);
								marquee_dst = (SDL_Rect){ SCALE1(PADDING+BUTTON_PADDING), SCALE1(PADDING+(j*PILL_SIZE)+4), room, 0 };
								marquee_overflow = full_width - room;
								marqueeOffset(entry->path, marquee_overflow, now); // starts it over for a new name
								continue;
							}
						}
						if (j==selected_row) {
							GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){
								SCALE1(PADDING),
								SCALE1(PADDING+(j*PILL_SIZE)),
								max_width,
								SCALE1(PILL_SIZE)
							});
							text_color = COLOR_BLACK;
						}
						else if (entry->unique) {
							trimSortingMeta(&entry_unique);
							char unique_name[256];
							fitText(font.large, entry_unique, unique_name, available_width, SCALE1(BUTTON_PADDING*2));
						
							SDL_Surface* text = renderText(font.large, unique_name, COLOR_DARK_TEXT);
							if (text) SDL_BlitSurface(text, &(SDL_Rect){
								0,
								0,
								max_width-SCALE1(BUTTON_PADDING*2),
								text->h
							}, screen, &(SDL_Rect){
								SCALE1(PADDING+BUTTON_PADDING),
								SCALE1(PADDING+(j*PILL_SIZE)+4)
							});

							fitText(font.large, entry_name, display_name, available_width-dot_width, SCALE1(BUTTON_PADDING*2));
							if (entry->favorite) strcat(display_name, FAVORITE_DOT);
						}
						SDL_Surface* text = renderText(font.large, display_name, text_color);
						if (text) SDL_BlitSurface(text, &(SDL_Rect){
							0,
							0,
							max_width-SCALE1(BUTTON_PADDING*2),
							text->h
						}, screen, &(SDL_Rect){
							SCALE1(PADDING+BUTTON_PADDING),
							SCALE1(PADDING+(j*PILL_SIZE)+4)
						});
					}
				}
				else {
					// TODO: for some reason screen's dimensions end up being 0x0 in GFX_blitMessage...
					if (inSwitcher()) GFX_blitMessage(font.small, "No games pinned yet\n\nHold SELECT on a game in a list,\nor press X in a game's menu", screen, &(SDL_Rect){0,0,screen->w,screen->h});
					else GFX_blitMessage(font.large, "Empty folder", screen, &(SDL_Rect){0,0,screen->w,screen->h}); //, NULL);
				}
			
				flash_gpu = carousel && Carousel_useGPU(); // over the carousel's art, which the GPU draws after the menu
				blitFlash(screen);
				if (flash_gpu && (show_setting || GFX_settingFading()) && !GetHDMI()) queueHardwareGroup(screen, ow); // brightness/volume on top too
				
				// buttons
				char fav_path[256];
				char* fav_button = (total>0 && getFavoritePath(top->entries->items[top->selected], fav_path)) ? "Y" : NULL; // NULL ends the group early
				if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
				else if (inSwitcher()) GFX_blitButtonGroup((char*[]){ total>0 ? "X" : NULL,"REMOVE", NULL }, 0, screen, 0);
				else if (total>0 && exactMatch(top->path, FAUX_MOSTPLAYED_PATH)) { // stats matter more than resume/fav hints here
					Entry* entry = top->entries->items[top->selected];
					char total_str[32], average_str[32];
					formatShortPlayTime(entry->play.seconds, total_str);
					formatShortPlayTime(getAveragePlayTime(&entry->play), average_str);
					GFX_blitButtonGroup((char*[]){ total_str,"ALL", average_str,"AVG", NULL }, 0, screen, 0);
				}
				else if (can_resume) GFX_blitButtonGroup((char*[]){ "X","RESUME", fav_button,"FAV", NULL }, 0, screen, 0);
				else GFX_blitButtonGroup((char*[]){
					BTN_SLEEP==BTN_POWER?"POWER":"MENU",
					BTN_SLEEP==BTN_POWER||simple_mode?"SLEEP":"INFO",
					fav_button,"FAV",
					NULL }, 0, screen, 0);
			
				if (total==0) {
					if (stack->count>1) {
						GFX_blitButtonGroup((char*[]){ "B","BACK",  NULL }, 0, screen, 1);
					}
				}
				else {
					if (stack->count>1) {
						char preview[256];
						int slot;
						int resumable = inSwitcher() && getSwitcherSave(top->entries->items[top->selected], preview, &slot);
						GFX_blitButtonGroup((char*[]){ "B","BACK", "A",resumable ? "RESUME" : "OPEN", NULL }, 1, screen, 1);
					}
					else {
						GFX_blitButtonGroup((char*[]){ "A","OPEN", NULL }, 0, screen, 1);
					}
				}
			}

			if (marquee_text) {
				// keep everything but the scrolling name for the frames that only move it
				if (!marquee_base || marquee_base->w!=screen->w || marquee_base->h!=screen->h) {
					if (marquee_base) SDL_FreeSurface(marquee_base);
					marquee_base = SDL_CreateRGBSurface(0, screen->w, screen->h, screen->format->BitsPerPixel, screen->format->Rmask, screen->format->Gmask, screen->format->Bmask, screen->format->Amask);
				}
				if (marquee_base) SDL_BlitSurface(screen, NULL, marquee_base, NULL);
				blitMarquee(screen, now);
				if (flash_until && !flash_gpu && !show_version) blitFlash(screen); // a popup stays over the scrolling name
			}

			frame_drawn_at = SDL_GetTicks();
			GFX_flip(screen);
			frame_flipped_at = SDL_GetTicks();
			dirty = 0;
			static int first_frame = 1;
			if (first_frame) {
				first_frame = 0;
				LOG_info("first frame: %llums after start%s\n", (unsigned long long)(getMicroseconds()-main_begin)/1000, fresh_boot ? " (fresh boot)" : "");
			}
		}
		else {
			// idle: get the art for the next move ready (one piece a frame), otherwise just wait for the next frame
			int idle = total>0 && !show_version && !carousel_slide_from && !PAD_isPressed(BTN_ANY|BTN_MENU);
			int worked = 0;
			if (idle && showsCarousel()) worked = Carousel_useGPU() ? Carousel_preloadGPU(top, screen) : Carousel_preload(top, screen);
			else if (idle) worked = preloadThumbs(top, screen);
			if (!worked) GFX_sync();
		}
		
		// if (!first_draw) {
		// 	first_draw = SDL_GetTicks();
		// 	LOG_info("- first draw: %lu\n", first_draw - main_begin);
		// }
		
		// handle HDMI change
		static int had_hdmi = -1;
		int has_hdmi = GetHDMI();
		if (had_hdmi==-1) had_hdmi = has_hdmi;
		if (has_hdmi!=had_hdmi) {
			had_hdmi = has_hdmi;

			Entry* entry = top->entries->items[top->selected];
			LOG_info("restarting after HDMI change... (%s)\n", entry->path);
			saveLast(entry->path); // NOTE: doesn't work in Recents (by design)
			sleep(4);
			quit = 1;
		}
	}
	
	if (version) SDL_FreeSurface(version);
	freeThumbs();
	freeTextCache();
	if (marquee_base) SDL_FreeSurface(marquee_base);
	if (marquee_text) SDL_FreeSurface(marquee_text);
	Decoder_quit(); // before the art's surfaces and the screen go
	Carousel_quit();
	Carousel_freeGPU();
	PLAT_destroyTexture(flash_texture);
	if (flash_font) TTF_CloseFont(flash_font);

	BGM_quit(); // a quick fade, before the device closes
	SFX_quit(); // lets the confirm sound for a launched game finish
	Menu_quit();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	QuitSettings();
}