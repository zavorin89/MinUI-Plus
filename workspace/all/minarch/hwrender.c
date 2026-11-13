// GPU-drawn cores (libretro's hardware rendering, OpenGL ES 2): what N64 needs. The core draws into a framebuffer of
// its own on a GLES2 context MinArch makes on the window SDL already has (the A30's Mali), and each finished frame is
// read back into memory as RGB565, so from there it goes through MinArch like any other core's: scaling, borders,
// the menu, states, the game switcher.
//
// The GL functions are looked up through SDL at run time (the toolchain has no GL libraries to link). The core
// itself is built without linking them too: MINARCH_GLES=1 (set by a GPU core's launch.sh) has MinArch load the
// A30's libGLESv2.so for everything before the core is opened, and the core's GL calls find it there.

#define _GNU_SOURCE // (dladdr)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <ucontext.h>
#include "sdl.h"
#include "defines.h"
#include "api.h"
#include "utils.h"
#include "libretro.h"
#include "hwrender.h"

SDL_Window* PLAT_getWindow(void) __attribute__((weak)); // the platform's window, where it has one to share
SDL_Texture* PLAT_gpuTexture(int width, int height) __attribute__((weak));
int PLAT_flipGpuFrame(SDL_Texture* texture, int width, int height, double aspect, int bottom_up) __attribute__((weak));

// the few GLES2 names and functions needed here (no headers in the toolchain)
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef void GLvoid;
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGB 0x1907
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_IMPLEMENTATION_COLOR_READ_TYPE 0x8B9A
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#define GL_EXTENSIONS 0x1F03
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

static struct {
	void (*GenFramebuffers)(GLsizei, GLuint*);
	void (*DeleteFramebuffers)(GLsizei, const GLuint*);
	void (*BindFramebuffer)(GLenum, GLuint);
	void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
	void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
	GLenum (*CheckFramebufferStatus)(GLenum);
	void (*GenRenderbuffers)(GLsizei, GLuint*);
	void (*DeleteRenderbuffers)(GLsizei, const GLuint*);
	void (*BindRenderbuffer)(GLenum, GLuint);
	void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
	void (*GenTextures)(GLsizei, GLuint*);
	void (*DeleteTextures)(GLsizei, const GLuint*);
	void (*BindTexture)(GLenum, GLuint);
	void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
	void (*TexParameteri)(GLenum, GLenum, GLint);
	void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*);
	void (*PixelStorei)(GLenum, GLint);
	void (*GetIntegerv)(GLenum, GLint*);
	const unsigned char* (*GetString)(GLenum);
	void (*Flush)(void);
} gl;

static struct {
	int requested; // the core asked (SET_HW_RENDER) and we said yes
	int ready; // context and framebuffer made, the core told (context_reset)
	uint64_t read_us;
	struct retro_hw_render_callback cb;
	SDL_Window* window;
	SDL_GLContext context;
	GLuint fbo, color, depth;
	int width, height; // the framebuffer's (the core's largest frame)
	int read_565; // the driver reads back RGB565 itself (else RGBA, converted here)
	uint16_t* frame; // the last frame, RGB565, top row first
	uint8_t* rgba; // reading room when converting
	SDL_Texture* shown; // SDL's own texture, when the core draws straight into it: SDL's renderer then shows it like any other
} hw;

// where a GPU core died (the core and the driver have no other way to say): to the pak's log. The stack's words let
// the core's own frames be found when the crash is inside the driver
static void HW_crashed(int signal, siginfo_t* info, void* context) {
#if defined(__arm__) && defined(__linux__)
	mcontext_t* m = &((ucontext_t*)context)->uc_mcontext;
#endif
	char line[256];
	int len;
#if defined(__arm__) && defined(__linux__)
	len = snprintf(line, sizeof(line), "hw: crash: signal %i at pc %08lx lr %08lx sp %08lx r0 %08lx r1 %08lx address %p\n", signal, m->arm_pc, m->arm_lr, m->arm_sp, m->arm_r0, m->arm_r1, info->si_addr);
	write(STDERR_FILENO, line, len);
#else
	len = snprintf(line, sizeof(line), "hw: crash: signal %i address %p\n", signal, info->si_addr);
	write(STDERR_FILENO, line, len);
#endif
	int maps = open("/proc/self/maps", O_RDONLY);
	if (maps>=0) {
		char buffer[4096];
		int got;
		while ((got = read(maps, buffer, sizeof(buffer)))>0) write(STDERR_FILENO, buffer, got);
		close(maps);
	}
#if defined(__arm__) && defined(__linux__)
	unsigned long* stack = (unsigned long*)m->arm_sp; // (last: this could run off its end)
	for (int i=0; i<1024; i+=8) {
		len = snprintf(line, sizeof(line), "%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n", stack[i],stack[i+1],stack[i+2],stack[i+3],stack[i+4],stack[i+5],stack[i+6],stack[i+7]);
		write(STDERR_FILENO, line, len);
	}
#endif
	_exit(1);
}

// MINARCH_PROFILE=<file>: where the CPU's time goes on the device itself, sampled by a timer and written to the file
// when the game's closed: each 16-byte stretch of code and who called it, with its samples, then what's mapped where
#define PROFILE_MOST (1<<18)
typedef struct { uint32_t pc, lr; } ProfileSample;
static ProfileSample* profile_samples = NULL;
static volatile int profile_count = 0;
static uint64_t profile_from = 0;
static void HW_profileTick(int signal, siginfo_t* info, void* context) {
#if defined(__arm__) && defined(__linux__)
	int i = __sync_fetch_and_add(&profile_count, 1);
	if (i<PROFILE_MOST) {
		profile_samples[i].pc = ((ucontext_t*)context)->uc_mcontext.arm_pc & ~15u;
		profile_samples[i].lr = ((ucontext_t*)context)->uc_mcontext.arm_lr & ~15u;
	}
#endif
}
static void HW_profileStart(void) {
	if (!getenv("MINARCH_PROFILE") || profile_samples) return;
	profile_samples = malloc(PROFILE_MOST * sizeof(ProfileSample));
	if (!profile_samples) return;
	struct sigaction tick = {0};
	tick.sa_sigaction = HW_profileTick;
	tick.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &tick, NULL);
	struct itimerval every = { {0,1000}, {0,1000} }; // (as often as the kernel's timer goes)
	setitimer(ITIMER_PROF, &every, NULL);
	profile_from = getMicroseconds();
}
static int HW_compareSamples(const void* a, const void* b) {
	const ProfileSample* sa = a;
	const ProfileSample* sb = b;
	if (sa->pc!=sb->pc) return sa->pc<sb->pc ? -1 : 1;
	return sa->lr<sb->lr ? -1 : sa->lr>sb->lr;
}
static void HW_profileEnd(void) {
	if (!profile_samples) return;
	struct itimerval off = {{0,0},{0,0}};
	setitimer(ITIMER_PROF, &off, NULL);
	signal(SIGPROF, SIG_IGN);
	int count = MIN(profile_count, PROFILE_MOST);
	qsort(profile_samples, count, sizeof(ProfileSample), HW_compareSamples);
	FILE* file = fopen(getenv("MINARCH_PROFILE"), "w");
	if (file) {
		fprintf(file, "%i samples in %i seconds\n", count, (int)((getMicroseconds() - profile_from) / 1000000));
		for (int i=0; i<count; ) {
			ProfileSample sample = profile_samples[i];
			int from = i;
			while (i<count && profile_samples[i].pc==sample.pc && profile_samples[i].lr==sample.lr) i++;
			Dl_info at = {0}, caller = {0};
			dladdr((void*)(uintptr_t)sample.pc, &at);
			dladdr((void*)(uintptr_t)sample.lr, &caller);
			fprintf(file, "%s|%x|%s|%s|%x|%i\n", at.dli_fname ? at.dli_fname : "?", sample.pc - (uint32_t)(uintptr_t)at.dli_fbase, at.dli_sname ? at.dli_sname : "",
				caller.dli_fname ? caller.dli_fname : "?", sample.lr - (uint32_t)(uintptr_t)caller.dli_fbase, i - from);
		}
		FILE* maps = fopen("/proc/self/maps", "r");
		char line[256];
		while (maps && fgets(line, sizeof(line), maps)) fputs(line, file);
		if (maps) fclose(maps);
		fclose(file);
	}
	free(profile_samples);
	profile_samples = NULL;
}

void HW_preload(void) {
	if (!getenv("MINARCH_GLES")) return;
	static char crash_stack[32768]; // its own, in case that's what ran out
	stack_t alternate = { .ss_sp = crash_stack, .ss_size = sizeof(crash_stack) };
	sigaltstack(&alternate, NULL);
	struct sigaction crash = {0};
	crash.sa_sigaction = HW_crashed;
	crash.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigaction(SIGSEGV, &crash, NULL);
	sigaction(SIGBUS, &crash, NULL);
	sigaction(SIGILL, &crash, NULL);
	sigaction(SIGFPE, &crash, NULL);
	sigaction(SIGABRT, &crash, NULL);
	// the core's GL calls are left for the loader to find: these, loaded for everything, are where it finds them
	// (EGL too: a core may look up its extensions through eglGetProcAddress)
	char* sets[][4] = { { "libEGL.so", "libEGL.so.1", NULL }, { "libGLESv2.so", "libGLESv2.so.2", "libMali.so", NULL } };
	for (int s=0; s<2; s++) {
		int loaded = 0;
		for (int i=0; sets[s][i] && !loaded; i++) {
			if (dlopen(sets[s][i], RTLD_NOW | RTLD_GLOBAL)) {
				LOG_info("hw: loaded %s\n", sets[s][i]);
				loaded = 1;
			}
		}
		if (!loaded) LOG_error("hw: couldn't load %s (%s)\n", sets[s][0], dlerror());
	}
}

static uintptr_t HW_getFramebuffer(void) {
	return hw.fbo;
}
static retro_proc_address_t HW_getProcAddress(const char* sym) {
	return (retro_proc_address_t)SDL_GL_GetProcAddress(sym);
}

bool HW_set(struct retro_hw_render_callback* cb) {
	LOG_info("hw: core asks for context type %i (%i.%i) depth %i stencil %i bottom_left %i\n", cb->context_type, cb->version_major, cb->version_minor, cb->depth, cb->stencil, cb->bottom_left_origin);
	if (cb->context_type!=RETRO_HW_CONTEXT_OPENGLES2 && !(cb->context_type==RETRO_HW_CONTEXT_OPENGLES_VERSION && cb->version_major==2)) return false;
	if (!PLAT_getWindow || !PLAT_getWindow()) return false;
	cb->get_current_framebuffer = HW_getFramebuffer;
	cb->get_proc_address = HW_getProcAddress;
	hw.cb = *cb;
	hw.requested = 1;
	return true;
}
int HW_active(void) {
	return hw.requested;
}

#define LOAD(name) if (!(*(void**)&gl.name = SDL_GL_GetProcAddress("gl" #name))) { LOG_error("hw: no gl" #name "\n"); return 0; }
static int HW_loadFunctions(void) {
	LOAD(GenFramebuffers) LOAD(DeleteFramebuffers) LOAD(BindFramebuffer) LOAD(FramebufferTexture2D)
	LOAD(FramebufferRenderbuffer) LOAD(CheckFramebufferStatus) LOAD(GenRenderbuffers) LOAD(DeleteRenderbuffers)
	LOAD(BindRenderbuffer) LOAD(RenderbufferStorage) LOAD(GenTextures) LOAD(DeleteTextures) LOAD(BindTexture)
	LOAD(TexImage2D) LOAD(TexParameteri) LOAD(ReadPixels) LOAD(PixelStorei) LOAD(GetIntegerv) LOAD(GetString)
	LOAD(Flush)
	return 1;
}
#undef LOAD

void HW_makeCurrent(void) {
	if (hw.ready) SDL_GL_MakeCurrent(hw.window, hw.context);
}

int HW_init(int max_width, int max_height) {
	if (!hw.requested) return 0;
	hw.window = PLAT_getWindow();
	if (!(SDL_GetWindowFlags(hw.window) & SDL_WINDOW_OPENGL)) LOG_info("hw: the window isn't a GL one (SDL's renderer isn't GLES)\n");
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

	hw.width = max_width>0 ? max_width : 640;
	hw.height = max_height>0 ? max_height : 480;
	// frames straight to the screen: the core's context shares with SDL's renderer's, and draws into one of its textures
	GLint shown_id = 0;
	if (getenv("MINARCH_GLES_DIRECT") && PLAT_gpuTexture && PLAT_flipGpuFrame) {
		void (*get_integer)(GLenum, GLint*) = SDL_GL_GetProcAddress("glGetIntegerv");
		hw.shown = PLAT_gpuTexture(hw.width, hw.height);
		if (hw.shown && get_integer && SDL_GL_BindTexture(hw.shown, NULL, NULL)==0) { // (which makes the renderer's context current)
			get_integer(GL_TEXTURE_BINDING_2D, &shown_id);
			SDL_GL_UnbindTexture(hw.shown);
		}
		if (!shown_id) hw.shown = NULL; // (SDL's renderer tidies it away)
	}
	if (hw.shown) {
		SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
		hw.context = SDL_GL_CreateContext(hw.window);
		SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
		if (!hw.context) {
			LOG_info("hw: no context sharing with SDL's renderer: %s\n", SDL_GetError());
			hw.shown = NULL;
		}
	}
	if (!hw.context) hw.context = SDL_GL_CreateContext(hw.window); // (it's made current)
	if (!hw.context) {
		LOG_error("hw: no GLES2 context: %s\n", SDL_GetError());
		hw.requested = 0;
		return 0;
	}
	if (!HW_loadFunctions()) {
		SDL_GL_DeleteContext(hw.context);
		hw.requested = 0;
		return 0;
	}
	LOG_info("hw: %s, %s (SDL video: %s)\n", gl.GetString(GL_RENDERER), gl.GetString(GL_VERSION), SDL_GetCurrentVideoDriver());

	if (hw.shown) hw.color = shown_id;
	else gl.GenTextures(1, &hw.color);
	gl.BindTexture(GL_TEXTURE_2D, hw.color);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	// 16-bit, what it's read back as anyway: less for the GPU to write, and nothing for the driver to convert
	gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, hw.width, hw.height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
	gl.GenFramebuffers(1, &hw.fbo);
	gl.BindFramebuffer(GL_FRAMEBUFFER, hw.fbo);
	gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hw.color, 0);
	if (hw.cb.depth || hw.cb.stencil) {
		gl.GenRenderbuffers(1, &hw.depth);
		gl.BindRenderbuffer(GL_RENDERBUFFER, hw.depth);
		const char* ext = (const char*)gl.GetString(GL_EXTENSIONS);
		int packed = hw.cb.stencil && ext && strstr(ext, "GL_OES_packed_depth_stencil");
		gl.RenderbufferStorage(GL_RENDERBUFFER, packed ? GL_DEPTH24_STENCIL8_OES : GL_DEPTH_COMPONENT16, hw.width, hw.height);
		gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, hw.depth);
		if (packed) gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, hw.depth);
	}
	GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status!=GL_FRAMEBUFFER_COMPLETE) {
		LOG_info("hw: no 16-bit framebuffer (0x%x), using 32-bit\n", status);
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, hw.width, hw.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status!=GL_FRAMEBUFFER_COMPLETE) LOG_error("hw: framebuffer incomplete (0x%x)\n", status);
	}

	GLint format = 0, type = 0;
	gl.GetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &format);
	gl.GetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);
	hw.read_565 = format==GL_RGB && type==GL_UNSIGNED_SHORT_5_6_5;
	gl.PixelStorei(GL_PACK_ALIGNMENT, hw.read_565 ? 2 : 4);
	hw.frame = malloc(hw.width * hw.height * sizeof(uint16_t));
	if (!hw.read_565) hw.rgba = malloc(hw.width * hw.height * 4);
	LOG_info("hw: framebuffer %ix%i, %s, read back as %s\n", hw.width, hw.height, hw.shown ? "shown straight" : "not shared", hw.read_565 ? "RGB565" : "RGBA");

	hw.ready = 1;
	return 1;
}

void HW_start(void) {
	if (!hw.ready) return;
	if (hw.cb.context_reset) hw.cb.context_reset();
	HW_makeCurrent(); // (in case the core's reset left another current)
	gl.BindFramebuffer(GL_FRAMEBUFFER, hw.fbo);
	HW_profileStart();
}

int HW_present(unsigned width, unsigned height, double aspect) {
	if (!hw.ready || !hw.shown) return 0;
	gl.Flush(); // the frame's on its way: the GPU finishes it while the core gets on with the next
	if (!PLAT_flipGpuFrame(hw.shown, width, height, aspect, hw.cb.bottom_left_origin)) return 0;
	HW_makeCurrent();
	return 1;
}

uint64_t HW_takeReadUs(void) {
	uint64_t us = hw.read_us;
	hw.read_us = 0;
	return us;
}

const void* HW_readFrame(unsigned width, unsigned height, size_t* pitch) {
	if (!hw.ready || !hw.frame || (!hw.read_565 && !hw.rgba)) return NULL; // (no memory for it: the frame's skipped)
	uint64_t read_from = getMicroseconds();
	if ((int)width>hw.width) width = hw.width;
	if ((int)height>hw.height) height = hw.height;
	gl.BindFramebuffer(GL_FRAMEBUFFER, hw.fbo);
	// GL's rows start at the bottom (unless the core drew it the other way up): turned the right way up here
	int flip = hw.cb.bottom_left_origin;
	if (hw.read_565) {
		gl.ReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, hw.frame);
		if (flip) {
			uint16_t row[width];
			for (unsigned y=0; y<height/2; y++) {
				uint16_t* a = hw.frame + y * width;
				uint16_t* b = hw.frame + (height - 1 - y) * width;
				memcpy(row, a, width * 2); memcpy(a, b, width * 2); memcpy(b, row, width * 2);
			}
		}
	}
	else {
		gl.ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, hw.rgba);
		for (unsigned y=0; y<height; y++) {
			const uint8_t* in = hw.rgba + (flip ? height - 1 - y : y) * width * 4;
			uint16_t* out = hw.frame + y * width;
			for (unsigned x=0; x<width; x++, in+=4) *out++ = (in[0] >> 3) << 11 | (in[1] >> 2) << 5 | in[2] >> 3;
		}
	}
	*pitch = width * sizeof(uint16_t);
	hw.read_us += getMicroseconds() - read_from;
	return hw.frame;
}

void HW_quit(void) {
	if (!hw.ready) return;
	HW_profileEnd();
	HW_makeCurrent();
	if (hw.cb.context_destroy) hw.cb.context_destroy();
	if (hw.depth) gl.DeleteRenderbuffers(1, &hw.depth);
	gl.DeleteFramebuffers(1, &hw.fbo);
	if (!hw.shown) gl.DeleteTextures(1, &hw.color); // (SDL's goes with its renderer)
	SDL_GL_DeleteContext(hw.context);
	free(hw.frame);
	free(hw.rgba);
	memset(&hw, 0, sizeof(hw));
}
