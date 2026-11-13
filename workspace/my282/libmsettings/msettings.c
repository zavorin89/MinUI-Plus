#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <string.h>
#include <sound/asound.h>

#include "msettings.h"

///////////////////////////////////////

#define SETTINGS_VERSION 1
typedef struct Settings {
	int version; // future proofing
	int brightness;
	int headphones;
	int speaker;
	int unused[2]; // for future use
	// NOTE: doesn't really need to be persisted but still needs to be shared
	int jack; 
	int hdmi; 
} Settings;
static Settings DefaultSettings = {
	.version = SETTINGS_VERSION,
	.brightness = 2,
	.headphones = 4,
	.speaker = 8,
	.jack = 0,
	.hdmi = 0,
};
static Settings* settings;
static Settings local_settings; // when the shared memory can't be had: this program's own, so it still runs

#define SHM_KEY "/SharedSettings"
static char SettingsPath[256];
static int shm_fd = -1;
static int is_host = 0;
static int shm_size = sizeof(Settings);
static int brightnessRaw(int value);

void InitSettings(void) {	
	const char* userdata = getenv("USERDATA_PATH");
	snprintf(SettingsPath, sizeof(SettingsPath), "%s/msettings.bin", userdata ? userdata : ".");
	
	shm_fd = shm_open(SHM_KEY, O_RDWR | O_CREAT | O_EXCL, 0644); // see if it exists
	if (shm_fd==-1 && errno==EEXIST) { // already exists
		puts("Settings client");
		shm_fd = shm_open(SHM_KEY, O_RDWR, 0644);
		settings = shm_fd>=0 ? mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0) : MAP_FAILED;
		if (settings==MAP_FAILED) { // (read from the card instead, as a host would)
			settings = &local_settings;
			memcpy(settings, &DefaultSettings, shm_size);
			int fd = open(SettingsPath, O_RDONLY);
			if (fd>=0) {
				if (read(fd, settings, shm_size)!=shm_size) memcpy(settings, &DefaultSettings, shm_size);
				close(fd);
			}
		}
	}
	else { // host
		puts("Settings host"); // should always be keymon
		is_host = 1;
		// we created it so set initial size and populate
		settings = shm_fd>=0 && ftruncate(shm_fd, shm_size)==0 ? mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0) : MAP_FAILED;
		if (settings==MAP_FAILED) settings = &local_settings;
		
		memcpy(settings, &DefaultSettings, shm_size);
		int fd = open(SettingsPath, O_RDONLY);
		if (fd>=0) {
			if (read(fd, settings, shm_size)!=shm_size) memcpy(settings, &DefaultSettings, shm_size); // a cut-short file
			close(fd);
		}
		// a damaged file can't set the screen dark for good or the volume past the top
		if (settings->brightness<0 || settings->brightness>10) settings->brightness = DefaultSettings.brightness;
		if (settings->speaker<0 || settings->speaker>20) settings->speaker = DefaultSettings.speaker;
		if (settings->headphones<0 || settings->headphones>20) settings->headphones = DefaultSettings.headphones;
		
		// these shouldn't be persisted
		// settings->jack = 0;
		// settings->hdmi = 0;
	}
	
	printf("brightness: %i \nspeaker: %i\n", settings->brightness, settings->speaker); fflush(stdout);
	
	// applied as they are, not saved again: every program starts with this, and the save ends in a sync()
	SetRawVolume(GetVolume() * 5);
	if (!settings->hdmi) SetRawBrightness(brightnessRaw(GetBrightness()));
	// system("echo $(< " BRIGHTNESS_PATH ")");
}
void QuitSettings(void) {
	if (settings!=&local_settings) munmap(settings, shm_size);
	if (shm_fd>=0) close(shm_fd);
	if (is_host) shm_unlink(SHM_KEY);
}
static inline void SaveSettings(void) {
	// what's on the card already (eg. the same brightness and volume put back on every wake) isn't written again.
	// Compared with the file, not a copy kept here: keymon and the launcher or a game each save to it
	Settings on_card;
	int fd = open(SettingsPath, O_RDONLY);
	if (fd>=0) {
		int same = read(fd, &on_card, shm_size)==shm_size && !memcmp(&on_card, settings, shm_size);
		close(fd);
		if (same) return;
	}
	fd = open(SettingsPath, O_CREAT|O_WRONLY, 0644);
	if (fd>=0) {
		write(fd, settings, shm_size);
		fsync(fd); // just this file: sync() flushed the whole card on every volume and brightness step
		close(fd);
	}
}

int GetBrightness(void) { // 0-10
	return settings->brightness;
}
static int brightnessRaw(int value) {
	int raw = 255;
	switch (value) {
		case 0:  raw =   3;	break; 	// 0
		case 1:  raw =   4;	break; 	// 1
		case 2:  raw =   5;	break; 	// 1
		case 3:  raw =   6;	break; 	// 1
		case 4:  raw =   8;	break;	// 2
		case 5:  raw =  12;	break;	// 4
		case 6:  raw =  16;	break;	// 4
		case 7:  raw =  24;	break;	// 8
		case 8:  raw =  72;	break;	// 48
		case 9:  raw = 128;	break;	// 80
		case 10: raw = 255;	break;	// *
	}
	return raw;
}
void SetBrightness(int value) {
	if (settings->hdmi) return;
	
	SetRawBrightness(brightnessRaw(value));
	settings->brightness = value;
	SaveSettings();
}

int GetVolume(void) { // 0-20
	return settings->jack ? settings->headphones : settings->speaker;
}
void SetVolume(int value) {
	if (settings->hdmi) return;
	
	if (settings->jack) settings->headphones = value;
	else settings->speaker = value;
	
	int raw = value * 5;
	SetRawVolume(raw);
	SaveSettings();
}

#define DISP_LCD_SET_BRIGHTNESS  0x102
void SetRawBrightness(int val) { // 0 - 255
	if (settings->hdmi) return;
	
	printf("SetRawBrightness(%i)\n", val); fflush(stdout);
	int fd = open("/dev/disp", O_RDWR);
	if (fd>=0) {
	    unsigned long param[4]={0,val,0,0};
		ioctl(fd, DISP_LCD_SET_BRIGHTNESS, &param);
		close(fd);
	}
}
void SetRawVolume(int val) { // 0 - 100
	printf("SetRawVolume(%i)\n", val); fflush(stdout);
	// straight to the sound driver, as amixer would set it (its percent is rounded onto the control's 0-63), instead
	// of starting a shell and amixer for every change. amixer is still there if the driver turns it down
	int fd = open("/dev/snd/controlC0", O_RDWR);
	if (fd>=0) {
		struct snd_ctl_elem_value value;
		memset(&value, 0, sizeof(value));
		value.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		strcpy((char*)value.id.name, "headphone volume");
		int level = (val * 63 + 50) / 100;
		if (level==0 && val>0) level = 1;
		value.value.integer.value[0] = level;
		int ok = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value)>=0;
		close(fd);
		if (ok) return;
	}
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "amixer set 'headphone volume' %i%% > /dev/null 2>&1", val);
	system(cmd);
}

// monitored and set by thread in keymon?
int GetJack(void) {
	return settings->jack;
}
void SetJack(int value) {
	settings->jack = value;
	SetVolume(GetVolume());
}

int GetHDMI(void) {	return 0; }
void SetHDMI(int value) { /* buh */ }

int GetMute(void) { return 0; }
void SetMute(int value) {}
