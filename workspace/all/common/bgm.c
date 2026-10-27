// Background music for the launcher's menus, built to stay out of the way of the menus themselves:
// - a thread at the lowest priority decodes about a second ahead into a ring, in short bursts, then sleeps
// - the audio callback only copies from the ring, scaled by the volume (silence if the ring ever runs dry)
// - songs are streamed from the card a chunk at a time, nothing is loaded whole

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "bgm.h"

#define DR_MP3_IMPLEMENTATION
#include "audio/dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "audio/dr_wav.h"
#ifndef BGM_FLAC_LINKED // minarch already has dr_flac, from libchdr
#define DR_FLAC_IMPLEMENTATION
#endif
#include "../minarch/libchdr/include/dr_libs/dr_flac.h"
#define STB_VORBIS_NO_PUSHDATA_API
#include "audio/stb_vorbis.c"

#define SONG_MAX 256
#define RING_FRAMES 65536 // stereo frames, about 1.4 seconds at 48000Hz (a power of two, for the wrapping)
#define CHUNK_FRAMES 1024 // decoded per step
#define FADE_IN_MS 1500
#define FADE_OUT_MS 120

enum { SONG_NONE, SONG_MP3, SONG_OGG, SONG_FLAC, SONG_WAV };

static struct {
	int enabled; // -1 until read
	int volume; // 0 until read
	int song_count; // -1 until looked for
	char songs[SONG_MAX][MAX_PATH];

	pthread_t thread;
	int running;
	volatile int quit;
	volatile int paused; // the pause menu closed: fades out, keeping its place in the song

	int16_t ring[RING_FRAMES * 2];
	volatile uint32_t written; // frames, ever (wraps fine as unsigned)
	volatile uint32_t read;
	int rate; // the device's

	volatile int gain; // what the volume asks for, Q16
	int level; // where the fade is now, Q16, only touched on the audio thread
} bgm = { .enabled = -1, .song_count = -1 };

// the song being decoded
static struct {
	int type;
	drmp3 mp3;
	stb_vorbis* ogg;
	drflac* flac;
	FILE* flac_file; // read through callbacks: minarch's dr_flac has no file functions
	drwav wav;
	int channels;
	int rate;
	uint32_t phase; // the resampler's place between the last frame and the next, Q16
	int16_t previous[2];
} song;

static void BGM_mix(int16_t* stream, int frames);

///////////////////////////////

static int volumeGain(int volume) { // a curve, since loudness isn't linear: 10 is full, 5 a quarter
	return (volume * volume * 65536) / (BGM_VOLUME_MAX * BGM_VOLUME_MAX);
}
int BGM_isEnabled(void) {
	if (bgm.enabled==-1) bgm.enabled = exists(BGM_ENABLED_PATH) && getInt(BGM_ENABLED_PATH); // off unless turned on
	return bgm.enabled;
}
int BGM_getVolume(void) {
	if (!bgm.volume) {
		bgm.volume = exists(BGM_VOLUME_PATH) ? getInt(BGM_VOLUME_PATH) : BGM_VOLUME_MAX / 2;
		if (bgm.volume<1) bgm.volume = 1;
		if (bgm.volume>BGM_VOLUME_MAX) bgm.volume = BGM_VOLUME_MAX;
	}
	return bgm.volume;
}
void BGM_setVolume(int volume) {
	if (volume<1) volume = 1;
	if (volume>BGM_VOLUME_MAX) volume = BGM_VOLUME_MAX;
	bgm.volume = volume;
	bgm.gain = volumeGain(volume);
	putInt(BGM_VOLUME_PATH, volume);
}
void BGM_setEnabled(int enabled) {
	bgm.enabled = enabled;
	putInt(BGM_ENABLED_PATH, enabled);
	if (enabled) BGM_start();
	else BGM_quit();
}

static int songType(char* name) {
	if (name[0]=='.') return SONG_NONE; // also macOS's ._ files
	char* ext = strrchr(name, '.');
	if (!ext) return SONG_NONE;
	if (!strcasecmp(ext, ".mp3")) return SONG_MP3;
	if (!strcasecmp(ext, ".ogg")) return SONG_OGG;
	if (!strcasecmp(ext, ".flac")) return SONG_FLAC;
	if (!strcasecmp(ext, ".wav")) return SONG_WAV;
	return SONG_NONE;
}
int BGM_countSongs(void) {
	if (bgm.song_count>=0) return bgm.song_count;
	bgm.song_count = 0;
	DIR* dir = opendir(BGM_PATH);
	if (!dir) return 0;
	struct dirent* e;
	while ((e = readdir(dir)) && bgm.song_count<SONG_MAX) {
		if (songType(e->d_name)==SONG_NONE) continue;
		snprintf(bgm.songs[bgm.song_count++], MAX_PATH, "%s/%s", BGM_PATH, e->d_name);
	}
	closedir(dir);
	return bgm.song_count;
}

///////////////////////////////

static size_t flacRead(void* file, void* out, size_t bytes) {
	return fread(out, 1, bytes, file);
}
static drflac_bool32 flacSeek(void* file, int offset, drflac_seek_origin origin) {
	int whence = origin==DRFLAC_SEEK_SET ? SEEK_SET : (origin==DRFLAC_SEEK_END ? SEEK_END : SEEK_CUR);
	return fseek(file, offset, whence)==0;
}
static drflac_bool32 flacTell(void* file, drflac_int64* at) {
	*at = ftell(file);
	return *at>=0;
}
static void closeSong(void) {
	switch (song.type) {
		case SONG_MP3: drmp3_uninit(&song.mp3); break;
		case SONG_OGG: stb_vorbis_close(song.ogg); break;
		case SONG_FLAC: drflac_close(song.flac); fclose(song.flac_file); break;
		case SONG_WAV: drwav_uninit(&song.wav); break;
	}
	song.type = SONG_NONE;
}
static int openSong(char* path) {
	memset(&song, 0, sizeof(song));
	int type = songType(strrchr(path, '/')+1);
	switch (type) {
		case SONG_MP3:
			if (!drmp3_init_file(&song.mp3, path, NULL)) return 0;
			song.channels = song.mp3.channels;
			song.rate = song.mp3.sampleRate;
			break;
		case SONG_OGG: {
			int error;
			song.ogg = stb_vorbis_open_filename(path, &error, NULL);
			if (!song.ogg) return 0;
			stb_vorbis_info info = stb_vorbis_get_info(song.ogg);
			song.channels = 2; // stb_vorbis mixes to two for us
			song.rate = info.sample_rate;
			break;
		}
		case SONG_FLAC:
			song.flac_file = fopen(path, "rb");
			if (!song.flac_file) return 0;
			song.flac = drflac_open(flacRead, flacSeek, flacTell, song.flac_file, NULL);
			if (!song.flac) {
				fclose(song.flac_file);
				return 0;
			}
			song.channels = song.flac->channels;
			song.rate = song.flac->sampleRate;
			break;
		case SONG_WAV:
			if (!drwav_init_file(&song.wav, path, NULL)) return 0;
			song.channels = song.wav.channels;
			song.rate = song.wav.sampleRate;
			break;
		default: return 0;
	}
	song.type = type;
	if (song.channels<1 || song.rate<8000 || song.rate>192000) {
		closeSong();
		return 0;
	}
	return 1;
}
// every song once in a shuffled order, then shuffled again; the first is never the one last launch ended on
// (kept in /tmp), nor does a new order start with the song that just played
static int order[SONG_MAX];
static int order_at = SONG_MAX;
static void shuffle(int avoid) {
	for (int i=0; i<bgm.song_count; i++) order[i] = i;
	for (int i=bgm.song_count-1; i>0; i--) {
		int j = rand() % (i + 1);
		int t = order[i]; order[i] = order[j]; order[j] = t;
	}
	if (bgm.song_count>1 && order[0]==avoid) {
		int t = order[0]; order[0] = order[1]; order[1] = t;
	}
	order_at = 0;
}
static int nextSong(void) {
	static int last = -1;
	if (last==-1 && exists(BGM_LAST_PATH)) {
		char last_path[MAX_PATH] = "";
		getFile(BGM_LAST_PATH, last_path, sizeof(last_path));
		for (int i=0; i<bgm.song_count; i++) if (exactMatch(bgm.songs[i], last_path)) last = i;
	}
	for (int tries=0; tries<bgm.song_count; tries++) {
		if (order_at>=bgm.song_count) shuffle(last);
		int pick = order[order_at++];
		last = pick;
		putFile(BGM_LAST_PATH, bgm.songs[pick]);
		if (openSong(bgm.songs[pick])) {
			LOG_info("music: %s\n", bgm.songs[pick]);
			return 1;
		}
		LOG_info("music: can't play %s\n", bgm.songs[pick]);
	}
	return 0;
}
// up to CHUNK_FRAMES stereo frames of the song, 0 at its end
static int decode(int16_t* out) {
	static int16_t raw[CHUNK_FRAMES * 8]; // up to 8 channels
	int frames = 0;
	switch (song.type) {
		case SONG_MP3: frames = drmp3_read_pcm_frames_s16(&song.mp3, CHUNK_FRAMES, raw); break;
		case SONG_OGG: frames = stb_vorbis_get_samples_short_interleaved(song.ogg, 2, raw, CHUNK_FRAMES * 2); break;
		case SONG_FLAC: frames = drflac_read_pcm_frames_s16(song.flac, CHUNK_FRAMES, raw); break;
		case SONG_WAV: frames = drwav_read_pcm_frames_s16(&song.wav, CHUNK_FRAMES, raw); break;
	}
	int channels = MIN(song.channels, 8);
	for (int i=0; i<frames; i++) {
		int16_t* frame = &raw[i * channels];
		out[i*2] = frame[0];
		out[i*2+1] = channels>1 ? frame[1] : frame[0];
	}
	return frames;
}
// to the device's rate (linear, plenty for music under menus), straight into the ring
static void resampleInto(int16_t* in, int frames) {
	uint32_t step = ((uint64_t)song.rate << 16) / bgm.rate;
	uint32_t at = bgm.written;
	for (;;) {
		int i = song.phase >> 16; // between in[i-1] (or the previous chunk's last) and in[i]
		if (i>=frames) break;
		int frac = song.phase & 0xFFFF;
		int16_t* a = i ? &in[(i-1)*2] : song.previous;
		int16_t* b = &in[i*2];
		int16_t* dst = &bgm.ring[(at % RING_FRAMES) * 2];
		dst[0] = a[0] + (((b[0] - a[0]) * frac) >> 16);
		dst[1] = a[1] + (((b[1] - a[1]) * frac) >> 16);
		at += 1;
		song.phase += step;
	}
	song.phase -= frames << 16;
	song.previous[0] = in[(frames-1)*2];
	song.previous[1] = in[(frames-1)*2+1];
	__sync_synchronize(); // the frames are in before the callback can see them
	bgm.written = at;
}

static void logCost(struct timespec* cpu_from, uint32_t written_from) {
	struct timespec cpu_to;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_to);
	double cpu = (cpu_to.tv_sec - cpu_from->tv_sec) + (cpu_to.tv_nsec - cpu_from->tv_nsec) / 1e9;
	double played = (double)(bgm.written - written_from) / bgm.rate;
	if (played>0) LOG_info("music: %.1fs decoded with %.2fs of CPU (%.1f%%)\n", played, cpu, cpu * 100 / played);
}
static void* BGM_run(void* arg) {
	setpriority(PRIO_PROCESS, syscall(SYS_gettid), 19); // the menus always come first
	bgm.rate = SFX_open();
	if (!bgm.rate) return NULL;
	srand(time(NULL));
	SFX_setMusic(NULL); // (fresh counters before the callback reads them)
	bgm.level = 0;
	bgm.gain = volumeGain(BGM_getVolume());
	bgm.written = bgm.read = 0;
	SFX_setMusic(BGM_mix);

	static int16_t chunk[CHUNK_FRAMES * 2];
	int playing = 0;
	// what decoding costs, logged per song: CPU time against the time it plays for
	struct timespec cpu_from;
	uint32_t written_from = 0;
	while (!bgm.quit) {
		uint32_t queued = bgm.written - bgm.read;
		uint32_t room = RING_FRAMES - queued;
		// a chunk can become up to 6x as many frames (8000Hz to 48000Hz)
		if (room<CHUNK_FRAMES * 6 || queued>RING_FRAMES * 3 / 4) {
			usleep(queued>RING_FRAMES * 7 / 8 ? 150000 : 40000); // (full and waiting, eg. asleep or in a game: rarely)
			continue;
		}
		if (!playing) {
			playing = nextSong();
			if (!playing) break; // nothing that plays
			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_from);
			written_from = bgm.written;
		}
		int frames = decode(chunk);
		if (frames>0) resampleInto(chunk, frames);
		if (frames<=0) {
			logCost(&cpu_from, written_from);
			closeSong();
			playing = 0;
		}
	}
	if (playing) {
		logCost(&cpu_from, written_from);
		closeSong();
	}
	return NULL;
}
// on the audio thread: adds what's ready into stream (silence, or a game's audio), fading
static void BGM_mix(int16_t* stream, int frames) {
	int target = (bgm.quit || bgm.paused) ? 0 : bgm.gain;
	if (!target && !bgm.level) return; // faded out: the rest waits for the next time
	uint32_t ready = bgm.written - bgm.read;
	__sync_synchronize();
	int count = MIN((uint32_t)frames, ready);
	int level = bgm.level;
	int up = bgm.gain / (bgm.rate * FADE_IN_MS / 1000) + 1; // the same time to fade in at any volume
	int down = 65536 / (bgm.rate * FADE_OUT_MS / 1000) + 1;
	uint32_t at = bgm.read;
	for (int i=0; i<count; i++) {
		if (level<target) level = MIN(target, level + up);
		else if (level>target) level = MAX(target, level - down);
		int16_t* src = &bgm.ring[(at % RING_FRAMES) * 2];
		int l = stream[i*2] + ((src[0] * level) >> 16);
		int r = stream[i*2+1] + ((src[1] * level) >> 16);
		stream[i*2] = l>32767 ? 32767 : (l<-32768 ? -32768 : l);
		stream[i*2+1] = r>32767 ? 32767 : (r<-32768 ? -32768 : r);
		at += 1;
		if (!level && !target) break; // done fading out
	}
	bgm.level = level;
	bgm.read = at;
}

void BGM_start(void) {
	bgm.paused = 0;
	if (bgm.running || !BGM_isEnabled() || !BGM_countSongs()) return;
	bgm.quit = 0;
	if (pthread_create(&bgm.thread, NULL, BGM_run, NULL)==0) bgm.running = 1;
}
void BGM_pause(void) {
	bgm.paused = 1;
}
void BGM_quit(void) {
	if (!bgm.running) return;
	bgm.quit = 1;
	pthread_join(bgm.thread, NULL);
	for (int waited=0; waited<FADE_OUT_MS+40 && bgm.level>0 && bgm.written!=bgm.read; waited+=10) SDL_Delay(10);
	SFX_setMusic(NULL);
	bgm.running = 0;
}
