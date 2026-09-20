#ifndef MINI_PLATFORM_SDL_H
#define MINI_PLATFORM_SDL_H

#include "platform.h"

/* Optional native Wayland transport, resolved from the SDL2 runtime library.
 * The game, framebuffer, audio and headless tests remain independent of SDL. */
int sdlplat_open(int w, int h, int scale);
void sdlplat_close(void);
void sdlplat_poll(PlatInput *out);
void sdlplat_present(const uint32_t *pixels, int w, int h);
int sdlplat_scale(void);
int sdlplat_focus(void);
void sdlplat_mode(char *out, int cap);

#endif
