/* minui.c — implementation of the tiny pixel-art UI toolkit. */
#include "minui.h"

#include "font5x7.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ theme */
MuiTheme mui_theme_default(void)
{
    MuiTheme t;
    t.bg = MUI_RGB(0x07, 0x08, 0x0F);
    t.panel = MUI_RGB(0x0F, 0x12, 0x1E);
    t.panel_hi = MUI_RGB(0x1A, 0x21, 0x36);
    t.panel_edge = MUI_RGB(0x2C, 0x35, 0x55);
    t.shadow = MUI_RGB(0x03, 0x04, 0x08);
    t.text = MUI_RGB(0xE4, 0xE9, 0xF5);
    t.text_dim = MUI_RGB(0x84, 0x8E, 0xAC);
    t.text_strong = MUI_RGB(0xFF, 0xFF, 0xFF);
    t.accent = MUI_RGB(0x3C, 0xD6, 0xE6);
    t.accent2 = MUI_RGB(0xFF, 0x5E, 0xA8);
    t.good = MUI_RGB(0x6C, 0xE0, 0x7A);
    t.warn = MUI_RGB(0xFF, 0xC2, 0x4D);
    t.danger = MUI_RGB(0xFF, 0x4D, 0x5E);
    return t;
}

void mui_begin(Mui *m, uint32_t *pixels, int w, int h, MuiTheme th, MuiInput in, float now)
{
    m->fb.px = pixels;
    m->fb.w = w;
    m->fb.h = h;
    m->th = th;
    m->in = in;
    m->now = now;
    m->cursor_visible = ((int)(now * 2.0f)) % 2 == 0;
}

/* ------------------------------------------------------------- primitives */
void mui_clear(Mui *m, uint32_t color)
{
    int n = m->fb.w * m->fb.h;
    for (int i = 0; i < n; ++i) m->fb.px[i] = color;
}

void mui_px(Mui *m, int x, int y, uint32_t color)
{
    if (x < 0 || y < 0 || x >= m->fb.w || y >= m->fb.h) return;
    m->fb.px[(size_t)y * (size_t)m->fb.w + (size_t)x] = color;
}

void mui_hline(Mui *m, int x, int y, int w, uint32_t color)
{
    if (y < 0 || y >= m->fb.h) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > m->fb.w) w = m->fb.w - x;
    if (w <= 0) return;
    uint32_t *row = m->fb.px + (size_t)y * (size_t)m->fb.w + (size_t)x;
    for (int i = 0; i < w; ++i) row[i] = color;
}

void mui_vline(Mui *m, int x, int y, int h, uint32_t color)
{
    if (x < 0 || x >= m->fb.w) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > m->fb.h) h = m->fb.h - y;
    for (int i = 0; i < h; ++i) m->fb.px[(size_t)(y + i) * (size_t)m->fb.w + (size_t)x] = color;
}

void mui_rect(Mui *m, int x, int y, int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < h; ++i) mui_hline(m, x, y + i, w, color);
}

void mui_rect_outline(Mui *m, int x, int y, int w, int h, uint32_t color)
{
    if (w <= 1 || h <= 1) return;
    mui_hline(m, x, y, w, color);
    mui_hline(m, x, y + h - 1, w, color);
    mui_vline(m, x, y, h, color);
    mui_vline(m, x + w - 1, y, h, color);
}

static uint32_t blend(uint32_t dst, uint32_t src, int alpha)
{
    if (alpha <= 0) return dst;
    if (alpha >= 255) return src;
    int dr = (int)((dst >> 16) & 0xFF), dg = (int)((dst >> 8) & 0xFF), db = (int)(dst & 0xFF);
    int sr = (int)((src >> 16) & 0xFF), sg = (int)((src >> 8) & 0xFF), sb = (int)(src & 0xFF);
    int r = dr + ((sr - dr) * alpha) / 255;
    int g = dg + ((sg - dg) * alpha) / 255;
    int b = db + ((sb - db) * alpha) / 255;
    return MUI_RGB(r, g, b);
}

void mui_rect_blend(Mui *m, int x, int y, int w, int h, uint32_t color, int alpha)
{
    for (int j = y; j < y + h; ++j) {
        if (j < 0 || j >= m->fb.h) continue;
        for (int i = x; i < x + w; ++i) {
            if (i < 0 || i >= m->fb.w) continue;
            uint32_t *p = &m->fb.px[(size_t)j * (size_t)m->fb.w + (size_t)i];
            *p = blend(*p, color, alpha);
        }
    }
}

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

void mui_noise_rect(Mui *m, int x, int y, int w, int h, uint32_t color, int alpha, uint32_t seed)
{
    uint32_t s = seed;
    for (int j = y; j < y + h; ++j) {
        for (int i = x; i < x + w; ++i) {
            s = hash32(s + (uint32_t)i * 73856093u + (uint32_t)j * 19349663u);
            int a = (int)(s & 0xFFu);
            if (a < alpha) {
                if (i < 0 || j < 0 || i >= m->fb.w || j >= m->fb.h) continue;
                uint32_t *p = &m->fb.px[(size_t)j * (size_t)m->fb.w + (size_t)i];
                *p = blend(*p, color, 40 + (int)(s >> 8 & 0x3F));
            }
        }
    }
}

/* --------------------------------------------------------------- sprites */
void mui_blit(Mui *m, const uint32_t *sprite, int sw, int sh, int x, int y, int opaque)
{
    for (int j = 0; j < sh; ++j) {
        int dy = y + j;
        if (dy < 0 || dy >= m->fb.h) continue;
        const uint32_t *src = sprite + (size_t)j * (size_t)sw;
        uint32_t *dst = m->fb.px + (size_t)dy * (size_t)m->fb.w;
        for (int i = 0; i < sw; ++i) {
            int dx = x + i;
            if (dx < 0 || dx >= m->fb.w) continue;
            uint32_t c = src[i];
            if (!opaque && (c & 0x00FFFFFFu) == 0) continue;
            dst[dx] = c;
        }
    }
}

void mui_blit_tint(Mui *m, const uint32_t *sprite, int sw, int sh, int x, int y, uint32_t tint, int tint_alpha)
{
    for (int j = 0; j < sh; ++j) {
        int dy = y + j;
        if (dy < 0 || dy >= m->fb.h) continue;
        const uint32_t *src = sprite + (size_t)j * (size_t)sw;
        uint32_t *dst = m->fb.px + (size_t)dy * (size_t)m->fb.w;
        for (int i = 0; i < sw; ++i) {
            int dx = x + i;
            if (dx < 0 || dx >= m->fb.w) continue;
            uint32_t c = src[i];
            if ((c & 0x00FFFFFFu) == 0) continue;
            dst[dx] = blend(c, tint, tint_alpha);
        }
    }
}

void mui_blit_scaled(Mui *m, const uint32_t *sprite, int sw, int sh, int x, int y, int scale, int opaque)
{
    if (scale <= 0) return;
    for (int j = 0; j < sh; ++j) {
        for (int i = 0; i < sw; ++i) {
            uint32_t c = sprite[(size_t)j * (size_t)sw + (size_t)i];
            if (!opaque && (c & 0x00FFFFFFu) == 0) continue;
            mui_rect(m, x + i * scale, y + j * scale, scale, scale, c);
        }
    }
}

/* ------------------------------------------------------------------ text */
int mui_text_w(const char *s, int scale)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0) return 0;
    int adv = (FONT_W + 1) * scale;
    return (int)n * adv - scale; /* no trailing gap */
}

int mui_text_h(int scale) { return FONT_H * scale; }

/* Draws one glyph with clipping; `scale` >= 1. */
static void glyph(Mui *m, int x, int y, int ch, uint32_t color, int scale)
{
    const uint8_t *cols = font5x7_glyph(ch);
    for (int c = 0; c < FONT_W; ++c) {
        uint8_t bits = cols[c];
        if (!bits) continue;
        for (int r = 0; r < FONT_H; ++r) {
            if (!(bits & (1u << r))) continue;
            if (scale == 1) mui_px(m, x + c, y + r, color);
            else mui_rect(m, x + c * scale, y + r * scale, scale, scale, color);
        }
    }
}

void mui_text(Mui *m, int x, int y, const char *s, uint32_t color, int scale)
{
    if (!s || scale < 1) return;
    int adv = (FONT_W + 1) * scale;
    for (const char *p = s; *p; ++p) {
        if (*p != ' ') glyph(m, x, y, (unsigned char)*p, color, scale);
        x += adv;
    }
}

void mui_text_center(Mui *m, int cx, int y, const char *s, uint32_t color, int scale)
{
    mui_text(m, cx - mui_text_w(s, scale) / 2, y, s, color, scale);
}

void mui_text_right(Mui *m, int rx, int y, const char *s, uint32_t color, int scale)
{
    mui_text(m, rx - mui_text_w(s, scale), y, s, color, scale);
}

void mui_text_shadow(Mui *m, int x, int y, const char *s, uint32_t color, int scale)
{
    mui_text(m, x + scale, y + scale, s, m->th.shadow, scale);
    mui_text(m, x, y, s, color, scale);
}

void mui_textf(Mui *m, int x, int y, uint32_t color, int scale, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mui_text(m, x, y, buf, color, scale);
}

/* --------------------------------------------------------------- composed */
void mui_panel(Mui *m, int x, int y, int w, int h)
{
    mui_rect(m, x + 2, y + 2, w, h, m->th.shadow);
    mui_rect(m, x, y, w, h, m->th.panel);
    mui_rect_outline(m, x, y, w, h, m->th.panel_edge);
    /* inner top-left highlight and bottom-right shade give the chrome depth */
    mui_hline(m, x + 1, y + 1, w - 2, m->th.panel_hi);
    mui_vline(m, x + 1, y + 1, h - 2, m->th.panel_hi);
    /* pixel-art corner ticks */
    mui_px(m, x, y, m->th.accent);
    mui_px(m, x + w - 1, y, m->th.accent);
    mui_px(m, x, y + h - 1, m->th.accent);
    mui_px(m, x + w - 1, y + h - 1, m->th.accent);
}

int mui_panel_titled(Mui *m, int x, int y, int w, int h, const char *title)
{
    mui_panel(m, x, y, w, h);
    mui_rect(m, x + 1, y + 1, w - 2, 11, m->th.panel_hi);
    mui_hline(m, x + 1, y + 12, w - 2, m->th.panel_edge);
    mui_text_center(m, x + w / 2, y + 3, title, m->th.text_strong, 1);
    return y + 15;
}

void mui_bar(Mui *m, int x, int y, int w, int h, float frac, uint32_t fill, uint32_t back)
{
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    mui_rect(m, x, y, w, h, back);
    int fw = (int)(frac * (float)(w - 2) + 0.5f);
    if (fw > 0) mui_rect(m, x + 1, y + 1, fw, h - 2, fill);
    mui_rect_outline(m, x, y, w, h, m->th.panel_edge);
}

void mui_divider(Mui *m, int x, int y, int w, uint32_t color)
{
    mui_hline(m, x, y, w, color);
    mui_hline(m, x + 1, y + 1, w - 2, m->th.shadow);
}

int mui_hint_w(const char *key, const char *label)
{
    return mui_text_w(key, 1) + 6 + mui_text_w(label, 1);
}

void mui_hint(Mui *m, int x, int y, const char *key, const char *label)
{
    int kw = mui_text_w(key, 1) + 4;
    mui_rect(m, x, y - 1, kw, 9, m->th.panel_hi);
    mui_rect_outline(m, x, y - 1, kw, 9, m->th.panel_edge);
    mui_text(m, x + 2, y, key, m->th.text_strong, 1);
    mui_text(m, x + kw + 3, y, label, m->th.text_dim, 1);
}

void mui_prompt(Mui *m, int cx, int y, const char *text, uint32_t color, int scale)
{
    int w = mui_text_w(text, scale);
    mui_text(m, cx - w / 2, y, text, color, scale);
    if (m->cursor_visible) {
        int dot = 3 * scale;
        mui_rect(m, cx + w / 2 + 2 * scale, y, dot, dot, color);
    }
}

/* ------------------------------------------------------------------- menu */
void mui_menu_begin(MuiMenu *menu, int count)
{
    /* Deliberately does not read the previous count: the menu object is
     * program state (see the contract in minui.h), and reading it here would
     * be a use of uninitialised memory if a caller forgot to zero-initialise. */
    if (count > MUI_MAX_MENU_ITEMS) count = MUI_MAX_MENU_ITEMS;
    if (count < 0) count = 0;
    menu->count = count;
    if (menu->index >= count) menu->index = count > 0 ? count - 1 : 0;
    if (menu->index < 0) menu->index = 0;
}

void mui_menu_nav(Mui *m, MuiMenu *menu)
{
    if (menu->count <= 0) return;
    if (m->in.up) menu->index = (menu->index + menu->count - 1) % menu->count;
    if (m->in.down) menu->index = (menu->index + 1) % menu->count;
}

int mui_menu_item(Mui *m, MuiMenu *menu, int idx, int x, int y, int w, int h, const char *label, const char *value,
                  int enabled)
{
    int sel = (idx == menu->index);
    float g = menu->glow[idx];
    /* clamp: also turns a NaN from any uninitialised caller into 0 */
    if (!(g >= 0.0f) || g > 4.0f) g = 0.0f;
    if (sel) g += (1.0f - g) * 0.35f;
    else g += (0.0f - g) * 0.25f;
    menu->glow[idx] = g;

    uint32_t bg = sel ? m->th.panel_hi : m->th.panel;
    mui_rect(m, x, y, w, h, bg);
    if (g > 0.02f) {
        int a = (int)(g * 40.0f);
        mui_rect_blend(m, x, y, w, h, m->th.accent, a);
    }
    mui_rect_outline(m, x, y, w, h, sel ? m->th.accent : m->th.panel_edge);

    uint32_t fg = enabled ? (sel ? m->th.text_strong : m->th.text) : m->th.text_dim;
    mui_text(m, x + 8, y + (h - FONT_H) / 2, label, fg, 1);
    if (value && *value) {
        mui_text_right(m, x + w - 8, y + (h - FONT_H) / 2, value, sel ? m->th.accent2 : m->th.text_dim, 1);
    }
    if (sel) {
        int pulse = (int)(g * 3.0f);
        for (int i = 0; i < 3; ++i) {
            mui_px(m, x + 2 + i, y + h / 2 - 1 - i + pulse % 2, m->th.accent);
            mui_px(m, x + 2 + i, y + h / 2 + 1 + i - pulse % 2, m->th.accent);
            mui_px(m, x + 2 + i, y + h / 2, m->th.accent);
        }
    }
    return (sel && enabled && m->in.confirm) ? 1 : 0;
}
