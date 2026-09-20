/* font5x7.h — tiny built-in 5x7 pixel font for Mini Space Shooter.
 *
 * The glyph data lives in font5x7.c as readable ASCII art ('.' = empty,
 * '#' = lit pixel) that is rasterised once, lazily, into column bitmaps.
 *
 * Layout contract used by the renderer (src/c/minui.c):
 *   - Each glyph is FONT_W (5) columns wide, FONT_H (7) rows tall.
 *   - Bit 0 of a column byte is the TOP row, bit 6 is the BOTTOM row.
 *   - Printable ASCII only: FONT_FIRST (' ') .. FONT_LAST ('~').
 *   - Characters outside that range render as '?'.
 */
#ifndef MINI_FONT5X7_H
#define MINI_FONT5X7_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define FONT_FIRST 32
#define FONT_LAST 126
#define FONT_COUNT (FONT_LAST - FONT_FIRST + 1) /* 95 printable glyphs */
#define FONT_W 5
#define FONT_H 7

/* Returns FONT_W column bytes for `ch`, or the '?' glyph when out of range.
 * Never returns NULL. Thread-safe after the first call (idempotent build). */
const uint8_t *font5x7_glyph(int ch);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MINI_FONT5X7_H */
