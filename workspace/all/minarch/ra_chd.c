// CHD support for rcheevos' disc hashing (eg. PlayStation games, which MinUI keeps as .chd).
// rcheevos asks for a track and then reads its sectors' user data (2048 bytes each, back to back);
// this answers from the CHD's hunks, and hands .cue/.bin/.iso to rcheevos' own reader.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rc_hash.h"
#include "libchdr/chd.h"
#include "libchdr/cdrom.h"

#include "ra_chd.h"

#define CHD_FRAME_SIZE (CD_MAX_SECTOR_DATA + CD_MAX_SUBCODE_DATA) // each CD frame in a CHD hunk: 2352 + 96

typedef struct ChdTrack {
	chd_file* chd;
	uint32_t chd_first_frame; // where the track starts among the CHD's frames
	uint32_t first_sector; // the disc sector the track starts at
	uint32_t pregap; // sectors of pregap stored at the start of the track
	uint32_t frames; // in the track
	int data_offset; // where the 2048 bytes of user data sit in a 2352 byte frame
	int data_size;
	uint8_t* hunk; // the decoded hunk currently held
	int hunk_index;
	uint32_t frames_per_hunk;
} ChdTrack;

typedef struct Handle {
	int is_chd;
	void* inner; // ChdTrack, or rcheevos' own track handle
} Handle;

static struct rc_hash_cdreader fallback; // rcheevos' reader, for everything that isn't a CHD

static int isChd(const char* path) {
	size_t len = strlen(path);
	return len>4 && !strcasecmp(path + len - 4, ".chd");
}

// where the user data is in a raw frame for a CHD track type
static void trackLayout(const char* type, int* offset, int* size) {
	*size = 2048;
	if (!strcmp(type, "MODE2_RAW")) *offset = 24; // sync, header, subheader
	else if (!strcmp(type, "MODE1_RAW")) *offset = 16; // sync, header
	else if (!strcmp(type, "MODE2_FORM1")) *offset = 0;
	else if (!strcmp(type, "MODE2") || !strcmp(type, "MODE2_FORM_MIX")) *offset = 8; // subheader
	else if (!strcmp(type, "AUDIO")) { *offset = 0; *size = 2352; }
	else *offset = 0; // MODE1: cooked 2048
}

static ChdTrack* openChdTrack(const char* path, uint32_t want) {
	chd_file* chd = NULL;
	chd_error err = chd_open(path, CHD_OPEN_READ, NULL, &chd);
	if (err!=CHDERR_NONE) {
		fprintf(stderr, "[INFO] ra_chd: couldn't open %s: %s (%i)\n", path, chd_error_string(err), err);
		return NULL;
	}
	const chd_header* header = chd_get_header(chd);

	// walk the track list: CHD frames are padded to a multiple of 4 per track, disc sectors aren't
	struct { int number; char type[32]; uint32_t frames, pregap, chd_first, first_sector; int pregap_stored; } tracks[99];
	int count = 0;
	uint32_t chd_frame = 0, sector = 0;
	for (int i=0; i<99; i++) {
		char meta[256] = "";
		uint32_t got = 0;
		int number = 0, frames = 0, pregap = 0, postgap = 0;
		char type[32] = "", subtype[32] = "", pgtype[32] = "", pgsub[32] = "";
		if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, i, meta, sizeof(meta)-1, &got, NULL, NULL)==CHDERR_NONE) {
			meta[got] = '\0';
			if (sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &number, type, subtype, &frames, &pregap, pgtype, pgsub, &postgap)<4) break;
		}
		else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, i, meta, sizeof(meta)-1, &got, NULL, NULL)==CHDERR_NONE) {
			meta[got] = '\0';
			if (sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &number, type, subtype, &frames)<4) break;
		}
		else break;
		tracks[count].number = number;
		snprintf(tracks[count].type, sizeof(tracks[count].type), "%s", type);
		tracks[count].frames = frames;
		tracks[count].pregap_stored = pgtype[0]=='V'; // the pregap's frames are in the CHD, at the track's start
		tracks[count].pregap = tracks[count].pregap_stored ? pregap : 0;
		tracks[count].chd_first = chd_frame;
		tracks[count].first_sector = sector;
		chd_frame += (frames + 3) / 4 * 4;
		sector += frames;
		count += 1;
	}

	int pick = -1;
	if (want==RC_HASH_CDTRACK_FIRST_DATA || want==0) {
		for (int i=0; i<count && pick<0; i++) if (strcmp(tracks[i].type, "AUDIO")) pick = i;
	}
	else if (want==RC_HASH_CDTRACK_LAST) pick = count - 1;
	else if (want==RC_HASH_CDTRACK_LARGEST) {
		for (int i=0; i<count; i++) if (pick<0 || tracks[i].frames>tracks[pick].frames) pick = i;
	}
	else if (want==RC_HASH_CDTRACK_FIRST_OF_SECOND_SESSION) pick = -1; // CHDs don't keep sessions (Saturn/Dreamcast only)
	else {
		for (int i=0; i<count; i++) if ((uint32_t)tracks[i].number==want) pick = i;
	}
	if (pick<0 || !header->hunkbytes || header->hunkbytes % CHD_FRAME_SIZE) {
		fprintf(stderr, "[INFO] ra_chd: no track %u in %s (%i tracks, hunks of %u bytes)\n", want, path, count, header->hunkbytes);
		chd_close(chd);
		return NULL;
	}

	ChdTrack* track = calloc(1, sizeof(ChdTrack));
	track->chd = chd;
	track->chd_first_frame = tracks[pick].chd_first;
	track->first_sector = tracks[pick].first_sector;
	track->pregap = tracks[pick].pregap;
	track->frames = tracks[pick].frames;
	trackLayout(tracks[pick].type, &track->data_offset, &track->data_size);
	track->frames_per_hunk = header->hunkbytes / CHD_FRAME_SIZE;
	track->hunk = malloc(header->hunkbytes);
	track->hunk_index = -1;
	return track;
}

static size_t readChd(ChdTrack* track, uint32_t sector, uint8_t* buffer, size_t requested) {
	if (sector<track->first_sector) return 0;
	size_t total = 0;
	while (requested>0) {
		uint32_t frame = track->chd_first_frame + (sector - track->first_sector);
		if (sector - track->first_sector >= track->frames) break; // past the end of the track
		int hunk = frame / track->frames_per_hunk;
		if (hunk!=track->hunk_index) {
			if (chd_read(track->chd, hunk, track->hunk)!=CHDERR_NONE) break;
			track->hunk_index = hunk;
		}
		uint8_t* data = track->hunk + (frame % track->frames_per_hunk) * CHD_FRAME_SIZE + track->data_offset;
		size_t n = requested<(size_t)track->data_size ? requested : (size_t)track->data_size;
		memcpy(buffer, data, n);
		buffer += n;
		total += n;
		requested -= n;
		sector += 1;
	}
	return total;
}

static void* RC_CCONV openTrack(const char* path, uint32_t track, const struct rc_hash_iterator* iterator) {
	Handle* handle = calloc(1, sizeof(Handle));
	if (isChd(path)) {
		handle->is_chd = 1;
		handle->inner = openChdTrack(path, track);
	}
	else if (fallback.open_track_iterator && (iterator || !fallback.open_track)) handle->inner = fallback.open_track_iterator(path, track, iterator);
	else if (fallback.open_track) handle->inner = fallback.open_track(path, track);
	if (!handle->inner) {
		free(handle);
		return NULL;
	}
	return handle;
}
static void* RC_CCONV openTrackPlain(const char* path, uint32_t track) {
	return openTrack(path, track, NULL);
}
static size_t RC_CCONV readSector(void* track_handle, uint32_t sector, void* buffer, size_t requested) {
	Handle* handle = track_handle;
	if (handle->is_chd) return readChd(handle->inner, sector, buffer, requested);
	return fallback.read_sector(handle->inner, sector, buffer, requested);
}
static void RC_CCONV closeTrack(void* track_handle) {
	Handle* handle = track_handle;
	if (handle->is_chd) {
		ChdTrack* track = handle->inner;
		chd_close(track->chd);
		free(track->hunk);
		free(track);
	}
	else fallback.close_track(handle->inner);
	free(handle);
}
static uint32_t RC_CCONV firstTrackSector(void* track_handle) {
	Handle* handle = track_handle;
	if (handle->is_chd) {
		ChdTrack* track = handle->inner;
		return track->first_sector + track->pregap;
	}
	return fallback.first_track_sector(handle->inner);
}

void RA_getDiscReader(struct rc_hash_cdreader* reader) {
	rc_hash_get_default_cdreader(&fallback);
	memset(reader, 0, sizeof(*reader));
	reader->open_track_iterator = openTrack;
	reader->open_track = openTrackPlain; // rc_client only takes a reader that has this one too (it then calls the other)
	reader->read_sector = readSector;
	reader->close_track = closeTrack;
	reader->first_track_sector = firstTrackSector;
}
