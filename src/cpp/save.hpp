/* save.hpp — leaderboard, run stats and the *dynamic* five-medal ladder.
 *
 * The medal design is the interesting part.  There are always five medals
 * (bronze → diamond) and their thresholds are derived from your best score:
 *
 *      bronze   round_up(best * 0.10)
 *      silver   round_up(best * 0.25)
 *      gold     round_up(best * 0.45)
 *      platinum round_up(best * 0.70)
 *      diamond  best        (always exactly your record)
 *
 * So the medals never change appearance, but the score each one represents
 * keeps moving up as you improve: a diamond you earned at 145 will later be
 * represented by a different number while lower tiers shift up behind it.
 * The file remembers, per medal, the highest score it has ever represented
 * and the date, so "diamond at 145" is still on record after "diamond at
 * 2394385" replaces it in the HUD.
 *
 * Storage is a small, human-readable, line based text file.  Unknown keys are
 * ignored and a corrupt file falls back to defaults instead of failing.
 */
#ifndef MINI_SAVE_HPP
#define MINI_SAVE_HPP

#include <cstdint>

namespace mss {

constexpr int MEDAL_COUNT = 5;
constexpr int LEADERBOARD_SIZE = 10;
constexpr int MEDAL_HISTORY = 32;

enum MedalTier : int {
    MEDAL_BRONZE = 0,
    MEDAL_SILVER = 1,
    MEDAL_GOLD = 2,
    MEDAL_PLATINUM = 3,
    MEDAL_DIAMOND = 4,
    MEDAL_NONE = -1
};

const char *medal_name(int tier);
const char *medal_short(int tier);

struct MedalInfo {
    int threshold = 0;      /* score needed right now for this medal */
    int best_threshold = 0; /* highest score this medal has ever represented */
    uint32_t best_date = 0; /* yyyymmdd when that happened */
    int earned_count = 0;   /* how many runs earned this tier */
};

struct LeaderEntry {
    int score = 0;
    int level = 1;
    int medal_mask = 0; /* bit per tier earned in that run */
    uint32_t date = 0;
    int seconds = 0;
};

struct HistEntry {
    uint32_t date = 0;
    int tier = 0;
    int score = 0;
};

struct SaveData {
    int best_score = 0;
    int best_level = 1;
    int runs = 0;
    int kills = 0;
    int deaths = 0;
    int total_seconds = 0;
    int best_run_seconds = 0;
    MedalInfo medal[MEDAL_COUNT];
    LeaderEntry board[LEADERBOARD_SIZE];
    int board_count = 0;
    HistEntry history[MEDAL_HISTORY];
    int history_count = 0;
};

/* Fills the five thresholds for a given best score (strictly increasing,
 * last one exactly `best`).  `out` must hold MEDAL_COUNT ints. */
void medal_thresholds(int best, int out[MEDAL_COUNT]);

/* Highest medal tier `score` reaches against the ladder built from `best`,
 * or MEDAL_NONE. */
int medal_for_score(int score, int best);

/* Refreshes MedalInfo thresholds for the current best score. */
void refresh_medal_thresholds(SaveData &s);

/* Records a finished run: updates stats, medal records/history and the
 * leaderboard.  Returns the index of the new leaderboard entry, or -1 if the
 * run did not make the board. */
int record_run(SaveData &s, int score, int level, int medal_mask, int seconds);

void save_defaults(SaveData &s);
/* Returns true when a file was read and parsed. */
bool save_load(SaveData &s);
/* Writes the file, creating the directory. Returns true on success. */
bool save_store(const SaveData &s);
void save_path(char *out, int cap);
uint32_t save_today(void);
const char *save_medal_mask_text(int mask, char *buf, int cap);

} /* namespace mss */

#endif /* MINI_SAVE_HPP */
