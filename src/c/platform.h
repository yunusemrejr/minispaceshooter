/* platform.h — the one and only OS-dependent layer of Mini Space Shooter.
 *
 * Everything above this header (minui, game, ML, director) is pure computation
 * over a plain 32-bit framebuffer.  The X11 implementation adds XShm for
 * zero-copy presents, plus a headless mode used by --shot/--selftest.
 *
 * Pixel format: 0x00RRGGBB, one uint32 per pixel, row-major, no padding.
 */
#ifndef MINI_PLATFORM_H
#define MINI_PLATFORM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Keyboard actions.  The renderer/UI never sees raw keysyms. */
typedef enum {
    PK_LEFT = 0,
    PK_RIGHT,
    PK_UP,
    PK_DOWN,
    PK_FIRE,     /* space / z / j */
    PK_ENTER,    /* return / kp-enter */
    PK_BACK,     /* escape */
    PK_PAUSE,    /* p */
    PK_MUTE,     /* m */
    PK_VOL_DOWN, /* [ */
    PK_VOL_UP,   /* ] */
    PK_SCALE_DOWN,
    PK_SCALE_UP,
    PK_DEBUG, /* f1 */
    PK_ALLY_1, PK_ALLY_2, PK_ALLY_3, PK_ALLY_4,
    PK_MUSIC, /* n */
    PK_COUNT
} PlatKey;

typedef struct {
    uint8_t down[PK_COUNT];    /* held this frame */
    uint8_t pressed[PK_COUNT]; /* went down this frame */
    uint8_t released[PK_COUNT];/* went up this frame */
    int quit;                  /* window close request */
    int scale;                 /* integer pixel scale currently in use */
} PlatInput;

/* Opens the window (or the headless sink when headless != 0).
 * requested_scale <= 0 asks for the largest integer scale that fits the screen.
 * Returns 0 on success, non-zero when no display is available. */
int plat_open(int internal_w, int internal_h, int requested_scale, int headless);

void plat_close(void);

/* Drains X events into `out`.  Clears stale held keys when focus is lost. */
void plat_poll(PlatInput *out);

/* Nearest-neighbour upscale internal -> window and blit.  Cheap, single pass. */
void plat_present(const uint32_t *pixels, int w, int h);

/* Reads the window content back from the X server and compares it with the last
 * frame presented. Returns matching pixels, -2 for an unmapped X11 window, or
 * -1 when read-back is unavailable (Wayland, headless, or refused XGetImage). Used by
 * --verify-present to prove the presentation path really delivers the pixels,
 * including the XShm double-buffer handshake. */
int plat_verify_present(void);

/* Nearest-neighbour upscale into an arbitrary destination buffer.
 * Used for the --shot renderer and available to any caller that needs it. */
void plat_scale_blit(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh, int scale);

int plat_scale(void);            /* current integer scale factor */
/* Asks the X server for the keyboard focus.  Test aid: it makes synthetic key
 * events (XTEST) land in this window instead of the user's active window.
 * Returns 0 when this window really holds the input focus. */
int plat_focus_self(void);
void plat_screen_size(int *w, int *h);
int plat_headless(void);
void plat_mode_label(char *out, int cap); /* e.g. "x11-shm 1152x648" */

double plat_time(void);              /* monotonic seconds */
void plat_sleep(double seconds);     /* yields the CPU, never busy-waits */

/* Writes a binary PPM (P6).  Returns 0 on success. */
int plat_write_ppm(const char *path, const uint32_t *px, int w, int h);

/* Small string helpers so C++ callers avoid <string>. */
int plat_snprintf(char *dst, int cap, const char *fmt, ...);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MINI_PLATFORM_H */
