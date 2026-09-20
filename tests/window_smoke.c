/* Optional real-display regression. Never loads or writes a game save.
 * SDL_PushEvent uses the real runtime event queue, exercising the native adapter.
 */
#define _POSIX_C_SOURCE 200809L
#include "platform.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t pixels[384 * 216];
typedef union { uint32_t type; uint64_t align; unsigned char bytes[56]; } Event;
static int (*push_event)(Event *);

static void key_event(int code, int down)
{
    Event event = {0};
    event.type = down ? 0x300 : 0x301;
    event.bytes[12] = (unsigned char)down;
    memcpy(event.bytes + 16, &code, sizeof(code));
    if (push_event(&event) != 1) { fprintf(stderr, "could not queue keyboard event\n"); exit(1); }
}

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); plat_close(); return 1; } } while (0)

int main(void)
{
    CHECK(plat_open(384, 216, 2, 0) == 0);
    char mode[80];
    plat_mode_label(mode, sizeof(mode));
    printf("window smoke: %s\n", mode);
    for (int y = 0; y < 216; ++y) {
        for (int x = 0; x < 384; ++x) pixels[y * 384 + x] = ((x / 16 + y / 16) % 2) ? 0x20b8d0 : 0x182840;
    }
    PlatInput input;
    for (int i = 0; i < 20; ++i) { plat_poll(&input); plat_present(pixels, 384, 216); plat_sleep(0.02); }
    if (strncmp(mode, "wayland", 7) != 0) {
        int matched = plat_verify_present();
        CHECK(matched >= 0);
        CHECK(matched >= 384 * 216 * plat_scale() * plat_scale() * 99 / 100);
        plat_close();
        puts("ok - X11 visible window and pixel read-back");
        return 0;
    }
    void *lib = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    CHECK(lib != NULL);
    void *symbol = dlsym(lib, "SDL_PushEvent");
    CHECK(symbol != NULL);
    memcpy(&push_event, &symbol, sizeof(symbol));

    key_event(80, 1); plat_poll(&input);
    CHECK(input.down[PK_LEFT] && input.pressed[PK_LEFT]);
    key_event(4, 1); key_event(80, 0); plat_poll(&input);
    CHECK(input.down[PK_LEFT] && !input.released[PK_LEFT]); /* alternate key still held */
    key_event(4, 0); plat_poll(&input);
    CHECK(!input.down[PK_LEFT] && input.released[PK_LEFT]);
    key_event(44, 1); key_event(44, 0); plat_poll(&input);
    CHECK(input.pressed[PK_FIRE] && input.released[PK_FIRE] && !input.down[PK_FIRE]);
    puts("ok - movement aliases and quick fire taps");

    for (int k = 0; k < 4; ++k) {
        key_event(30 + k, 1); plat_poll(&input);
        CHECK(input.pressed[PK_ALLY_1 + k]);
        plat_poll(&input); CHECK(input.down[PK_ALLY_1 + k] && !input.pressed[PK_ALLY_1 + k]);
        key_event(30 + k, 0); plat_poll(&input);
        key_event(89 + k, 1); key_event(89 + k, 0); plat_poll(&input);
        CHECK(input.pressed[PK_ALLY_1 + k] && input.released[PK_ALLY_1 + k]);
    }
    key_event(17, 1); key_event(17, 0); plat_poll(&input); CHECK(input.pressed[PK_MUSIC]);
    puts("ok - 1-4 purchases, held-key edges, keypad and music toggle");

    int scale = plat_scale();
    key_event(59, 1); plat_poll(&input);
    CHECK(plat_scale() == (scale > 1 ? scale - 1 : 1));
    key_event(59, 0); plat_poll(&input);
    plat_present(pixels, 384, 216);
    key_event(60, 1); plat_poll(&input);
    CHECK(plat_scale() == scale);
    key_event(60, 0); plat_poll(&input);
    plat_present(pixels, 384, 216);
    puts("ok - scale down/up and resized surface presentation");

    key_event(80, 1); plat_poll(&input);
    Event event = {0}; event.type = 0x200; event.bytes[12] = 13;
    CHECK(push_event(&event) == 1); plat_poll(&input);
    CHECK(!input.down[PK_LEFT] && input.released[PK_LEFT]);
    puts("ok - focus loss clears held controls");
    event.bytes[12] = 14;
    CHECK(push_event(&event) == 1); plat_poll(&input);
    CHECK(input.quit);
    plat_close();
    dlclose(lib);
    puts("ok - window close and cleanup");
    return 0;
}
