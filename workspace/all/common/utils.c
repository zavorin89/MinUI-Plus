#define _GNU_SOURCE // for strcasestr
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <ctype.h>
#include <sys/time.h>
#include "defines.h"
#include "utils.h"

///////////////////////////////////////

int prefixMatch(char* pre, char* str) {
	return (strncasecmp(pre,str,strlen(pre))==0);
}
int suffixMatch(char* suf, char* str) {
	int len = strlen(suf);
	int offset = strlen(str)-len;
	return (offset>=0 && strncasecmp(suf, str+offset, len)==0);
}
int exactMatch(char* str1, char* str2) {
	if (!str1 || !str2) return 0; // NULL isn't safe here
	int len1 = strlen(str1);
	if (len1!=strlen(str2)) return 0;
	return (strncmp(str1,str2,len1)==0);
}
int containsString(char* haystack, char* needle) {
	return strcasestr(haystack, needle) != NULL;
}
int hide(char* file_name) {
	return file_name[0]=='.' || suffixMatch(".disabled", file_name) || exactMatch("map.txt", file_name);
}

// both write at most NAME_MAX+1 (256) bytes to out_name: the work is done on the whole path in a buffer of its own,
// so a path longer than a name (up to MAX_PATH) can't run past the caller's
#define NAME_OUT 256
void getDisplayName(const char* in_name, char* out_name) {
	char* tmp;
	char work_name[MAX_PATH];
	snprintf(work_name, sizeof(work_name), "%s", in_name);
	
	if (suffixMatch("/" PLATFORM, work_name)) { // hide platform from Tools path...
		tmp = strrchr(work_name, '/');
		tmp[0] = '\0';
	}
	
	// extract just the filename if necessary
	tmp = strrchr(work_name, '/');
	snprintf(out_name, NAME_OUT, "%s", tmp ? tmp+1 : work_name);
	
	// remove extension(s), eg. .p8.png
	while ((tmp = strrchr(out_name, '.'))!=NULL) {
		int len = strlen(tmp);
		if ((len>2 && len<=5) || !strcmp(tmp, ".splore")) tmp[0] = '\0'; // 1-4 letter extension plus dot (was 1-3, extended for .doom files), and PICO-8's Splore
		else break;
	}
	
	// remove trailing parens (round and square)
	strcpy(work_name, out_name);
	while ((tmp=strrchr(out_name, '('))!=NULL || (tmp=strrchr(out_name, '['))!=NULL) {
		if (tmp==out_name) break;
		tmp[0] = '\0';
		tmp = out_name;
	}
	
	// make sure we haven't nuked the entire name
	if (out_name[0]=='\0') strcpy(out_name, work_name);
	
	// remove trailing whitespace
	tmp = out_name + strlen(out_name) - 1;
    while(tmp>out_name && isspace((unsigned char)*tmp)) tmp--;
    tmp[1] = '\0';
}
void getEmuName(const char* in_name, char* out_name) {
	char work[MAX_PATH];
	snprintf(work, sizeof(work), "%s", in_name);
	char* tmp = work;
	
	// extract just the Roms folder name if necessary
	if (prefixMatch(ROMS_PATH "/", tmp)) {
		tmp += strlen(ROMS_PATH) + 1;
		char* slash = strchr(tmp, '/');
		if (slash) slash[0] = '\0';
	}

	// finally extract pak name from parenths if present
	char* paren = strrchr(tmp, '(');
	if (paren) {
		tmp = paren + 1;
		char* close = strchr(tmp, ')');
		if (close) close[0] = '\0';
	}
	snprintf(out_name, NAME_OUT, "%s", tmp);
}
void getEmuPath(char* emu_name, char* pak_path) {
	sprintf(pak_path, "%s/Emus/%s/%s.pak/launch.sh", SDCARD_PATH, PLATFORM, emu_name);
	if (exists(pak_path)) return;
	sprintf(pak_path, "%s/Emus/%s.pak/launch.sh", PAKS_PATH, emu_name);
}

void normalizeNewline(char* line) {
	int len = strlen(line);
	if (len>1 && line[len-1]=='\n' && line[len-2]=='\r') { // windows!
		line[len-2] = '\n';
		line[len-1] = '\0';
	}
}
void trimTrailingNewlines(char* line) {
	int len = strlen(line);
	while (len>0 && line[len-1]=='\n') {
		line[len-1] = '\0'; // trim newline
		len -= 1;
	}
}
void trimSortingMeta(char** str) { // eg. `001) `
	// TODO: this code is suss
	char* safe = *str;
	while(isdigit(**str)) *str += 1; // ignore leading numbers

	if (*str[0]==')') { // then match a closing parenthesis
		*str += 1;
	}
	else { //  or bail, restoring the string to its original value
		*str = safe;
		return;
	}
	
	while(isblank(**str)) *str += 1; // ignore leading space
}

///////////////////////////////////////

int exists(char* path) {
	return access(path, F_OK)==0;
}
void touch(char* path) {
	close(open(path, O_RDWR|O_CREAT, 0777));
}
void putFile(char* path, char* contents) {
	FILE* file = fopen(path, "w");
	if (file) {
		fputs(contents, file);
		fclose(file);
	}
}
void getFile(char* path, char* buffer, size_t buffer_size) {
	FILE *file = fopen(path, "r");
	if (file) {
		fseek(file, 0L, SEEK_END);
		size_t size = ftell(file);
		if (size>buffer_size-1) size = buffer_size - 1;
		rewind(file);
		fread(buffer, sizeof(char), size, file);
		fclose(file);
		buffer[size] = '\0';
	}
}
char* allocFile(char* path) { // caller must free!
	char* contents = NULL;
	FILE *file = fopen(path, "r");
	if (file) {
		fseek(file, 0L, SEEK_END);
		size_t size = ftell(file);
		contents = calloc(size+1, sizeof(char));
		fseek(file, 0L, SEEK_SET);
		fread(contents, sizeof(char), size, file);
		fclose(file);
		contents[size] = '\0';
	}
	return contents;
}
int getInt(char* path) {
	int i = 0;
	FILE *file = fopen(path, "r");
	if (file!=NULL) {
		fscanf(file, "%i", &i);
		fclose(file);
	}
	return i;
}
void putInt(char* path, int value) {
	char buffer[8];
	sprintf(buffer, "%d", value);
	putFile(path, buffer);
}

uint64_t getMicroseconds(void) {
    uint64_t ret;
    struct timeval tv;

    gettimeofday(&tv, NULL);

    ret = (uint64_t)tv.tv_sec * 1000000;
    ret += (uint64_t)tv.tv_usec;

    return ret;
}
///////////////////////////////////////

// play time, one line per game with paths relative to SDCARD_PATH:
// "<path>\t<seconds>\t<sessions>\t<last played>\t<last session seconds>"
// (older lines without the last two fields read as 0)

int parsePlayTimeLine(char* line, char** path, PlayTime* out) {
	normalizeNewline(line);
	trimTrailingNewlines(line);
	char* tab = strchr(line, '\t');
	if (!tab) return 0;
	*tab = '\0';
	*path = line;
	memset(out, 0, sizeof(PlayTime));
	sscanf(tab+1, "%d\t%d\t%d\t%d", &out->seconds, &out->sessions, &out->last_played, &out->last_session);
	return 1;
}
int getPlayTime(char* path, PlayTime* out) {
	memset(out, 0, sizeof(PlayTime));
	FILE* file = fopen(PLAYTIME_PATH, "r");
	if (!file) return 0;
	
	int found = 0;
	char line[MAX_PATH+64]; // a path, then its numbers
	while (fgets(line, sizeof(line), file)) {
		char* line_path;
		PlayTime line_time;
		if (!parsePlayTimeLine(line, &line_path, &line_time)) continue;
		if (!exactMatch(line_path, path)) continue;
		*out = line_time;
		found = 1;
		break;
	}
	fclose(file);
	return found;
}
static void writePlayTimeLine(FILE* file, char* path, PlayTime* time) {
	fprintf(file, "%s\t%i\t%i\t%i\t%i\n", path, time->seconds, time->sessions, time->last_played, time->last_session);
}
void setPlayTime(char* path, PlayTime* time) {
	// written to a temp file then renamed so losing power mid-write can't wipe every game's stats
	char tmp_path[MAX_PATH];
	sprintf(tmp_path, "%s.tmp", PLAYTIME_PATH);
	FILE* out = fopen(tmp_path, "w");
	if (!out) return;
	
	FILE* in = fopen(PLAYTIME_PATH, "r");
	if (in) {
		char line[MAX_PATH+64];
		while (fgets(line, sizeof(line), in)) {
			char* line_path;
			PlayTime line_time;
			if (!parsePlayTimeLine(line, &line_path, &line_time)) continue;
			if (exactMatch(line_path, path)) continue; // replaced below
			writePlayTimeLine(out, line_path, &line_time);
		}
		fclose(in);
	}
	if (time) writePlayTimeLine(out, path, time);
	fclose(out);
	
	rename(tmp_path, PLAYTIME_PATH);
	sync();
}
void removePlayTime(char* path) {
	setPlayTime(path, NULL);
}
int getPlayTimeTotals(int* total_seconds, char* top_path, int* top_seconds) {
	*total_seconds = 0;
	*top_seconds = 0;
	top_path[0] = '\0';
	FILE* file = fopen(PLAYTIME_PATH, "r");
	if (!file) return 0;
	
	int count = 0;
	char line[MAX_PATH+64];
	while (fgets(line, sizeof(line), file)) {
		char* line_path;
		PlayTime line_time;
		if (!parsePlayTimeLine(line, &line_path, &line_time)) continue;
		*total_seconds += line_time.seconds;
		if (line_time.seconds>*top_seconds) {
			*top_seconds = line_time.seconds;
			strcpy(top_path, line_path);
		}
		count += 1;
	}
	fclose(file);
	return count;
}
int getAveragePlayTime(PlayTime* time) {
	return time->sessions>0 ? time->seconds / time->sessions : time->seconds;
}
void formatPlayTime(int seconds, char* out) {
	int minutes = seconds / 60;
	int hours = minutes / 60;
	minutes %= 60;
	if (hours) sprintf(out, "%ih %im", hours, minutes);
	else if (seconds>=60) sprintf(out, "%im", minutes);
	else strcpy(out, "<1m");
}

///////////////////////////////

int Switcher_load(char paths[SWITCHER_MAX][MAX_PATH]) {
	FILE* file = fopen(SWITCHER_PATH, "r");
	if (!file) return 0;
	int count = 0;
	char line[MAX_PATH];
	while (count<SWITCHER_MAX && fgets(line, sizeof(line), file)) {
		normalizeNewline(line);
		trimTrailingNewlines(line);
		if (!line[0]) continue;
		strcpy(paths[count++], line);
	}
	fclose(file);
	return count;
}
static void Switcher_save(char paths[SWITCHER_MAX][MAX_PATH], int count) {
	FILE* file = fopen(SWITCHER_PATH, "w");
	if (!file) return;
	for (int i=0; i<count; i++) fprintf(file, "%s\n", paths[i]);
	fclose(file);
	sync();
}
int Switcher_has(char* path) {
	char paths[SWITCHER_MAX][MAX_PATH];
	int count = Switcher_load(paths);
	for (int i=0; i<count; i++) if (exactMatch(paths[i], path)) return 1;
	return 0;
}
int Switcher_add(char* path) {
	char paths[SWITCHER_MAX][MAX_PATH];
	int count = Switcher_load(paths);
	for (int i=0; i<count; i++) if (exactMatch(paths[i], path)) return 0;
	if (count==SWITCHER_MAX) return -1;
	strcpy(paths[count++], path);
	Switcher_save(paths, count);
	return 1;
}
void Switcher_remove(char* path) {
	char paths[SWITCHER_MAX][MAX_PATH];
	int count = Switcher_load(paths);
	int kept = 0;
	for (int i=0; i<count; i++) {
		if (exactMatch(paths[i], path)) continue;
		if (kept!=i) strcpy(paths[kept], paths[i]);
		kept += 1;
	}
	Switcher_save(paths, kept);
}
