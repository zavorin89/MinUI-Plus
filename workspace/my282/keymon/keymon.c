#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/time.h>
#include <signal.h>

#include <msettings.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

// #include "defines.h"

#define VOLUME_MIN 		0
#define VOLUME_MAX 		20
#define BRIGHTNESS_MIN 	0
#define BRIGHTNESS_MAX 	10

// uses different codes from SDL
#define CODE_MENU		1 // but also 354
#define CODE_PLUS		115
#define CODE_MINUS		114

//	for ev.value
#define RELEASED	0
#define PRESSED		1
#define REPEAT		2

#define INPUT_COUNT 2
static int inputs[INPUT_COUNT];
static struct input_event ev;

// the menu and games stop keymon while asleep (SIGSTOP) and let it go on waking (SIGCONT): what was pressed meanwhile
// (eg. power, to wake) is then still waiting to be read, and mustn't change the volume or brightness
static volatile sig_atomic_t resumed = 0;
static void onContinue(int sig) { resumed = 1; }

int main (int argc, char *argv[]) {
	InitSettings();
	
	// TODO: will require two inputs
	// input_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	inputs[0] = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	inputs[1] = open("/dev/input/event3", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	
	uint32_t input;
	uint32_t val;
	uint32_t menu_pressed = 0;
	
	uint32_t up_pressed = 0;
	uint32_t up_just_pressed = 0;
	uint32_t up_repeat_at = 0;
	
	uint32_t down_pressed = 0;
	uint32_t down_just_pressed = 0;
	uint32_t down_repeat_at = 0;
	
	uint8_t ignore;
	uint32_t then;
	uint32_t now;
	struct timeval tod;
	
	struct sigaction cont = {0};
	cont.sa_handler = onContinue; // (no SA_RESTART: a poll it interrupts returns, and the loop goes round)
	sigaction(SIGCONT, &cont, NULL);
	
	gettimeofday(&tod, NULL);
	then = tod.tv_sec * 1000 + tod.tv_usec / 1000; // essential SDL_GetTicks()
	ignore = 0;
	
	while (1) {
		gettimeofday(&tod, NULL);
		now = tod.tv_sec * 1000 + tod.tv_usec / 1000;
		if (resumed) { // ignore input that arrived during sleep
			resumed = 0;
			ignore = 1;
		}
		
		for (int i=0; i<INPUT_COUNT; i++) {
			int input_fd = inputs[i];
			while(read(input_fd, &ev, sizeof(ev))==sizeof(ev)) {
				if (ignore) continue;
				val = ev.value;

				if (( ev.type != EV_KEY ) || ( val > REPEAT )) continue;
				// printf("code: %i (%i)\n", ev.code, val); fflush(stdout);
				switch (ev.code) {
					case CODE_MENU:
						menu_pressed = val;
					break;
					break;
					case CODE_PLUS:
						up_pressed = up_just_pressed = val;
						if (val) up_repeat_at = now + 300;
					break;
					case CODE_MINUS:
						down_pressed = down_just_pressed = val;
						if (val) down_repeat_at = now + 300;
					break;
					default:
					break;
				}
			}
		}
		
		if (ignore) {
			menu_pressed = 0;
			up_pressed = up_just_pressed = 0;
			down_pressed = down_just_pressed = 0;
			up_repeat_at = 0;
			down_repeat_at = 0;
		}
		
		if (up_just_pressed || (up_pressed && now>=up_repeat_at)) {
			if (menu_pressed) {
				// printf("brightness up\n"); fflush(stdout);
				val = GetBrightness();
				if (val<BRIGHTNESS_MAX) SetBrightness(++val);
			}
			else {
				// printf("volume up\n"); fflush(stdout);
				val = GetVolume();
				if (val<VOLUME_MAX) SetVolume(++val);
			}
			
			if (up_just_pressed) up_just_pressed = 0;
			else up_repeat_at += 100;
		}
		
		if (down_just_pressed || (down_pressed && now>=down_repeat_at)) {
			if (menu_pressed) {
				// printf("brightness down\n"); fflush(stdout);
				val = GetBrightness();
				if (val>BRIGHTNESS_MIN) SetBrightness(--val);
			}
			else {
				// printf("volume down\n"); fflush(stdout);
				val = GetVolume();
				if (val>VOLUME_MIN) SetVolume(--val);
			}
			
			if (down_just_pressed) down_just_pressed = 0;
			else down_repeat_at += 100;
		}
		
		then = now;
		ignore = 0;
		
		// waits for a button instead of waking 60 times a second for nothing: at 60fps only while volume or
		// brightness is held (for its repeat), otherwise until a button (or the wake from sleep, SIGCONT)
		#define IDLE_MS -1
		struct pollfd fds[INPUT_COUNT];
		int watched = 0;
		for (int i=0; i<INPUT_COUNT; i++) {
			if (inputs[i]<0) continue;
			fds[watched].fd = inputs[i];
			fds[watched].events = POLLIN;
			fds[watched].revents = 0;
			watched += 1;
		}
		int holding = up_pressed || down_pressed;
		if (watched) poll(fds, watched, holding ? 16 : IDLE_MS);
		else usleep(16666);
	}
}
