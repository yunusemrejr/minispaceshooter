/* minui.h — a tiny immediate-mode pixel-art UI toolkit (C11, no dependencies).
 *
 * Design notes
 *   * Immediate mode: widgets are drawn and their interaction result returned
 *     in one call.  Persistent state (selection index, animations) lives in
 *     caller-owned structs, so there is no hidden global UI state.
 *   * Everything renders into a plain 32-bit framebuffer (0x00RRGGBB).
 *   * Drawing is clipped once at the compositor level; widgets assume they are
 *     called with sane rectangles inside the framebuffer.
 *   * The palette font is 5x7 (font5x7.c) and supports integer scaling.
 */
#ifndef MINI_MINUI_H
#define MINI_MINUI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define MUI_RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))
#define MUI_MAX_MENU_ITEMS 8

/* ------------------------------------------------------------------ canvas */
typedef struct {
    uint32_t *px;
    int w, h;
} MuiFb;

typedef struct {
    uint32_t bg, panel, panel_hi, panel_edge, shadow;
    uint32_t text, text_dim, text_strong;
    uint32_t accent, accent2, good, warn, danger;
} MuiTheme;

/* Per-frame input edges, supplied by the platform layer. */
typedef struct {
    int up, down, left, right, confirm, back;
} MuiInput;

typedef struct {
    MuiFb fb;
    MuiTheme th;
    MuiInput in;
    float now; /* seconds, drives animation and blinking */
    int cursor_visible;
} Mui;

MuiTheme mui_theme_default(void);

void mui_begin(Mui *m, uint32_t *pixels, int w, int h, MuiTheme th, MuiInput in, float time);

/* ------------------------------------------------------------- primitives */
void mui_clear(Mui *m, uint32_t color);
void mui_px(Mui *m, int x, int y, uint32_t color);
void mui_hline(Mui *m, int x, int y, int w, uint32_t color);
void mui_vline(Mui *m, int x, int y, int h, uint32_t color);
void mui_rect(Mui *m, int x, int y, int w, int h, uint32_t color);
void mui_rect_outline(Mui *m, int x, int y, int w, int h, uint32_t color);
/* Alpha = 0..255, blends toward color. */
void mui_rect_blend(Mui *m, int x, int y, int w, int h, uint32_t color, int alpha);
void mui_noise_rect(Mui *m, int x, int y, int w, int h, uint32_t color, int alpha, uint32_t seed);

/* Sprite blit; 0x000000 is treated as transparent unless opaque != 0. */
void mui_blit(Mui *m, const uint32_t *sprite, int sw, int sh, int x, int y, int opaque);
void mui_blit_tint(Mui *m, const uint32_t *sprite, int sw, int sh, int x, int y, uint32_t tint, int tint_alpha);
void mui_blit_scaled(Mui *m, const uint32_t *sprite, int sw, int sh, int x, int y, int scale, int opaque);

/* ------------------------------------------------------------------- text */
int mui_text_w(const char *s, int scale);
int mui_text_h(int scale);
void mui_text(Mui *m, int x, int y, const char *s, uint32_t color, int scale);
void mui_text_center(Mui *m, int cx, int y, const char *s, uint32_t color, int scale);
void mui_text_right(Mui *m, int rx, int y, const char *s, uint32_t color, int scale);
void mui_text_shadow(Mui *m, int x, int y, const char *s, uint32_t color, int scale);
void mui_textf(Mui *m, int x, int y, uint32_t color, int scale, const char *fmt, ...);

/* --------------------------------------------------------------- composed */
/* Panel with 1px edge, inner highlight and drop shadow. */
void mui_panel(Mui *m, int x, int y, int w, int h);
/* Panel with a centred title bar; returns the first usable body y. */
int mui_panel_titled(Mui *m, int x, int y, int w, int h, const char *title);
void mui_bar(Mui *m, int x, int y, int w, int h, float frac, uint32_t fill, uint32_t back);
void mui_divider(Mui *m, int x, int y, int w, uint32_t color);
/* Key hint row: draws the key in a chip and its label next to it. */
void mui_hint(Mui *m, int x, int y, const char *key, const char *label);
int mui_hint_w(const char *key, const char *label);
/* Blinking cursor used on menus and "press enter" prompts. */
void mui_prompt(Mui *m, int cx, int y, const char *text, uint32_t color, int scale);

/* ------------------------------------------------------------------- menu */
typedef struct {
    int index; /* selected row */
    int count; /* rows in the menu (managed by mui_menu_begin) */
    float glow[MUI_MAX_MENU_ITEMS];
} MuiMenu;

/* MuiMenu holds state between frames, so the caller MUST zero-initialise it
 * once (`MuiMenu menu = {0};` in C, `MuiMenu menu{};` in C++) before the first
 * mui_menu_begin().  Everything else in minui is stateless. */
void mui_menu_begin(MuiMenu *menu, int count);
/* Draws one item; returns 1 when it was activated this frame. */
int mui_menu_item(Mui *m, MuiMenu *menu, int idx, int x, int y, int w, int h, const char *label, const char *value,
                  int enabled);
/* Handles up/down navigation and wraps. Call before mui_menu_item rows. */
void mui_menu_nav(Mui *m, MuiMenu *menu);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MINI_MINUI_H */
