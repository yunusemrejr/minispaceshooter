/* main.cpp — screens, main loop, frame pacing and the offscreen shot mode.
 *
 * Screens: title / play / pause / game-over / leaderboard / medal wall /
 * controls.  Rendering is immediate-mode through minui, the simulation runs on
 * a fixed 1/60 s step, and the loop sleeps instead of spinning so an idle
 * frame costs almost nothing.
 *
 * Command line:
 *   --shot DIR     render every screen offscreen to DIR (PPM frames) and exit
 *   --frames N     run N frames then exit (packaged smoke test)
 *   --scale N      force an integer pixel scale (window opens at 384*N x 216*N)
 *   --seed N       deterministic run seed
 *   --debug        start with the ML/difficulty overlay on
 *   --level N      start the (shot/frame) run at level N
 *   --reset-save   wipe scores and medals back to defaults, then exit
 *   --autoplay     drive a scripted run through the real window, verify the
 *                  state machine and gameplay wiring, then exit (non-zero on
 *                  failure).  Used as the packaged integration check.
 *   --focus-self   ask the X server for keyboard focus (helps the key test)
 *   --verify-present N  present N frames, then read the window back from the
 *                  X server and compare it with what was sent (exit non-zero
 *                  when the pixels do not match); proves the presentation
 *                  path, including the XShm handshake
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include "art.hpp"
#include "audio.h"
#include "game.hpp"
#include "minui.h"
#include "platform.h"
#include "rng.hpp"
#include "save.hpp"

using namespace mss;

namespace {

constexpr int IW = 384;
constexpr int IH = 216;
constexpr float STEP = 1.0f / 60.0f;

uint32_t g_pixels[IW * IH];
uint32_t g_scaled[(IW * 2) * (IH * 2)];

enum Screen : int {
    SC_TITLE = 0,
    SC_PLAY,
    SC_PAUSE,
    SC_OVER,
    SC_BOARD,
    SC_MEDALS,
    SC_CONTROLS
};

const char *screen_name(int s)
{
    switch (s) {
    case SC_TITLE: return "title";
    case SC_PLAY: return "play";
    case SC_PAUSE: return "pause";
    case SC_OVER: return "gameover";
    case SC_BOARD: return "leaderboard";
    case SC_MEDALS: return "medals";
    case SC_CONTROLS: return "controls";
    default: return "?";
    }
}

struct App {
    Game game;
    SaveData save;
    Rng rng;
    Mui m;
    MuiTheme theme;
    /* Zero-initialised: minui keeps selection/animation state in these across
     * frames, and mui_menu_begin() inspects the previous count. */
    MuiMenu title_menu{};
    MuiMenu pause_menu{};
    MuiMenu over_menu{};

    int screen = SC_TITLE;
    int prev_screen = SC_TITLE;
    bool debug = false;
    bool running = true;
    bool run_recorded = false;
    bool new_record = false;
    bool persist_save = true;
    bool save_failed = false;
    int record_at_start = 0;
    int last_board_pos = -1;
    int last_medal_up = MEDAL_NONE;

    double now = 0.0;
    double fps = 0.0;
    float fps_accum = 0.0f;
    int fps_frames = 0;
    float ui_sound_cd = 0.0f;
};

void on_run_finished(App &a);

/* ------------------------------------------------------------------ chrome */
void draw_title_art(Mui &m, float t)
{
    /* Wordmark: two sizes of the same 5x7 font keeps it chunky and readable. */
    int cx = IW / 2;
    mui_text_center(&m, cx, 22, "MINI SPACE", m.th.text_dim, 2);
    mui_text_shadow(&m, cx - mui_text_w("SHOOTER", 4) / 2, 36, "SHOOTER", m.th.text_strong, 4);
    mui_text_center(&m, cx, 66, "ENDLESS  //  ADAPTIVE", m.th.accent, 1);
    /* a slow scanning line under the logo for a little life */
    int sx = (int)((t * 40.0f));
    sx = sx % (IW + 60) - 30;
    mui_hline(&m, sx, 74, 30, m.th.accent);
}

void draw_medal_icon(Mui &m, int tier, int x, int y, int scale)
{
    const Sprite &s = art::medal[tier];
    if (scale <= 1) mui_blit(&m, s.px, s.w, s.h, x, y, 0);
    else mui_blit_scaled(&m, s.px, s.w, s.h, x, y, scale, 0);
}

void draw_footer_hints(Mui &m, const char *text)
{
    mui_rect(&m, 0, IH - 11, IW, 11, MUI_RGB(0x05, 0x06, 0x0C));
    mui_hline(&m, 0, IH - 11, IW, m.th.panel_edge);
    mui_text(&m, 4, IH - 9, text, m.th.text_dim, 1);
}

/* ------------------------------------------------------------ screen: title */
void render_title(App &a)
{
    Mui &m = a.m;
    a.game.draw_attract(m);
    mui_rect_blend(&m, 0, 0, IW, 88, MUI_RGB(0x04, 0x05, 0x0A), 150);
    draw_title_art(m, (float)a.now);

    int n = 5;
    mui_menu_begin(&a.title_menu, n);
    mui_menu_nav(&m, &a.title_menu);
    int x = 96, w = 192, y = 92, h = 15;
    /* one backing panel for the whole menu: keeps the items readable over the
     * starfield and hides the attract ship drifting behind the rows */
    mui_panel(&m, x - 6, y - 6, w + 12, (h + 3) * n + 6);
    char best[32];
    std::snprintf(best, sizeof(best), "%d", a.save.best_score);
    int chosen = -1;
    if (mui_menu_item(&m, &a.title_menu, 0, x, y, w, h, "START RUN", a.save.best_score > 0 ? best : "", 1)) chosen = 0;
    if (mui_menu_item(&m, &a.title_menu, 1, x, y + h + 3, w, h, "LEADERBOARD", "TOP 10", 1)) chosen = 1;
    if (mui_menu_item(&m, &a.title_menu, 2, x, y + (h + 3) * 2, w, h, "MEDAL WALL", medal_short(4), 1)) chosen = 2;
    if (mui_menu_item(&m, &a.title_menu, 3, x, y + (h + 3) * 3, w, h, "CONTROLS", "", 1)) chosen = 3;
    if (mui_menu_item(&m, &a.title_menu, 4, x, y + (h + 3) * 4, w, h, "QUIT", "", 1)) chosen = 4;

    mui_text_center(&m, IW / 2, 178, "the game learns how you fly", m.th.text_dim, 1);
    mui_text_center(&m, IW / 2, 188, "and never lets the ceiling end", m.th.text_dim, 1);
    draw_footer_hints(m, "ARROWS MOVE   SPACE FIRE   TAB COMMAND   ESC PAUSE   F1 ML DEBUG   M MUTE");

    if (chosen >= 0) {
        aud_play(SFX_UI, 1.0f);
        switch (chosen) {
        case 0:
            a.record_at_start = a.save.best_score;
            a.game.reset(a.rng, 1, a.save.best_score, a.save.runs > 0);
            a.run_recorded = false;
            a.new_record = false;
            a.last_board_pos = -1;
            a.last_medal_up = MEDAL_NONE;
            a.screen = SC_PLAY;
            break;
        case 1: a.screen = SC_BOARD; break;
        case 2: a.screen = SC_MEDALS; break;
        case 3: a.screen = SC_CONTROLS; break;
        default: a.running = false; break;
        }
    }
}

/* ----------------------------------------------------------- screen: board */
void render_board(App &a)
{
    Mui &m = a.m;
    mui_clear(&m, m.th.bg);
    mui_noise_rect(&m, 0, 0, IW, IH, MUI_RGB(0x1A, 0x22, 0x40), 40, 12345u);
    int body = mui_panel_titled(&m, 6, 4, IW - 12, IH - 18, "LEADERBOARD");

    mui_text(&m, 14, body + 2, " #   SCORE     LEVEL   MEDALS      DATE", m.th.text_dim, 1);
    mui_divider(&m, 12, body + 11, IW - 24, m.th.panel_edge);

    if (a.save.board_count == 0) {
        mui_text_center(&m, IW / 2, body + 48, "NO RUNS RECORDED YET", m.th.text_dim, 1);
        mui_text_center(&m, IW / 2, body + 60, "PRESS ENTER TO FLY", m.th.text, 1);
    }
    for (int i = 0; i < a.save.board_count && i < LEADERBOARD_SIZE; ++i) {
        const LeaderEntry &e = a.save.board[i];
        int y = body + 15 + i * 12;
        bool fresh = (i == a.last_board_pos);
        uint32_t col = fresh ? m.th.warn : (i == 0 ? m.th.accent : m.th.text);
        if (fresh) mui_rect_blend(&m, 12, y - 2, IW - 24, 11, m.th.warn, 26);
        mui_textf(&m, 14, y, col, 1, "%2d", i + 1);
        mui_textf(&m, 40, y, col, 1, "%7d", e.score);
        mui_textf(&m, 108, y, m.th.text_dim, 1, "LV%02d", e.level);
    /* medals: just the top tier reached plus how many were earned, so the
     * date column stays readable even for a five-medal run */
    int top = MEDAL_NONE, count = 0;
    for (int k = MEDAL_COUNT - 1; k >= 0; --k) {
        if (e.medal_mask & (1 << k)) {
            if (top == MEDAL_NONE) top = k;
            count++;
        }
    }
    char mbuf[24];
    if (top == MEDAL_NONE) std::snprintf(mbuf, sizeof(mbuf), "-");
    else std::snprintf(mbuf, sizeof(mbuf), "%s x%d", medal_short(top), count);
    mui_text(&m, 150, y, mbuf, fresh ? m.th.warn : m.th.text_dim, 1);
    uint32_t d = e.date;
    mui_textf(&m, 240, y, m.th.text_dim, 1, "%04u-%02u-%02u", d / 10000u, (d / 100u) % 100u, d % 100u);
        char tbuf[16];
        std::snprintf(tbuf, sizeof(tbuf), "%d:%02d", e.seconds / 60, e.seconds % 60);
        mui_text_right(&m, IW - 16, y, tbuf, m.th.text_dim, 1);
    }

    mui_textf(&m, 14, IH - 22, m.th.text_dim, 1, "BEST %d   RUNS %d   KILLS %d   DEATHS %d", a.save.best_score, a.save.runs,
              a.save.kills, a.save.deaths);
    draw_footer_hints(m, "ESC / ENTER  BACK TO TITLE");
    if (m.in.back || m.in.confirm) {
        aud_play(SFX_UI, 0.95f);
        a.screen = SC_TITLE;
    }
}

/* ---------------------------------------------------------- screen: medals */
void render_medals(App &a)
{
    Mui &m = a.m;
    mui_clear(&m, m.th.bg);
    mui_noise_rect(&m, 0, 0, IW, IH, MUI_RGB(0x2A, 0x1E, 0x14), 30, 999u);
    int body = mui_panel_titled(&m, 6, 4, IW - 12, IH - 18, "MEDAL WALL  (SCORES MOVE AS YOU IMPROVE)");

    mui_text(&m, 14, body + 1, "MEDAL      NOW        BEST EVER     EARNED", m.th.text_dim, 1);
    mui_divider(&m, 12, body + 10, IW - 24, m.th.panel_edge);

    for (int i = MEDAL_COUNT - 1; i >= 0; --i) {
        int row = (MEDAL_COUNT - 1) - i;
        int y = body + 14 + row * 17;
        const MedalInfo &mi = a.save.medal[i];
        bool earned = a.save.best_score >= mi.threshold && mi.threshold > 0;
        draw_medal_icon(m, i, 14, y - 2, 1);
        mui_text(&m, 30, y + 1, medal_name(i), earned ? m.th.text_strong : m.th.text_dim, 1);
        if (earned) mui_rect_blend(&m, 12, y - 3, 358, 13, m.th.accent, 12);
        mui_textf(&m, 100, y + 1, earned ? m.th.warn : m.th.text_dim, 1, "%7d", mi.threshold);
        mui_textf(&m, 172, y + 1, m.th.text, 1, "%7d", mi.best_threshold);
        uint32_t d = mi.best_date;
        if (mi.best_threshold > 0) {
            mui_textf(&m, 240, y + 1, m.th.text_dim, 1, "since %04u-%02u-%02u", d / 10000u, (d / 100u) % 100u, d % 100u);
        }
        mui_textf(&m, 340, y + 1, m.th.text_dim, 1, "x%d", mi.earned_count);
    }

    int hy = body + 14 + MEDAL_COUNT * 17 + 2;
    mui_hline(&m, 12, hy, IW - 24, m.th.panel_edge);
    mui_text(&m, 14, hy + 3, "PAST RECORDS  (what each medal used to mean)", m.th.text_dim, 1);
    int shown = 0;
    for (int i = a.save.history_count - 1; i >= 0 && shown < 3; --i, ++shown) {
        const HistEntry &h = a.save.history[i];
        int y = hy + 13 + shown * 9;
        mui_textf(&m, 22, y, m.th.text, 1, "%s  %d", medal_name(h.tier), h.score);
        uint32_t d = h.date;
        mui_textf(&m, 120, y, m.th.text_dim, 1, "on %04u-%02u-%02u", d / 10000u, (d / 100u) % 100u, d % 100u);
    }
    if (a.save.history_count == 0) mui_text(&m, 22, hy + 13, "nothing replaced yet", m.th.text_dim, 1);

    mui_textf(&m, 14, IH - 22, m.th.accent, 1, "RECORD %d   NEXT DIAMOND AT %d", a.save.best_score,
              a.save.medal[MEDAL_DIAMOND].threshold);
    draw_footer_hints(m, "ESC / ENTER  BACK TO TITLE");
    if (m.in.back || m.in.confirm) {
        aud_play(SFX_UI, 0.95f);
        a.screen = SC_TITLE;
    }
}

/* -------------------------------------------------------- screen: controls */
void render_controls(App &a)
{
    Mui &m = a.m;
    mui_clear(&m, m.th.bg);
    int body = mui_panel_titled(&m, 6, 4, IW - 12, IH - 18, "CONTROLS");
    struct Row {
        const char *key;
        const char *text;
    };
    static const Row ROWS[] = {
        {"ARROWS / WASD", "fly your ship"},
        {"SPACE / Z", "fire"},
        {"1 / 2 / 3 / 4", "buy scout / wing / cruiser / titan"},
        {"TAB", "focus / leave the command panel"},
        {"ARROWS + ENTER", "choose and buy a panel option"},
        {"ESC / P", "pause"},
        {"ENTER", "confirm"},
        {"M / N", "mute all / toggle calm music"},
        {"[ ]", "volume down / up"},
        {"- = / F2 F3", "pixel scale"},
        {"F1", "ML + difficulty overlay"},
        {"PANEL", "heal shield, floating base, ship upgrades"},
        {"REPAIR", "+1 heart every 5 levels (up to hull cap)"},
        {"CREDITS", "kills + levels refill; score stays"},
        {"ALLIES", "4 slots; take damage; learn as a team"},
        {"DEATH", "fleet and upgrades reset; record stays"},
    };
    int y = body + 4;
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); ++i, y += 11) {
        if (ROWS[i].key[0] == '\0') continue;
        mui_text(&m, 16, y, ROWS[i].key, m.th.accent, 1);
        mui_text(&m, 130, y, ROWS[i].text, m.th.text, 1);
    }
    mui_text(&m, 16, y + 6, "audio backend", m.th.text_dim, 1);
    char buf[32];
    aud_backend_name(buf, (int)sizeof(buf));
    mui_textf(&m, 130, y + 6, m.th.text, 1, "%s  MUSIC %s  VOL %.0f%%", buf,
              aud_music_enabled() ? "ON" : "OFF", (double)aud_get_master() * 100.0);
    draw_footer_hints(m, "ESC / ENTER  BACK TO TITLE");
    if (m.in.back || m.in.confirm) {
        aud_play(SFX_UI, 0.95f);
        a.screen = SC_TITLE;
    }
}

/* ----------------------------------------------------------- screen: pause */
void render_pause(App &a)
{
    Mui &m = a.m;
    a.game.draw(m, a.debug);
    mui_rect_blend(&m, 0, 0, IW, IH, MUI_RGB(0x04, 0x05, 0x0A), 170);
    int body = mui_panel_titled(&m, 108, 52, 168, 106, "PAUSED");
    (void)body;
    mui_menu_begin(&a.pause_menu, 3);
    mui_menu_nav(&m, &a.pause_menu);
    const RunStats &st = a.game.stats();
    mui_text_center(&m, IW / 2, 72, "SCORE", m.th.text_dim, 1);
    char sc[24];
    std::snprintf(sc, sizeof(sc), "%d", st.score);
    mui_text_center(&m, IW / 2, 82, sc, m.th.text_strong, 2);
    int chosen = -1;
    if (mui_menu_item(&m, &a.pause_menu, 0, 124, 104, 136, 13, "RESUME", "", 1)) chosen = 0;
    if (mui_menu_item(&m, &a.pause_menu, 1, 124, 119, 136, 13, "RESTART", "", 1)) chosen = 1;
    if (mui_menu_item(&m, &a.pause_menu, 2, 124, 134, 136, 13, "QUIT TO TITLE", "", 1)) chosen = 2;
    if (m.in.back) chosen = 0;
    if (chosen == 0) {
        aud_play(SFX_UI, 1.05f);
        a.screen = SC_PLAY;
    } else if (chosen == 1) {
        aud_play(SFX_UI, 0.9f);
        on_run_finished(a);
        a.record_at_start = a.save.best_score;
        a.game.reset(a.rng, 1, a.save.best_score, true);
        a.run_recorded = false;
        a.new_record = false;
        a.screen = SC_PLAY;
    } else if (chosen == 2) {
        aud_play(SFX_UI, 0.85f);
        on_run_finished(a);
        a.screen = SC_TITLE;
    }
}

/* -------------------------------------------------------- screen: summary */
void render_over(App &a)
{
    Mui &m = a.m;
    a.game.draw(m, a.debug);
    mui_rect_blend(&m, 0, 0, IW, IH, MUI_RGB(0x06, 0x03, 0x08), 190);
    int body = mui_panel_titled(&m, 48, 22, 288, 172, "RUN OVER");
    const RunStats &st = a.game.stats();

    mui_text_center(&m, IW / 2, body + 4, a.new_record ? "NEW RECORD" : "SCORE", a.new_record ? m.th.warn : m.th.text_dim,
                    1);
    char sc[32];
    std::snprintf(sc, sizeof(sc), "%d", st.score);
    mui_text_center(&m, IW / 2, body + 14, sc, m.th.text_strong, 3);

    mui_textf(&m, 74, body + 44, m.th.text, 1, "LEVEL REACHED");
    mui_textf(&m, 250, body + 44, m.th.text_strong, 1, "%d", st.level);
    mui_textf(&m, 74, body + 55, m.th.text, 1, "KILLS");
    mui_textf(&m, 250, body + 55, m.th.text_strong, 1, "%d", st.kills);
    mui_textf(&m, 74, body + 66, m.th.text, 1, "TIME");
    mui_textf(&m, 250, body + 66, m.th.text_strong, 1, "%d:%02d", (int)st.seconds / 60, (int)st.seconds % 60);
    mui_textf(&m, 74, body + 77, m.th.text, 1, "ACCURACY");
    float acc = st.shots > 0 ? 100.0f * (float)st.hits / (float)st.shots : 0.0f;
    mui_textf(&m, 250, body + 77, m.th.text_strong, 1, "%.0f%%", (double)acc);

    /* medals earned this run, and what the ladder looks like now */
    mui_text(&m, 74, body + 90, "MEDALS", m.th.text_dim, 1);
    for (int i = 0; i < MEDAL_COUNT; ++i) {
        bool got = (st.medal_mask & (1 << i)) != 0;
        int x = 132 + i * 22;
        if (got) {
            draw_medal_icon(m, i, x, body + 87, 1);
        } else {
            mui_rect(&m, x, body + 87, 11, 11, MUI_RGB(0x18, 0x1C, 0x2C));
            mui_rect_outline(&m, x, body + 87, 11, 11, m.th.panel_edge);
        }
        if (got) mui_textf(&m, x + 1, body + 99, m.th.warn, 1, "%d", a.save.medal[i].threshold / 1000);
    }
    if (a.last_medal_up != MEDAL_NONE) {
        mui_textf(&m, 74, body + 110, m.th.warn, 1, "NEW BEST: %s AT %d", medal_name(a.last_medal_up),
                  a.save.medal[a.last_medal_up].best_threshold);
    } else {
        mui_textf(&m, 74, body + 110, m.th.text_dim, 1, "BEST %d  -  DIAMOND NEEDS %d", a.save.best_score,
                  a.save.medal[MEDAL_DIAMOND].threshold);
    }
    mui_text_center(&m, IW / 2, body + 124,
                    a.save_failed ? "SAVE FAILED - RECORD KEPT IN THIS SESSION" : "LEVEL RESETS TO 1  -  RECORD AND MEDALS STAY",
                    a.save_failed ? m.th.warn : m.th.text_dim, 1);
    mui_prompt(&m, IW / 2, body + 136, "PRESS ENTER TO FLY AGAIN", m.th.accent, 1);

    if (m.in.confirm || m.in.back) {
        aud_play(SFX_UI, 1.0f);
        a.record_at_start = a.save.best_score;
        a.game.reset(a.rng, 1, a.save.best_score, true);
        a.run_recorded = false;
        a.new_record = false;
        a.screen = SC_PLAY;
    }
}

/* -------------------------------------------------------------- play input */
GameInput poll_play_input(const PlatInput &in)
{
    GameInput gi;
    gi.mx = 0.0f;
    gi.my = 0.0f;
    if (in.down[PK_LEFT]) gi.mx -= 1.0f;
    if (in.down[PK_RIGHT]) gi.mx += 1.0f;
    if (in.down[PK_UP]) gi.my -= 1.0f;
    if (in.down[PK_DOWN]) gi.my += 1.0f;
    gi.fire = in.down[PK_FIRE] != 0;
    return gi;
}

/* --------------------------------------------------------------- autoplay */
/* A scripted input source.  It feeds the same edge inputs the keyboard feeds,
 * so it exercises the real screen/state machine and gameplay wiring, and it
 * asserts what should happen instead of leaving it to the eye. */
struct Autoplay {
    uint8_t prev[PK_COUNT];
    uint8_t frame[PK_COUNT];
    int dir = 1;
    int saw_play = 0;
    int saw_pause = 0;
    int saw_move = 0;
    int saw_fire = 0;
    int saw_score = 0;
    int saw_ally = 0, saw_ally_learning = 0;
    /* command panel checks */
    int saw_shop_open = 0, saw_shop_restore = 0, saw_shop_buy = 0, shop_moved = 0;
    int sel_changes = 0, last_sel = 0, buy_level_before = 0, shop_buying = 0, shop_buy_frame = 0;
    float panel_x = -1.0f;
    float settle_x = -1.0f;
    float restore_drift = -1.0f;
    float start_x = -1.0f;
    int start_score = 0;

    void reset()
    {
        for (int i = 0; i < PK_COUNT; ++i) {
            prev[i] = 0;
            frame[i] = 0;
        }
    }
    void press(int key) { frame[key] = 1; }
};

void autoplay_step(App &a, Autoplay &ap, PlatInput &in)
{
    static int f = 0;
    for (int i = 0; i < PK_COUNT; ++i) ap.frame[i] = 0;

    if (f == 3) ap.press(PK_ENTER);                       /* title -> play */
    if (f >= 10 && f < 1740) ap.press(PK_FIRE);            /* hold fire */
    /* Aim at observed enemies; a blind sweep can miss every credit threshold
     * before dying, especially now that audio no longer stalls the frame loop.
     * The command-panel phase below owns the keyboard while it runs, so this
     * block stays out of its windows: two opposite arrow edges in one frame
     * would cancel to no movement and make the restore check lie. */
    if (a.screen == SC_PLAY && f >= 10 && f < 1740 && !(f > 555 && f <= 700)) {
        float x = a.game.player_x();
        if (x > 340.0f) ap.dir = -1;
        if (x < 44.0f) ap.dir = 1;
        Enemy visible[MAX_ENEMIES];
        int count = a.game.enemy_snapshot(visible, MAX_ENEMIES);
        float best = 1e9f, aim = x;
        bool target = false;
        for (int i = 0; i < count; ++i) {
            const Enemy &e = visible[i];
            if (e.y < HUD_H || e.y > a.game.player_y() - 16.0f) continue;
            float distance = std::fabs(e.x - x);
            if (distance >= best) continue;
            best = distance;
            float lead = (a.game.player_y() - e.y) / 180.0f;
            aim = e.x + e.vx * lead;
            target = true;
        }
        if (!target) ap.press(ap.dir > 0 ? PK_RIGHT : PK_LEFT);
        else if (aim > x + 3.0f) ap.press(PK_RIGHT);
        else if (aim < x - 3.0f) ap.press(PK_LEFT);
    }
    /* Keep the script alive across deaths, and probe the pause screen once a
     * second whenever we are actually playing, resuming immediately. */
    if (a.screen == SC_OVER) ap.press(PK_ENTER);
    if (a.screen == SC_PLAY && f > 20 && (f % 60) == 0 && !a.game.shop_open()) ap.press(PK_BACK);
    if (a.screen == SC_PAUSE) ap.press(PK_ENTER);
    /* One fleet purchase proves the hotkey path; after that the wallet is left
     * alone so it can actually reach the command panel's upgrade price. */
    if (a.screen == SC_PLAY && !ap.saw_ally && a.game.credits() >= ally_spec(AK_SCOUT).cost && f % 30 == 7)
        ap.press(PK_ALLY_1);

    /* ---- command panel ----
     * Phase 1 (f560..700): open with Tab, hold a flight key to prove the ship is
     * frozen while the panel is focused, move the selection with the arrows,
     * close with Tab, then prove the flight controls come back.
     * Phase 2 (afterwards): buy a real ship upgrade through Enter as soon as the
     * wallet can afford one -- the same key edges a player would send. */
    if (a.screen == SC_PLAY) {
        if (f == 560) {
            ap.panel_x = a.game.player_x();
            ap.settle_x = a.game.player_x();
            ap.press(PK_TAB);
        }
        if (f > 560 && f < 640) {
            ap.press(PK_RIGHT); /* held flight key: must not steer */
            if (f == 566 || f == 590) ap.press(PK_DOWN);
            if (f == 600) ap.press(PK_UP);
            if (a.game.shop_open()) {
                ap.saw_shop_open = 1;
                /* The ship keeps its smoothed momentum, so it coasts a few pixels
                 * to a stop exactly as if the key were released.  A held key must
                 * produce no sustained motion, and none at all once it settles. */
                if (std::fabs(a.game.player_x() - ap.panel_x) > 8.0f) ap.shop_moved = 1;
                if (f == 575) ap.settle_x = a.game.player_x();
                else if (f > 575 && std::fabs(a.game.player_x() - ap.settle_x) > 1.0f) ap.shop_moved = 1;
                if (a.game.shop_selection() != ap.last_sel) {
                    ap.last_sel = a.game.shop_selection();
                    ap.sel_changes++;
                }
            }
        }
        if (f == 640) ap.press(PK_TAB);
        if (f > 640 && f <= 700) {
            /* Steer away from the nearer wall: holding a blocked direction would
             * make "the controls came back" indistinguishable from "still frozen". */
            ap.press(a.game.player_x() > PLAY_W * 0.5f ? PK_LEFT : PK_RIGHT);
        }
        if (f == 700) {
            ap.restore_drift = std::fabs(a.game.player_x() - ap.panel_x);
            if (ap.restore_drift > 3.0f) ap.saw_shop_restore = 1;
        }
        if (f > 700 && !ap.saw_shop_buy) {
            if (!ap.shop_buying) {
                if (f % 30 == 0 && !a.game.shop_open() && a.game.ship_level() < SHIP_MAX_LEVEL &&
                    a.game.credits() >= a.game.shop_price(SHOP_UPGRADE)) {
                    ap.shop_buying = 1;
                    ap.shop_buy_frame = f;
                    ap.buy_level_before = a.game.ship_level();
                    ap.press(PK_TAB);
                }
            } else {
                int age = f - ap.shop_buy_frame;
                if (age == 2 || age == 4) ap.press(PK_DOWN); /* SHIELD -> BASE -> UPGRADE */
                if (age == 6) ap.press(PK_ENTER);
                if (age == 8) {
                    if (a.game.ship_level() > ap.buy_level_before) ap.saw_shop_buy = 1;
                    if (a.game.shop_open()) ap.press(PK_TAB); /* refused: close and retry later */
                    ap.shop_buying = 0;
                }
            }
        }
    }
    if (f == 360) ap.press(PK_DEBUG);
    if (f == 400) ap.press(PK_MUTE);
    if (f == 420) ap.press(PK_VOL_UP);
    if (f == 440) ap.press(PK_SCALE_UP);
    if (f == 460) ap.press(PK_SCALE_DOWN);
    if (f % 90 == 0) {
        /* progress trace: makes a failing self-check diagnosable at a glance */
        std::printf("    f%-4d screen=%-11s score=%-6d kills=%-3d shots=%-4d hp=%d x=%.0f\n", f, screen_name(a.screen),
                    a.game.stats().score, a.game.stats().kills, a.game.stats().shots, a.game.player_hp(),
                    (double)a.game.player_x());
    }
    f++;

    for (int i = 0; i < PK_COUNT; ++i) {
        uint8_t now = ap.frame[i];
        uint8_t was = ap.prev[i];
        in.down[i] = now;
        in.pressed[i] = (uint8_t)(now && !was);
        in.released[i] = (uint8_t)(!now && was);
        ap.prev[i] = now;
    }

    if (a.screen == SC_PLAY) {
        if (!ap.saw_play) {
            ap.saw_play = 1;
            ap.start_x = a.game.player_x();
            ap.start_score = a.game.stats().score;
        }
        if (std::fabs(a.game.player_x() - ap.start_x) > 3.0f) ap.saw_move = 1;
        if (a.game.stats().shots > 0) ap.saw_fire = 1;
        if (a.game.stats().allies_bought > 0) ap.saw_ally = 1;
        if (a.game.ally_policy().steps > 0) ap.saw_ally_learning = 1;
        if (a.game.stats().kills > 0 || a.game.stats().score > ap.start_score) ap.saw_score = 1;
    }
    if (a.screen == SC_PAUSE) ap.saw_pause = 1;
}

int autoplay_report(const Autoplay &ap)
{
    struct Check {
        const char *what;
        int ok;
    };
    const Check checks[] = {
        {"title menu starts a run", ap.saw_play},
        {"movement input moves the ship", ap.saw_move},
        {"fire input produces shots", ap.saw_fire},
        {"enemies can be hit (kill/score path)", ap.saw_score},
        {"escape reaches the pause screen", ap.saw_pause},
        {"earned credits buy an ally with key 1", ap.saw_ally},
        {"deployed allies update their learning policy", ap.saw_ally_learning},
        {"tab opens the command panel", ap.saw_shop_open},
        {"arrows move the panel selection", ap.sel_changes >= 2},
        {"the ship holds still while the panel is focused", ap.saw_shop_open && !ap.shop_moved},
        {"tab returns control to the ship", ap.saw_shop_restore},
        {"enter buys the selected panel item", ap.saw_shop_buy},
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        std::printf("  %s - %s\n", checks[i].ok ? "ok  " : "FAIL", checks[i].what);
        if (!checks[i].ok) failed++;
    }
    if (!ap.saw_shop_restore) {
        /* A flaky restore check must say how far the ship actually moved. */
        std::printf("    (panel restore drift was %.1f px; the ship must move > 3 px after Tab)\n",
                    (double)ap.restore_drift);
    }
    std::printf("autoplay: %d/%d checks passed\n", (int)(sizeof(checks) / sizeof(checks[0])) - failed,
                (int)(sizeof(checks) / sizeof(checks[0])));
    return failed == 0 ? 0 : 1;
}

void on_run_finished(App &a)
{
    if (a.run_recorded) return;
    a.run_recorded = true;
    const RunStats &st = a.game.stats();
    a.new_record = st.score > a.record_at_start;
    a.last_medal_up = MEDAL_NONE;
    /* Which medal moved to a new score?  That is the record the player keeps. */
    int before[MEDAL_COUNT];
    for (int i = 0; i < MEDAL_COUNT; ++i) before[i] = a.save.medal[i].best_threshold;
    a.last_board_pos = record_run(a.save, st.score, st.level, st.medal_mask, (int)st.seconds);
    for (int i = MEDAL_COUNT - 1; i >= 0; --i) {
        if (a.save.medal[i].best_threshold > before[i]) {
            a.last_medal_up = i;
            break;
        }
    }
    a.save.kills = counter((int64_t)a.save.kills + st.kills);
    a.save.deaths = counter((int64_t)a.save.deaths + st.deaths);
    a.save_failed = a.persist_save && !save_store(a.save);
    if (a.save_failed) std::fprintf(stderr, "mini-space-shooter: save failed; record remains in memory.\n");
}

/* ------------------------------------------------------------- shot mode */
/* Renders whatever is currently in g_pixels to DIR/<label>.ppm at 2x. */
void shot_one(App &a, const char *dir, const char *label)
{
    (void)a;
    plat_scale_blit(g_scaled, IW * 2, IH * 2, g_pixels, IW, IH, 2);
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s.ppm", dir, label);
    if (plat_write_ppm(path, g_scaled, IW * 2, IH * 2) == 0) {
        std::printf("  wrote %s\n", path);
    } else {
        std::printf("  FAILED %s\n", path);
    }
}

GameInput demo_input(float t)
{
    GameInput gi;
    gi.mx = std::sin(t * 0.9f) > 0.0f ? 1.0f : -1.0f;
    gi.my = 0.25f * std::sin(t * 0.37f);
    gi.fire = true;
    return gi;
}

void seed_demo_save(SaveData &s)
{
    /* Ascending order matters: it makes the ladder grow the way a real player
     * would, which is what fills the "past records" history. */
    save_defaults(s);
    record_run(s, 880, 2, 0x00, 51);
    record_run(s, 1450, 3, 0x01, 74);
    record_run(s, 2410, 4, 0x01, 118);
    record_run(s, 3980, 5, 0x03, 152);
    record_run(s, 5200, 6, 0x03, 190);
    record_run(s, 7100, 7, 0x07, 244);
    record_run(s, 9430, 9, 0x07, 301);
    record_run(s, 12110, 11, 0x0F, 355);
    record_run(s, 18240, 14, 0x0F, 488);
    record_run(s, 24850, 17, 0x1F, 612);
    s.runs = 23;
    s.kills = 1420;
    s.deaths = 23;
    s.total_seconds = 5400;
    s.best_run_seconds = 612;
}

/* ------------------------------------------------------- sprite sheet shot */
void render_sprite_sheet(App &a)
{
    Mui &m = a.m;
    mui_clear(&m, MUI_RGB(0x0A, 0x0C, 0x16));
    mui_noise_rect(&m, 0, 0, IW, IH, MUI_RGB(0x20, 0x28, 0x48), 26, 7u);
    mui_text(&m, 6, 4, "PIXEL ART SHEET  (drawn at 3x)", a.theme.text_strong, 1);
    mui_divider(&m, 4, 13, IW - 8, a.theme.panel_edge);

    struct Item {
        const Sprite *s;
        const char *label;
        int x, y, scale, label_y;
    };
    const Item items[] = {
        {&art::player, "PLAYER", 6, 18, 2, 50},
        {&art::player_mk[0], "MK2", 46, 18, 2, 50},
        {&art::player_mk[1], "MK3", 78, 18, 2, 50},
        {&art::player_mk[2], "MK4", 110, 18, 2, 50},
        {&art::player_mk[3], "MK5", 142, 18, 2, 50},
        {&art::enemy_grunt, "GRUNT", 174, 18, 2, 50},
        {&art::enemy_wasp, "WASP", 210, 18, 2, 50},
        {&art::enemy_brute, "BRUTE", 246, 18, 2, 50},
        {&art::enemy_ghost, "GHOST", 282, 18, 2, 50},
        {&art::base, "BASE", 310, 18, 2, 50},
        {&art::laser, "LASER", 352, 18, 2, 50},
        {&art::medal[MEDAL_BRONZE], "BRONZE", 8, 64, 3, 100},
        {&art::medal[MEDAL_SILVER], "SILVER", 58, 64, 3, 100},
        {&art::medal[MEDAL_GOLD], "GOLD", 108, 64, 3, 100},
        {&art::medal[MEDAL_PLATINUM], "PLATINUM", 158, 64, 3, 100},
        {&art::medal[MEDAL_DIAMOND], "DIAMOND", 218, 64, 3, 100},
        {&art::bullet_player, "SHOT", 8, 152, 5, 178},
        {&art::bullet_enemy, "ENEMY SHOT", 36, 152, 5, 178},
        {&art::bullet_big, "BIG SHOT", 102, 152, 4, 178},
        {&art::heart_full, "LIFE", 156, 152, 4, 178},
        {&art::heart_empty, "LOST LIFE", 192, 152, 4, 178},
        {&art::spark, "SPARK", 254, 152, 4, 178},
        {&art::flash_small, "FLASH S", 292, 151, 3, 178},
        {&art::flash_big, "FLASH L", 338, 150, 2, 178},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        const Item &it = items[i];
        mui_blit_scaled(&m, it.s->px, it.s->w, it.s->h, it.x, it.y, it.scale, 0);
        mui_text(&m, it.x, it.label_y, it.label, a.theme.text_dim, 1);
    }
    mui_text(&m, 6, IH - 10, "MK2-MK5: upgrade models  |  BASE + LASER: panel purchases", a.theme.text_dim, 1);
}

int run_shots(App &a, const char *dir)
{
    std::printf("rendering screens offscreen into %s\n", dir);
    /* create the output directory (component by component) */
    {
        char tmp[512];
        std::snprintf(tmp, sizeof(tmp), "%s", dir);
        for (char *p = tmp + 1; *p; ++p) {
            if (*p != '/') continue;
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
        mkdir(tmp, 0755);
    }
    seed_demo_save(a.save);

    /* title */
    a.screen = SC_TITLE;
    a.game.reset(a.rng, 3, a.save.best_score, false);
    for (int i = 0; i < 120; ++i) a.game.update_attract(STEP);
    mui_begin(&a.m, g_pixels, IW, IH, a.theme, MuiInput(), 3.0f);
    render_title(a);
    shot_one(a, dir, "01-title");

    /* leaderboard + medals + controls */
    mui_begin(&a.m, g_pixels, IW, IH, a.theme, MuiInput(), 3.0f);
    render_board(a);
    shot_one(a, dir, "02-leaderboard");
    mui_begin(&a.m, g_pixels, IW, IH, a.theme, MuiInput(), 3.0f);
    render_medals(a);
    shot_one(a, dir, "03-medals");
    mui_begin(&a.m, g_pixels, IW, IH, a.theme, MuiInput(), 3.0f);
    render_controls(a);
    shot_one(a, dir, "04-controls");

    /* a calm early level and a chaotic late one, both played by a simple bot */
    const int levels[2] = {2, 22};
    const char *names[2] = {"05-play-level-02", "06-play-level-22"};
    for (int k = 0; k < 2; ++k) {
        a.game.reset(a.rng, levels[k], a.save.best_score, false);
        float t = 0.0f;
        for (int i = 0; i < 1500; ++i) {
            t += STEP;
            a.game.update(demo_input(t), STEP);
            if (i % 120 == 0 && a.game.credits() >= ally_spec(k).cost) a.game.recruit(k);
            if (a.game.over()) {
                a.game.reset(a.rng, levels[k], a.save.best_score, false);
                t = 0.0f;
            }
        }
        mui_begin(&a.m, g_pixels, IW, IH, a.theme, MuiInput(), t);
        a.game.draw(a.m, false);
        shot_one(a, dir, names[k]);
    }

    /* the ML / difficulty overlay on top of live play */
    a.game.reset(a.rng, 22, a.save.best_score, false);
    float t = 0.0f;
    for (int i = 0; i < 1800; ++i) {
        t += STEP;
        a.game.update(demo_input(t), STEP);
        if (a.game.over()) {
            a.game.reset(a.rng, 22, a.save.best_score, false);
            t = 0.0f;
        }
    }
    mui_begin(&a.m, g_pixels, IW, IH, a.theme, MuiInput(), t);
    a.game.draw(a.m, true);
    shot_one(a, dir, "07-ml-overlay");

    /* pause overlay */
    a.screen = SC_PAUSE;
    mui_begin(&a.m, g_pixels, IW, IH, a.theme, MuiInput(), t);
    render_pause(a);
    shot_one(a, dir, "08-pause");

    /* run summary after a real (bot) death */
    a.screen = SC_PLAY;
    a.game.reset(a.rng, 12, a.save.best_score, false);
    t = 0.0f;
    for (int i = 0; i < 8000 && !a.game.over(); ++i) {
        t += STEP;
        /* this bot barely dodges, so it dies and produces a real summary */
        GameInput gi;
        gi.mx = 0.0f;
        gi.my = 0.0f;
        gi.fire = true;
        a.game.update(gi, STEP);
    }
    on_run_finished(a);
    a.new_record = false;
    a.screen = SC_OVER;
    mui_begin(&a.m, g_pixels, IW, IH, a.theme, MuiInput(), t);
    render_over(a);
    shot_one(a, dir, "09-gameover");
    /* sprite sheet: proves the art tables rasterise to the intended shapes */
    mui_begin(&a.m, g_pixels, IW, IH, a.theme, MuiInput(), t);
    render_sprite_sheet(a);
    shot_one(a, dir, "10-sprite-sheet");

    /* command panel doing all three jobs at once: focused, with the shield up,
     * the floating base escorting and the ship at the top of the upgrade ladder */
    a.game.reset(a.rng, 12, a.save.best_score, false);
    a.game.award_points(60000);
    for (int k = 0; k < AK_COUNT; ++k) a.game.recruit(k);
    struct BuyStep {
        int item;
        int times;
    };
    const BuyStep buys[] = {{SHOP_SHIELD, 1}, {SHOP_BASE, 1}, {SHOP_UPGRADE, SHIP_MAX_LEVEL}};
    for (size_t b = 0; b < sizeof(buys) / sizeof(buys[0]); ++b) {
        for (int i = 0; i < buys[b].times; ++i) {
            a.game.shop_toggle();
            for (int step = 0; step < buys[b].item; ++step) a.game.shop_move(1);
            if (!a.game.shop_activate() && a.game.shop_open()) a.game.shop_close();
        }
    }
    t = 0.0f;
    for (int i = 0; i < 240; ++i) {
        t += STEP;
        a.game.update(demo_input(t), STEP);
    }
    a.game.shop_toggle(); /* leave the panel focused so the cursor is visible */
    mui_begin(&a.m, g_pixels, IW, IH, a.theme, MuiInput(), t);
    a.game.draw(a.m, false);
    shot_one(a, dir, "11-command-panel");

    std::printf("done (11 shots)\n");
    return 0;
}

} /* namespace */

int main(int argc, char **argv)
{
    App app;
    app.theme = mui_theme_default();
    app.rng.seed(0xC0FFEEu);
    app.now = 0.0;
    int scale = 0;
    int frames = 0;
    int start_level = 1;
    const char *shot_dir = nullptr;
    bool force_headless = false;
    bool reset_save = false;
    bool focus_self = false;
    int verify_present = 0;
    int autoplay_frames = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot_dir = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) scale = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) app.rng.seed((uint32_t)std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--level") == 0 && i + 1 < argc) start_level = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--debug") == 0) app.debug = true;
        else if (std::strcmp(argv[i], "--reset-save") == 0) reset_save = true;
        else if (std::strcmp(argv[i], "--autoplay") == 0) autoplay_frames = 2100;
        else if (std::strcmp(argv[i], "--focus-self") == 0) focus_self = true;
        else if (std::strcmp(argv[i], "--verify-present") == 0) verify_present = (i + 1 < argc) ? std::atoi(argv[++i]) : 30;
        else if (std::strcmp(argv[i], "--headless") == 0) force_headless = true;
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("mini-space-shooter options:\n"
                        "  --shot DIR    render every screen offscreen to DIR/*.ppm\n"
                        "  --frames N    run N frames then exit\n"
                        "  --scale N     integer pixel scale (default: fit the screen)\n"
                        "  --seed N      deterministic run seed\n"
                        "  --level N     start at level N\n"
                        "  --debug       start with the ML overlay on\n"
                        "  --reset-save  wipe scores and medals, then exit\n"
                        "  --autoplay    scripted window run with self-checks\n"
                        "  --focus-self  request keyboard focus from the X server\n"
                        "  --verify-present N  read back and check the presented frame\n"
                        "  --headless    never open a window\n");
            return 0;
        }
    }

    int bad_rows = art::art_init();
    (void)bad_rows;
    app.persist_save = !shot_dir && autoplay_frames == 0 && !force_headless && frames == 0 && verify_present == 0;
    if (reset_save) {
        save_defaults(app.save);
        if (!save_store(app.save)) {
            std::fprintf(stderr, "mini-space-shooter: could not write the save file.\n");
            return 1;
        }
        char where[600];
        save_path(where, (int)sizeof(where));
        std::printf("save reset: %s\n", where);
        return 0;
    }
    save_defaults(app.save);
    if (app.persist_save) save_load(app.save);

    bool headless = force_headless || shot_dir != nullptr;
    if (plat_open(IW, IH, scale, headless ? 1 : 0) != 0) {
        std::fprintf(stderr, "mini-space-shooter: cannot open a display.\n"
                             "Run it inside a graphical session, or use --shot DIR for offscreen frames.\n");
        return 2;
    }

    if (!headless) aud_init();
    else aud_init_offline();

    if (shot_dir) {
        int rc = run_shots(app, shot_dir);
        aud_shutdown();
        plat_close();
        return rc;
    }

    if (focus_self) {
        /* Test aid: take the keyboard focus so XTEST-generated keys land in
         * this window instead of whatever the user is working in. */
        int ok = plat_focus_self();
        std::printf("focus-self: %s\n", ok == 0 ? "ok" : "unavailable");
        std::fflush(stdout);
    }

    Autoplay ap;
    ap.reset();
    if (autoplay_frames > 0) frames = autoplay_frames;

    app.record_at_start = app.save.best_score;
    app.game.reset(app.rng, start_level, app.save.best_score, app.save.runs > 0);

    double last = plat_time();
    double accum = 0.0;
    double fps_timer = last;
    int fps_count = 0;
    uint64_t frame_index = 0;
    int exit_code_verify = -1;
    PlatInput in;

    while (app.running) {
        plat_poll(&in);
        if (in.quit) {
            if (getenv("MSS_DEBUG_EVENTS")) std::fprintf(stderr, "[quit] window close handled\n");
            break;
        }
        if (autoplay_frames > 0) autoplay_step(app, ap, in);

        if (in.pressed[PK_DEBUG]) {
            app.debug = !app.debug;
            aud_play(SFX_UI, 1.2f);
        }
        if (in.pressed[PK_MUTE]) aud_toggle_mute();
        if (in.pressed[PK_MUSIC]) aud_set_music(!aud_music_enabled());
        /* Purchase once per input edge, even with zero or multiple fixed steps. */
        if (app.screen == SC_PLAY && !in.pressed[PK_BACK] && !in.pressed[PK_PAUSE]) {
            for (int k = 0; k < AK_COUNT; ++k) if (in.pressed[PK_ALLY_1 + k]) app.game.recruit(k);
        }
        /* ---- bottom-right command panel ----
         * Tab focuses it (and un-focuses it again); while focused the arrows pick
         * a row and Enter buys it.  Flight controls are suppressed inside
         * Game::update for exactly as long as it stays focused. */
        if (app.screen == SC_PLAY) {
            if (in.pressed[PK_TAB]) app.game.shop_toggle();
            if (app.game.shop_open()) {
                if (in.pressed[PK_UP]) app.game.shop_move(-1);
                if (in.pressed[PK_DOWN]) app.game.shop_move(1);
                if (in.pressed[PK_ENTER]) app.game.shop_activate();
            }
        }
        if (in.pressed[PK_VOL_DOWN]) aud_adjust_master(-0.04f);
        if (in.pressed[PK_VOL_UP]) aud_adjust_master(0.04f);

        double now = plat_time();
        double frame_dt = now - last;
        last = now;
        if (frame_dt > 0.25) frame_dt = 0.25; /* never simulate a huge stall */
        accum += frame_dt;

        int steps = 0;
        while (accum >= (double)STEP && steps < 5) {
            if (app.screen == SC_PLAY) {
                GameInput gi = poll_play_input(in);
                app.game.update(gi, STEP);
                if (app.game.over()) {
                    on_run_finished(app);
                    app.screen = SC_OVER;
                    mui_menu_begin(&app.over_menu, 1);
                }
            }
            accum -= (double)STEP;
            steps++;
            app.now += (double)STEP;
        }
        if (steps == 5) accum = 0.0; /* drop time we cannot catch up on */

        /* Screen transitions happen before the UI edges are built, so the edge
         * that caused a transition cannot leak into the screen we are entering
         * (otherwise pause would resume itself on the very same frame). */
        bool back_consumed = false;
        if (app.screen == SC_PLAY) {
            if (in.pressed[PK_BACK] && app.game.shop_open()) {
                /* Esc closes the panel first; a second Esc pauses the game. */
                app.game.shop_close();
                back_consumed = true;
            } else if (in.pressed[PK_BACK] || in.pressed[PK_PAUSE]) {
                app.game.shop_close(); /* never leave the panel open behind a pause */
                app.screen = SC_PAUSE;
                mui_menu_begin(&app.pause_menu, 3);
                app.pause_menu.glow[0] = 1.0f;
                aud_play(SFX_UI, 0.9f);
                back_consumed = true;
            }
        }

        MuiInput mi;
        mi.up = in.pressed[PK_UP];
        mi.down = in.pressed[PK_DOWN];
        mi.left = in.pressed[PK_LEFT];
        mi.right = in.pressed[PK_RIGHT];
        mi.confirm = back_consumed ? 0 : (in.pressed[PK_ENTER] || in.pressed[PK_FIRE]);
        mi.back = back_consumed ? 0 : in.pressed[PK_BACK];
        mui_begin(&app.m, g_pixels, IW, IH, app.theme, mi, (float)app.now);

        if (app.screen == SC_PLAY) app.game.draw(app.m, app.debug);
        switch (app.screen) {
        case SC_TITLE: render_title(app); break;
        case SC_PAUSE: render_pause(app); break;
        case SC_OVER: render_over(app); break;
        case SC_BOARD: render_board(app); break;
        case SC_MEDALS: render_medals(app); break;
        case SC_CONTROLS: render_controls(app); break;
        default: break;
        }

        /* debug frame counter in the corner of the title/board screens */
        if (app.debug && app.screen != SC_PLAY) {
            char label[80];
            plat_mode_label(label, (int)sizeof(label));
            mui_textf(&app.m, 4, 4, app.m.th.good, 1, "%.1f fps  %.0f us  %s", app.fps, 0.0, label);
        }

        plat_present(g_pixels, IW, IH);
        aud_pump(frame_dt);

        fps_count++;
        if (now - fps_timer >= 0.5) {
            app.fps = (double)fps_count / (now - fps_timer);
            fps_timer = now;
            fps_count = 0;
        }

        if (frame_index < UINT64_MAX) frame_index++;
        if (frames > 0 && frame_index >= (uint64_t)frames) break;

        /* Presentation self-check: present a few frames, then compare what the
         * X server holds with what we sent.  Three distinct outcomes, because
         * "the server will not tell us" must not be reported as success:
         *   0 = the pixels matched   1 = they did not match   2 = unverifiable */
        if (verify_present > 0 && frame_index >= (uint64_t)verify_present) {
            int px = plat_scale();
            int total = (IW * px) * (IH * px);
            int match = plat_verify_present();
            if (match == -2) {
                std::fprintf(stderr, "present verify: FAILED - the X11 window is not visible. "
                                     "On Wayland, use MSS_VIDEO_BACKEND=wayland ./run.sh.\n");
                exit_code_verify = 1;
            } else if (match < 0) {
                std::printf("present verify: UNVERIFIED - this backend does not provide window pixel read-back.\n");
                exit_code_verify = 2;
            } else {
                std::printf("present verify: %d/%d pixels match (%.2f%%)\n", match, total,
                            100.0 * (double)match / (double)total);
                exit_code_verify = (match * 100 >= total * 99) ? 0 : 1;
            }
            std::fflush(stdout);
            break;
        }

        /* Frame pacing: sleep the remainder of the 60 Hz budget. */
        double spent = plat_time() - now;
        double budget = (double)STEP;
        if (spent < budget) plat_sleep(budget - spent);
    }

    if (app.screen == SC_PLAY || app.screen == SC_PAUSE) on_run_finished(app);
    if (app.persist_save && app.save_failed) app.save_failed = !save_store(app.save);
    int exit_code = 0;
    if (autoplay_frames > 0) exit_code = autoplay_report(ap);
    if (exit_code_verify >= 0) exit_code = exit_code_verify;
    if (getenv("MSS_DEBUG_EVENTS")) {
        char backend[16];
        aud_backend_name(backend, (int)sizeof(backend));
        std::fprintf(stderr, "[exit] audio backend %s, frames %llu, errors %u, shutting down\n", backend,
                     (unsigned long long)aud_frames_submitted(), aud_output_errors());
    }
    aud_shutdown();
    if (getenv("MSS_DEBUG_EVENTS")) std::fprintf(stderr, "[exit] audio closed, closing window\n");
    plat_close();
    if (getenv("MSS_DEBUG_EVENTS")) std::fprintf(stderr, "[exit] window closed\n");
    return exit_code;
}
