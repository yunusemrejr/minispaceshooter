/* Native Wayland window transport using the optional system SDL2 runtime.
 * No SDL development package is required. The small ABI subset below follows
 * SDL2's public Linux C ABI (not SDL3): https://wiki.libsdl.org/SDL2/CategoryAPI
 * Window and surface objects are opaque; no SDL-owned memory is dereferenced.
 */
#define _POSIX_C_SOURCE 200809L
#include "platform_sdl.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Surface SDL_Surface;
typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct { int scancode, sym; uint16_t mod; uint32_t unused; } SDL_Keysym;
/* Stable event prefixes and union size from SDL2 SDL_events.h. */
typedef union {
    uint32_t type;
    struct { uint32_t type, timestamp, window_id; uint8_t event, pad1, pad2, pad3; int data1, data2; } window;
    struct { uint32_t type, timestamp, window_id; uint8_t state, repeat, pad2, pad3; SDL_Keysym keysym; } key;
    uint64_t align;
    uint8_t padding[56];
} SDL_Event;
_Static_assert(sizeof(void *) <= 8 && sizeof(SDL_Event) == 56, "SDL2 Linux event ABI");
_Static_assert(offsetof(SDL_Event, key.keysym) == 16, "SDL2 keyboard event ABI");

enum {
    SDL_QUIT_EVENT = 0x100, SDL_WINDOW_EVENT = 0x200,
    SDL_KEY_DOWN = 0x300, SDL_KEY_UP = 0x301,
    SDL_FOCUS_LOST = 13, SDL_WINDOW_CLOSE = 14,
    SDL_WINDOW_SHOWN = 4, SDL_INPUT_FOCUS = 0x200,
    SDL_WINDOW_CENTERED = 0x2fff0000
};

static struct {
    void *library;
    SDL_Window *window;
    SDL_Surface *source;
    uint32_t *pixels;
    char *previous_gdk_backend;
    int gdk_backend_set;
    int w, h, scale, max_scale, video_ready, failed;
    uint8_t keys[512], down[PK_COUNT];
    int (*VideoInit)(const char *);
    void (*VideoQuit)(void);
    void (*Quit)(void);
    const char *(*GetError)(void);
    int (*SetHint)(const char *, const char *);
    int (*GetDisplayUsableBounds)(int, SDL_Rect *);
    SDL_Window *(*CreateWindow)(const char *, int, int, int, int, uint32_t);
    void (*DestroyWindow)(SDL_Window *);
    uint32_t (*GetWindowFlags)(SDL_Window *);
    void (*SetWindowSize)(SDL_Window *, int, int);
    SDL_Surface *(*GetWindowSurface)(SDL_Window *);
    int (*UpdateWindowSurface)(SDL_Window *);
    SDL_Surface *(*CreateRGBSurfaceFrom)(void *, int, int, int, int, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*FreeSurface)(SDL_Surface *);
    int (*UpperBlitScaled)(SDL_Surface *, const SDL_Rect *, SDL_Surface *, SDL_Rect *);
    int (*PollEvent)(SDL_Event *);
    uint8_t (*EventState)(uint32_t, int);
    void (*RaiseWindow)(SDL_Window *);
} S;

void sdlplat_close(void)
{
    if (S.source) S.FreeSurface(S.source);
    free(S.pixels);
    if (S.window) S.DestroyWindow(S.window);
    if (S.video_ready) { S.VideoQuit(); S.Quit(); }
    if (S.library) dlclose(S.library);
    if (S.gdk_backend_set) {
        if (S.previous_gdk_backend) setenv("GDK_BACKEND", S.previous_gdk_backend, 1);
        else unsetenv("GDK_BACKEND");
    }
    free(S.previous_gdk_backend);
    memset(&S, 0, sizeof(S));
}

int sdlplat_open(int w, int h, int scale)
{
    memset(&S, 0, sizeof(S));
    S.library = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!S.library) {
        fprintf(stderr, "mini-space-shooter: native Wayland needs the SDL2 runtime (libsdl2-2.0-0).\n");
        return -1;
    }
#define LOAD(name) do { \
        void *symbol = dlsym(S.library, "SDL_" #name); \
        _Static_assert(sizeof(symbol) == sizeof(S.name), "function pointer ABI"); \
        if (!symbol) { fprintf(stderr, "mini-space-shooter: missing SDL_%s\n", #name); goto fail; } \
        memcpy(&S.name, &symbol, sizeof(symbol)); \
    } while (0)
    LOAD(VideoInit); LOAD(VideoQuit); LOAD(Quit); LOAD(GetError); LOAD(SetHint);
    LOAD(GetDisplayUsableBounds); LOAD(CreateWindow); LOAD(DestroyWindow);
    LOAD(GetWindowFlags); LOAD(SetWindowSize); LOAD(GetWindowSurface); LOAD(UpdateWindowSurface);
    LOAD(CreateRGBSurfaceFrom); LOAD(FreeSurface); LOAD(UpperBlitScaled);
    LOAD(PollEvent); LOAD(EventState); LOAD(RaiseWindow);
#undef LOAD
    /* libdecor's GTK plugin must use the same display protocol as SDL. Some
     * desktop launchers export GDK_BACKEND=x11, which otherwise strips the
     * native window's decorations. This change is local to this process. */
    const char *gdk_backend = getenv("GDK_BACKEND");
    if (gdk_backend) {
        S.previous_gdk_backend = strdup(gdk_backend);
        if (!S.previous_gdk_backend) goto fail;
    }
    if (setenv("GDK_BACKEND", "wayland", 1) != 0) goto fail;
    S.gdk_backend_set = 1;
    S.SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "mini-space-shooter");
    if (S.VideoInit("wayland") != 0) {
        fprintf(stderr, "mini-space-shooter: Wayland initialization failed: %s\n", S.GetError());
        S.VideoQuit(); S.Quit();
        goto fail;
    }
    S.video_ready = 1;
    SDL_Rect bounds = {0, 0, 1280, 800};
    S.GetDisplayUsableBounds(0, &bounds);
    S.max_scale = 1;
    for (int i = 2; i <= 6; ++i) {
        if (w * i <= bounds.w * 95 / 100 && h * i <= bounds.h * 88 / 100) S.max_scale = i;
    }
    if (scale <= 0) scale = S.max_scale < 4 ? S.max_scale : 4;
    if (scale > S.max_scale) scale = S.max_scale;
    S.w = w; S.h = h; S.scale = scale;
    S.window = S.CreateWindow("Mini Space Shooter", SDL_WINDOW_CENTERED, SDL_WINDOW_CENTERED,
                              w * scale, h * scale, SDL_WINDOW_SHOWN);
    if (!S.window) goto video_fail;
    S.pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!S.pixels) goto video_fail;
    S.source = S.CreateRGBSurfaceFrom(S.pixels, w, h, 32, w * 4, 0x00ff0000, 0x0000ff00, 0x000000ff, 0);
    if (!S.source || !S.GetWindowSurface(S.window)) goto video_fail;
    /* Drop payloads are heap-owned in SDL; this game does not accept drops. */
    for (uint32_t type = 0x1000; type <= 0x1003; ++type) S.EventState(type, 0);
    S.EventState(0x305, 0); /* extended text-editing payload */
    fprintf(stderr, "mini-space-shooter: native Wayland window (%dx%d, x%d).\n", w * scale, h * scale, scale);
    return 0;
video_fail:
    fprintf(stderr, "mini-space-shooter: Wayland window failed: %s\n", S.GetError());
fail:
    sdlplat_close();
    return -1;
}

static void read_keys(uint8_t *down)
{
    /* SDL2 scancodes follow the USB keyboard usage table, independent of layout. */
    down[PK_LEFT] = S.keys[80] || S.keys[4];
    down[PK_RIGHT] = S.keys[79] || S.keys[7];
    down[PK_UP] = S.keys[82] || S.keys[26];
    down[PK_DOWN] = S.keys[81] || S.keys[22];
    down[PK_FIRE] = S.keys[44] || S.keys[29] || S.keys[13];
    down[PK_ENTER] = S.keys[40] || S.keys[88];
    down[PK_BACK] = S.keys[41];
    down[PK_PAUSE] = S.keys[19];
    down[PK_TAB] = S.keys[43];
    down[PK_MUTE] = S.keys[16];
    down[PK_VOL_DOWN] = S.keys[47];
    down[PK_VOL_UP] = S.keys[48];
    down[PK_SCALE_DOWN] = S.keys[45] || S.keys[59];
    down[PK_SCALE_UP] = S.keys[46] || S.keys[60];
    down[PK_DEBUG] = S.keys[58];
    down[PK_MUSIC] = S.keys[17];
    for (int i = 0; i < 4; ++i) down[PK_ALLY_1 + i] = S.keys[30 + i] || S.keys[89 + i];
}

void sdlplat_poll(PlatInput *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->down, S.down, sizeof(S.down));
    out->quit = S.failed;
    SDL_Event event;
    while (S.PollEvent(&event)) {
        if (event.type == SDL_QUIT_EVENT) out->quit = 1;
        if (event.type == SDL_WINDOW_EVENT && event.window.event == SDL_WINDOW_CLOSE) out->quit = 1;
        if (event.type == SDL_WINDOW_EVENT && event.window.event == SDL_FOCUS_LOST) memset(S.keys, 0, sizeof(S.keys));
        if (event.type == SDL_KEY_DOWN || event.type == SDL_KEY_UP) {
            int code = event.key.keysym.scancode;
            if (code >= 0 && code < 512) S.keys[code] = (uint8_t)(event.type == SDL_KEY_DOWN);
        }
        read_keys(out->down);
        for (int i = 0; i < PK_COUNT; ++i) {
            out->pressed[i] |= (uint8_t)(out->down[i] && !S.down[i]);
            out->released[i] |= (uint8_t)(!out->down[i] && S.down[i]);
        }
        memcpy(S.down, out->down, sizeof(S.down));
    }
    int scale = S.scale + (out->pressed[PK_SCALE_UP] ? 1 : 0) - (out->pressed[PK_SCALE_DOWN] ? 1 : 0);
    if (scale < 1) scale = 1;
    if (scale > S.max_scale) scale = S.max_scale;
    if (scale != S.scale) {
        S.scale = scale;
        S.SetWindowSize(S.window, S.w * scale, S.h * scale);
    }
    out->scale = S.scale;
}

void sdlplat_present(const uint32_t *pixels, int w, int h)
{
    if (S.failed || w != S.w || h != S.h || !pixels) return;
    memcpy(S.pixels, pixels, (size_t)w * (size_t)h * sizeof(uint32_t));
    /* Re-query after every resize; the window owns and may replace its surface. */
    SDL_Surface *target = S.GetWindowSurface(S.window);
    if (!target || S.UpperBlitScaled(S.source, NULL, target, NULL) != 0 || S.UpdateWindowSurface(S.window) != 0) {
        fprintf(stderr, "mini-space-shooter: Wayland presentation failed: %s\n", S.GetError());
        S.failed = 1;
    }
}

int sdlplat_scale(void) { return S.scale; }
int sdlplat_focus(void)
{
    S.RaiseWindow(S.window);
    return (S.GetWindowFlags(S.window) & SDL_INPUT_FOCUS) ? 0 : -1;
}
void sdlplat_mode(char *out, int cap)
{
    if (cap > 0) snprintf(out, (size_t)cap, "wayland %dx%d (x%d)", S.w * S.scale, S.h * S.scale, S.scale);
}
