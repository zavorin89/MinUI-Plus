#ifndef HWRENDER_H
#define HWRENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "libretro.h"

// GPU-drawn cores (see hwrender.c)
void HW_preload(void); // before the core is opened (MINARCH_GLES=1 only)
bool HW_set(struct retro_hw_render_callback* cb); // RETRO_ENVIRONMENT_SET_HW_RENDER
int HW_active(void); // the core draws with the GPU
int HW_init(int max_width, int max_height); // after the game loads: the context and framebuffer
void HW_start(void); // the core's context_reset, once MinArch can take its callbacks: parallel-n64 starts emulating in it
void HW_makeCurrent(void); // ours again, before the core runs (SDL's renderer takes its own to draw)
const void* HW_readFrame(unsigned width, unsigned height, size_t* pitch); // the frame drawn, as RGB565
int HW_present(unsigned width, unsigned height, double aspect); // the frame straight onto the screen, with no reading back (0 when it can't)
uint64_t HW_takeReadUs(void); // microseconds spent reading frames back since the last call
void HW_quit(void);

#endif
