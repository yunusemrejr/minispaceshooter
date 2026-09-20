/* art.hpp — every pixel in this game is authored here as ASCII art.
 *
 * Sprites are stored as readable strings (one char per pixel) and rasterised
 * once at startup into 0x00RRGGBB buffers where 0x000000 means transparent.
 * `art_init()` returns the number of malformed rows it found, which is how the
 * self-tests prove the art tables stayed rectangular while being edited.
 */
#ifndef MINI_ART_HPP
#define MINI_ART_HPP

#include <cstdint>

#include "save.hpp" /* MEDAL_COUNT */

namespace mss {

struct Sprite {
    uint32_t *px;
    int w, h;
};

namespace art {

extern Sprite player;
extern Sprite ally[4];
extern Sprite bullet_ally;
extern Sprite enemy_grunt;
extern Sprite enemy_wasp;
extern Sprite enemy_brute;
extern Sprite enemy_ghost;

extern Sprite bullet_player;
extern Sprite bullet_enemy;
extern Sprite bullet_big;

extern Sprite medal[MEDAL_COUNT];
extern Sprite heart_full;
extern Sprite heart_empty;

extern Sprite spark;
extern Sprite flash_small;
extern Sprite flash_big;

/* Rasterises every table.  Returns the count of rows whose length did not
 * match the declared sprite width (0 means every table is well formed). */
int art_init(void);

/* Palette lookup used by the ASCII art ('\0' for transparent). */
uint32_t color(char c);

} /* namespace art */
} /* namespace mss */

#endif /* MINI_ART_HPP */
