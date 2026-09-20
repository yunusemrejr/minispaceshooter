/* art.cpp — ASCII-art pixel art tables and the one-time rasteriser.
 *
 * Every sprite is a rectangle of characters; '.' (and ' ') are transparent.
 * A tiny character translation step lets one authored shape serve several
 * sprites: the five medals share a single shape and differ only by palette,
 * which is exactly the "same icon, different score" idea the medal ladder
 * relies on.
 */
#include "art.hpp"

#include <cstring>

namespace mss {
namespace art {

/* ------------------------------------------------------------------ palette */
struct PaletteEntry {
    char c;
    uint32_t rgb;
};

static const PaletteEntry PALETTE[] = {
    {'k', 0x05060Au}, /* outline near-black */
    {'d', 0x2A3350u}, /* dark slate */
    {'x', 0x5A6A9Au}, /* mid slate */
    {'g', 0x4A5578u}, /* gunmetal */
    {'W', 0x8E9AB8u}, /* light steel */
    {'w', 0xF2F5FFu}, /* white */
    {'c', 0x2E9FB8u}, /* teal */
    {'C', 0x5FE3F2u}, /* cyan */
    {'B', 0x2C6BE0u}, /* blue */
    {'b', 0x1B3E8Cu}, /* deep blue */
    {'n', 0x101B3Au}, /* navy */
    {'e', 0x3FA84Au}, /* green */
    {'E', 0x8CF07Au}, /* bright green */
    {'y', 0xFFD65Cu}, /* yellow */
    {'o', 0xFF9A2Eu}, /* orange */
    {'r', 0xC03030u}, /* red */
    {'R', 0xFF5A5Au}, /* bright red */
    {'m', 0xC24BB0u}, /* magenta */
    {'p', 0xFF7AC0u}, /* pink */
    {'v', 0x7A5CE0u}, /* violet */
    {'s', 0x8C8C96u}, /* silver */
    {'S', 0xE0E4F0u}, /* light silver */
    {'A', 0xA86A2Eu}, /* bronze */
    {'a', 0xD9A066u}, /* light bronze */
    {'Y', 0xE0A81Eu}, /* gold */
    {'G', 0xFFE28Au}, /* light gold */
    {'q', 0x9AA6C8u}, /* platinum */
    {'Q', 0xE8F0FFu}, /* platinum light */
    {'K', 0xFF2D6Fu}, /* ribbon red */
    {'X', 0xFFFFFFu}, /* shape placeholder */
};

uint32_t color(char c)
{
    for (size_t i = 0; i < sizeof(PALETTE) / sizeof(PALETTE[0]); ++i) {
        if (PALETTE[i].c == c) return PALETTE[i].rgb;
    }
    return 0u; /* transparent */
}

/* --------------------------------------------------------------- shape data */
/* Player fighter, 9x11, nose up: white canopy, cyan hull, engine flame. */
static const char *const PLAYER_ROWS[] = {
    "....k....", "...kCk...", "...kCk...", "..kkCkk..", "..kCCCk..", ".kcCCCck.",
    "kccCCCcck", "kcCCCCCck", "kc.kCk.ck", "k..kCk..k", "...kok...",
};

/* Drone: cheap, slow, red core. */
static const char *const GRUNT_ROWS[] = {
    "..kkkkk..", ".keeeeek.", "kegEEEgek", "kgeeeeegk", "kgrrrrrgk", ".kgrrrgk.", "..kg.gk..", "...k.k...",
};

/* Wasp: fast strafer with wide wings. */
static const char *const WASP_ROWS[] = {
    "k.......k", "kk.....kk", "kck...kck", "kcck.kcck", ".kcckcck.", "..kmpmk..", "...kpk...", "....k....",
};

/* Brute: 13x11, armoured, fires spreads. */
static const char *const BRUTE_ROWS[] = {
    "...kkkkkkk...", "..kgggggggk..", ".kgWWWWWWWgk.", "kgWrrrrrrrWgk", "kgWrooooorWgk",
    "kgWoooooooWgk", ".kgGrrrrrGgk.", "..kgggggggk..", "....kkkkk....", ".....kok.....", "......k......",
};

/* Ghost: feints in and out. */
static const char *const GHOST_ROWS[] = {
    "...kkk...", "..kvvvk..", ".kvVVVvk.", "kvVCCCvvk", "kvVvvvVvk", ".kvVVVvk.", "..kvvvk..", "...k.k...", "..k...k..",
};

/* Friendly ships point upward, with green/gold hulls distinct from the player. */
static const char *const SCOUT_ROWS[] = {
    "...E...", "..kWk..", "..kEk..", ".keEek.", "keEEEek", "k.kEk.k", "..kok..",
};
static const char *const WING_ROWS[] = {
    "....E....", "...kWk...", "k..kEk..k", "kkkeEekkk", "keEEWEEek", "keEEEEEek", "ke.kEk.ek", "k..kok..k", "....o....",
};
static const char *const CRUISER_ROWS[] = {
    ".....E.....", "....kWk....", "...keEek...", "k..keEek..k", "kkkkEEEkkkk", "keeeEEEeeek", "keEEWwWEEek", "keEEEEEEEek", "ke.kEEEk.ek", "k..ko.ok..k", "....o.o....",
};
static const char *const TITAN_ROWS[] = {
    "......G......", ".....kWk.....", "....kyGyk....", "k...kyGyk...k", "kk.kkGGGkk.kk", "kykkyGGGykkyk", "kyyyyGwGyyyyk", "kyGyyGwGyyGyk", "kyGkyGGGykGyk", "ky.kkGGGkk.yk", "kk..kyGyk..kk", "....ko.ok....", ".....o.o.....",
};
static const char *const BULLET_ALLY_ROWS[] = {".E.", "EwE", ".E.", ".e."};

static const char *const BULLET_PLAYER_ROWS[] = {
    "kCk", "kwk", "kwk", "kCk", ".c.",
};

static const char *const BULLET_ENEMY_ROWS[] = {
    ".m.", "mpm", ".m.",
};

static const char *const BULLET_BIG_ROWS[] = {
    "..k..", ".kok.", "koyok", ".kok.", "..k..",
};

/* One shared medal shape: K ribbon, X metal, x metal shade, w centre spark. */
static const char *const MEDAL_ROWS[] = {
    "..K..K..K..", "..KKKKKKK..", ".kkkkkkkkk.", ".kXXXXXXXk.", "kXXXXXXXXXk", "kXXXxxxXXXk",
    "kXXxxwxxXXk", "kXXXxxxXXXk", "kXXXXXXXXXk", ".kXXXXXXXk.", ".kkkkkkkkk.",
};

/* One shared heart shape; full and empty are palette variants. */
static const char *const HEART_ROWS[] = {
    ".kk.kk.", "kXXkXXk", "kXXXXXk", ".kXXXk.", "..kXk..", "...k...",
};

static const char *const SPARK_ROWS[] = {
    "..X..", ".XXX.", "XXXXX", ".XXX.", "..X..",
};

static const char *const FLASH_SMALL_ROWS[] = {
    "..XXX..", ".XXXXX.", "XXXXXXX", "XXXXXXX", "XXXXXXX", ".XXXXX.", "..XXX..",
};

static const char *const FLASH_BIG_ROWS[] = {
    "....XXX....", "..XXXXXXX..", ".XXXXXXXXX.", "XXX.....XXX", "XX.......XX", "X.........X",
    "XX.......XX", "XXX.....XXX", ".XXXXXXXXX.", "..XXXXXXX..", "....XXX....",
};

/* ------------------------------------------------------------------ storage */
#define SPRITE_MAX_W 13
#define SPRITE_MAX_H 11

static uint32_t s_ally[4][13 * 13];
static uint32_t s_bullet_ally[3 * 4];
static uint32_t s_player[9 * 11];
static uint32_t s_grunt[9 * 8];
static uint32_t s_wasp[9 * 8];
static uint32_t s_brute[13 * 11];
static uint32_t s_ghost[9 * 9];
static uint32_t s_bullet_player[3 * 5];
static uint32_t s_bullet_enemy[3 * 3];
static uint32_t s_bullet_big[5 * 5];
static uint32_t s_medal[MEDAL_COUNT][11 * 11];
static uint32_t s_heart_full[7 * 6];
static uint32_t s_heart_empty[7 * 6];
static uint32_t s_spark[5 * 5];
static uint32_t s_flash_small[7 * 7];
static uint32_t s_flash_big[11 * 11];

Sprite ally[4] = {{s_ally[0], 7, 7}, {s_ally[1], 9, 9}, {s_ally[2], 11, 11}, {s_ally[3], 13, 13}};
Sprite bullet_ally{s_bullet_ally, 3, 4};
Sprite player{s_player, 9, 11};
Sprite enemy_grunt{s_grunt, 9, 8};
Sprite enemy_wasp{s_wasp, 9, 8};
Sprite enemy_brute{s_brute, 13, 11};
Sprite enemy_ghost{s_ghost, 9, 9};
Sprite bullet_player{s_bullet_player, 3, 5};
Sprite bullet_enemy{s_bullet_enemy, 3, 3};
Sprite bullet_big{s_bullet_big, 5, 5};
Sprite medal[MEDAL_COUNT] = {
    {s_medal[0], 11, 11}, {s_medal[1], 11, 11}, {s_medal[2], 11, 11}, {s_medal[3], 11, 11}, {s_medal[4], 11, 11},
};
Sprite heart_full{s_heart_full, 7, 6};
Sprite heart_empty{s_heart_empty, 7, 6};
Sprite spark{s_spark, 5, 5};
Sprite flash_small{s_flash_small, 7, 7};
Sprite flash_big{s_flash_big, 11, 11};

/* ------------------------------------------------------------------ builder */
static void identity(char trans[128])
{
    for (int i = 0; i < 128; ++i) trans[i] = (char)i;
}

static void substitute(char trans[128], const char *from, const char *to)
{
    for (int i = 0; from[i] && to[i]; ++i) trans[(int)(unsigned char)from[i]] = to[i];
}

/* Returns the number of rows whose length did not match `w`. */
static int build(uint32_t *dst, const char *const *rows, int w, int h, const char trans[128])
{
    int bad = 0;
    for (int y = 0; y < h; ++y) {
        const char *row = rows[y];
        size_t len = std::strlen(row);
        if ((int)len != w) bad++;
        for (int x = 0; x < w; ++x) {
            char c = (x < (int)len) ? row[x] : '.';
            char t = trans[(int)(unsigned char)c];
            dst[y * w + x] = (c == '.' || c == ' ') ? 0u : color(t);
        }
    }
    return bad;
}

int art_init(void)
{
    char id[128];
    identity(id);
    int bad = 0;
    bad += build(ally[0].px, SCOUT_ROWS, 7, 7, id);
    bad += build(ally[1].px, WING_ROWS, 9, 9, id);
    bad += build(ally[2].px, CRUISER_ROWS, 11, 11, id);
    bad += build(ally[3].px, TITAN_ROWS, 13, 13, id);
    bad += build(bullet_ally.px, BULLET_ALLY_ROWS, 3, 4, id);
    bad += build(player.px, PLAYER_ROWS, 9, 11, id);
    bad += build(enemy_grunt.px, GRUNT_ROWS, 9, 8, id);
    bad += build(enemy_wasp.px, WASP_ROWS, 9, 8, id);
    bad += build(enemy_brute.px, BRUTE_ROWS, 13, 11, id);
    bad += build(enemy_ghost.px, GHOST_ROWS, 9, 9, id);
    bad += build(bullet_player.px, BULLET_PLAYER_ROWS, 3, 5, id);
    bad += build(bullet_enemy.px, BULLET_ENEMY_ROWS, 3, 3, id);
    bad += build(bullet_big.px, BULLET_BIG_ROWS, 5, 5, id);
    bad += build(spark.px, SPARK_ROWS, 5, 5, id);
    bad += build(flash_small.px, FLASH_SMALL_ROWS, 7, 7, id);
    bad += build(flash_big.px, FLASH_BIG_ROWS, 11, 11, id);

    /* Hearts: same shape, full red or dim slate. */
    {
        char t[128];
        identity(t);
        substitute(t, "X", "R");
        bad += build(heart_full.px, HEART_ROWS, 7, 6, t);
        identity(t);
        substitute(t, "X", "d");
        bad += build(heart_empty.px, HEART_ROWS, 7, 6, t);
    }

    /* Medals: one silhouette, five palettes, exactly like the real ladder. */
    static const char *const metal[MEDAL_COUNT] = {"aA", "Ss", "GY", "Qq", "wC"};
    static const char *const ribbon[MEDAL_COUNT] = {"A", "s", "Y", "v", "C"};
    for (int i = 0; i < MEDAL_COUNT; ++i) {
        char t[128];
        identity(t);
        char from[3] = {'X', 'x', '\0'};
        char to[3] = {metal[i][0], metal[i][1], '\0'};
        substitute(t, from, to);
        char rfrom[2] = {'K', '\0'};
        char rto[2] = {ribbon[i][0], '\0'};
        substitute(t, rfrom, rto);
        bad += build(medal[i].px, MEDAL_ROWS, 11, 11, t);
    }
    return bad;
}

} /* namespace art */
} /* namespace mss */
