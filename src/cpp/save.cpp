/* save.cpp — text persistence for the leaderboard and the medal ladder. */
#define _POSIX_C_SOURCE 200809L

#include "save.hpp"
#include "limits.hpp"

#include <cerrno>
#include <charconv>
#include <cctype>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>

namespace mss {

const char *medal_name(int tier)
{
    switch (tier) {
    case MEDAL_BRONZE: return "BRONZE";
    case MEDAL_SILVER: return "SILVER";
    case MEDAL_GOLD: return "GOLD";
    case MEDAL_PLATINUM: return "PLATINUM";
    case MEDAL_DIAMOND: return "DIAMOND";
    default: return "-";
    }
}

const char *medal_short(int tier)
{
    switch (tier) {
    case MEDAL_BRONZE: return "BRZ";
    case MEDAL_SILVER: return "SLV";
    case MEDAL_GOLD: return "GLD";
    case MEDAL_PLATINUM: return "PLT";
    case MEDAL_DIAMOND: return "DIA";
    default: return "---";
    }
}

/* Round up to a "nice" step so the ladder reads like real score targets
 * (100, 250, 1250, 1677000 ...) instead of arbitrary numbers. */
static int round_nice(int v)
{
    if (v <= 0) return 1;
    int step;
    if (v < 1000) step = 25;
    else if (v < 5000) step = 50;
    else if (v < 20000) step = 100;
    else if (v < 100000) step = 250;
    else if (v < 500000) step = 500;
    else step = 1000;
    int r = counter(((int64_t)v + step - 1) / step * step);
    return r < 1 ? 1 : r;
}

void medal_thresholds(int best, int out[MEDAL_COUNT])
{
    if (best < 0) best = 0;
    if (best < MEDAL_COUNT) {
        /* Five distinct positive integer targets cannot fit below a record <5. */
        for (int i = 0; i < MEDAL_COUNT; ++i) out[i] = (best * (i + 1) + 4) / 5;
        return;
    }
    static const int ratio[MEDAL_COUNT] = {10, 25, 45, 70, 100};
    for (int i = 0; i < MEDAL_COUNT; ++i) {
        int raw = counter(((int64_t)best * ratio[i] + 50) / 100);
        out[i] = (i == MEDAL_COUNT - 1) ? best : round_nice(raw);
    }
    /* Keep the ladder strictly increasing with a sensible gap, and never let a
     * threshold pass the record it is derived from. */
    int gap = best / 50;
    if (gap < 1) gap = 1;
    if (gap > 5000) gap = 5000;
    for (int i = MEDAL_COUNT - 2; i >= 0; --i) {
        int ceiling = out[i + 1] - gap;
        if (ceiling < 1) ceiling = 1;
        if (out[i] > ceiling) out[i] = ceiling;
        if (out[i] < 1) out[i] = 1;
    }
    /* A minimum bar so the very first medals are not handed out for nothing. */
    int floor = best / 4;
    if (floor > 100) floor = 100;
    if (floor > 0 && out[0] < floor) out[0] = floor;
    /* Re-verify monotonicity after the floor was applied. */
    for (int i = 1; i < MEDAL_COUNT; ++i) {
        if (out[i] <= out[i - 1]) out[i] = out[i - 1] + 1;
    }
    if (out[MEDAL_COUNT - 1] > best && best > 0) out[MEDAL_COUNT - 1] = best;
}

int medal_for_score(int score, int best)
{
    int t[MEDAL_COUNT];
    medal_thresholds(best, t);
    int earned = MEDAL_NONE;
    for (int i = 0; i < MEDAL_COUNT; ++i) {
        if (score >= t[i] && t[i] > 0) earned = i;
    }
    return earned;
}

void refresh_medal_thresholds(SaveData &s)
{
    int t[MEDAL_COUNT];
    medal_thresholds(s.best_score, t);
    for (int i = 0; i < MEDAL_COUNT; ++i) s.medal[i].threshold = t[i];
}

void save_defaults(SaveData &s)
{
    s = SaveData();
    s.best_score = 0;
    s.best_level = 1;
    refresh_medal_thresholds(s);
}

uint32_t save_today(void)
{
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    return (uint32_t)((tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday);
}

void save_path(char *out, int cap)
{
    if (!out || cap <= 0) return;
    const char *xdg = getenv("XDG_DATA_HOME");
    int n;
    if (xdg && xdg[0]) {
        n = std::snprintf(out, (size_t)cap, "%s/mini-space-shooter/save.txt", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0]) home = ".";
        n = std::snprintf(out, (size_t)cap, "%s/.local/share/mini-space-shooter/save.txt", home);
    }
    if (n < 0 || n >= cap) out[0] = '\0';
}

static bool ensure_parent_dir(const char *path)
{
    char dir[4096];
    std::snprintf(dir, sizeof(dir), "%s", path);
    char *slash = std::strrchr(dir, '/');
    if (!slash) return false;
    *slash = '\0';
    /* create each component; ignore EEXIST */
    for (char *p = dir + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        int result = mkdir(dir, 0755);
        *p = '/';
        if (result != 0 && errno != EEXIST) return false;
    }
    return mkdir(dir, 0755) == 0 || errno == EEXIST;
}

/* --------------------------------------------------------------- file I/O */
/* Parse a complete row without scanf's out-of-range integer conversions. */
static int parse_values(const char *text, int64_t values[4])
{
    const char *end = text + std::strlen(text);
    int count = 0;
    while (text < end) {
        while (text < end && std::isspace((unsigned char)*text)) ++text;
        if (text == end) break;
        if (count == 4) return -1;
        auto result = std::from_chars(text, end, values[count]);
        if (result.ec != std::errc() || (result.ptr < end && !std::isspace((unsigned char)*result.ptr))) return -1;
        if (values[count] < 0 || values[count] > INT_MAX) return -1;
        ++count;
        text = result.ptr;
    }
    return count;
}

static int board_compare(const LeaderEntry &a, const LeaderEntry &b)
{
    if (a.score != b.score) return a.score > b.score ? -1 : 1;
    if (a.level != b.level) return a.level > b.level ? -1 : 1;
    return 0;
}

int record_run(SaveData &s, int score, int level, int medal_mask, int seconds)
{
    score = counter(score);
    level = level < 1 ? 1 : level;
    seconds = counter(seconds);
    medal_mask &= (1 << MEDAL_COUNT) - 1;
    s.runs = counter((int64_t)s.runs + 1);
    if (score > s.best_score) s.best_score = score;
    if (level > s.best_level) s.best_level = level;
    if (seconds > s.best_run_seconds) s.best_run_seconds = seconds;
    s.total_seconds = counter((int64_t)s.total_seconds + seconds);

    /* Medal records: a tier's best-remembered score only ever grows, and the
     * date marks when it reached that height. */
    uint32_t today = save_today();
    int t[MEDAL_COUNT];
    medal_thresholds(s.best_score, t);
    for (int i = 0; i < MEDAL_COUNT; ++i) {
        if (!(medal_mask & (1 << i))) continue;
        s.medal[i].earned_count = counter((int64_t)s.medal[i].earned_count + 1);
        int represented = t[i];
        if (represented > s.medal[i].best_threshold) {
            if (s.medal[i].best_threshold > 0) {
                if (s.history_count == MEDAL_HISTORY) {
                    for (int j = 1; j < MEDAL_HISTORY; ++j) s.history[j - 1] = s.history[j];
                    --s.history_count;
                }
                /* keep the score that is being replaced: that is the memory
                 * the player actually cares about ("diamond at 145"). */
                s.history[s.history_count].date = s.medal[i].best_date;
                s.history[s.history_count].tier = i;
                s.history[s.history_count].score = s.medal[i].best_threshold;
                s.history_count++;
            }
            s.medal[i].best_threshold = represented;
            s.medal[i].best_date = today;
        }
    }
    refresh_medal_thresholds(s);

    /* Leaderboard insert (top 10). */
    LeaderEntry e;
    e.score = score;
    e.level = level;
    e.medal_mask = medal_mask;
    e.date = today;
    e.seconds = seconds;
    int pos = -1;
    for (int i = 0; i < s.board_count; ++i) {
        if (board_compare(e, s.board[i]) < 0) {
            pos = i;
            break;
        }
    }
    if (pos < 0 && s.board_count < LEADERBOARD_SIZE) pos = s.board_count;
    if (pos >= 0) {
        int last = s.board_count < LEADERBOARD_SIZE ? s.board_count : LEADERBOARD_SIZE - 1;
        for (int i = last; i > pos; --i) s.board[i] = s.board[i - 1];
        s.board[pos] = e;
        if (s.board_count < LEADERBOARD_SIZE) s.board_count++;
    }
    return pos;
}

bool save_load(SaveData &s)
{
    save_defaults(s);
    char path[4096];
    save_path(path, (int)sizeof(path));
    if (!path[0]) return false;
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;

    char line[256];
    bool any = false, supported = true;
    int last_board = -1;
    while (std::fgets(line, sizeof(line), f)) {
        if (!std::strchr(line, '\n') && !std::feof(f)) {
            /* Discard the whole oversized row, including plausible-looking tails. */
            int ch;
            while ((ch = std::fgetc(f)) != '\n' && ch != EOF) {}
            last_board = -1;
            continue;
        }
        char key[32] = {};
        int offset = 0;
        if (std::sscanf(line, " %31s %n", key, &offset) != 1 || key[0] == '#') continue;
        if (std::strcmp(key, "board") == 0) last_board = -1;
        int64_t v[4] = {};
        int n = parse_values(line + offset, v);
        if (std::strcmp(key, "version") == 0) {
            if (n != 1 || v[0] != 1) { supported = false; break; }
            any = true;
            continue;
        }
        if (n <= 0) continue;
        int a = (int)v[0], b = (int)v[1], c = (int)v[2], d = (int)v[3];
        int *scalar = nullptr;
        if (std::strcmp(key, "best_score") == 0) scalar = &s.best_score;
        else if (std::strcmp(key, "best_level") == 0 && a >= 1) scalar = &s.best_level;
        else if (std::strcmp(key, "runs") == 0) scalar = &s.runs;
        else if (std::strcmp(key, "kills") == 0) scalar = &s.kills;
        else if (std::strcmp(key, "deaths") == 0) scalar = &s.deaths;
        else if (std::strcmp(key, "seconds") == 0) scalar = &s.total_seconds;
        else if (std::strcmp(key, "best_run_seconds") == 0) scalar = &s.best_run_seconds;
        if (scalar && n == 1) { *scalar = a; any = true; }
        else if (std::strcmp(key, "medal") == 0 && n == 4 && a < MEDAL_COUNT && d <= 99991231) {
            s.medal[a].best_threshold = c;
            s.medal[a].best_date = (uint32_t)d;
            any = true;
        } else if (std::strcmp(key, "medal_earned") == 0 && n == 2 && a < MEDAL_COUNT) {
            s.medal[a].earned_count = b;
            any = true;
        } else if (std::strcmp(key, "board") == 0 && n == 4 && b >= 1 && c < (1 << MEDAL_COUNT) &&
                   d <= 99991231 && s.board_count < LEADERBOARD_SIZE) {
            last_board = s.board_count++;
            s.board[last_board] = LeaderEntry{a, b, c, (uint32_t)d, 0};
            any = true;
        } else if (std::strcmp(key, "board_seconds") == 0 && n == 1 && last_board >= 0) {
            s.board[last_board].seconds = a;
            last_board = -1;
        } else if (std::strcmp(key, "hist") == 0 && n == 3 && a <= 99991231 && b < MEDAL_COUNT &&
                   s.history_count < MEDAL_HISTORY) {
            s.history[s.history_count++] = HistEntry{(uint32_t)a, b, c};
            any = true;
        }
    }
    bool ok = any && supported && !std::ferror(f);
    if (std::fclose(f) != 0) ok = false;
    if (!ok) { save_defaults(s); return false; }
    /* Repair ordering and records from valid entries in a partially damaged file. */
    for (int i = 0; i < s.board_count; ++i) {
        LeaderEntry e = s.board[i];
        int j = i;
        while (j > 0 && board_compare(e, s.board[j - 1]) < 0) {
            s.board[j] = s.board[j - 1];
            --j;
        }
        s.board[j] = e;
        if (e.score > s.best_score) s.best_score = e.score;
        if (e.level > s.best_level) s.best_level = e.level;
        if (e.seconds > s.best_run_seconds) s.best_run_seconds = e.seconds;
    }
    refresh_medal_thresholds(s);
    return true;
}

bool save_store(const SaveData &s)
{
    char path[4096];
    save_path(path, (int)sizeof(path));
    if (!path[0] || !ensure_parent_dir(path)) return false;
    char temporary[4120];
    std::snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temporary);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(temporary); return false; }
    std::fprintf(f, "# Mini Space Shooter save file (plain text, safe to delete)\n");
    std::fprintf(f, "version 1\n");
    std::fprintf(f, "best_score %d\n", s.best_score);
    std::fprintf(f, "best_level %d\n", s.best_level);
    std::fprintf(f, "runs %d\n", s.runs);
    std::fprintf(f, "kills %d\n", s.kills);
    std::fprintf(f, "deaths %d\n", s.deaths);
    std::fprintf(f, "seconds %d\n", s.total_seconds);
    std::fprintf(f, "best_run_seconds %d\n", s.best_run_seconds);
    for (int i = 0; i < MEDAL_COUNT; ++i) {
        std::fprintf(f, "medal %d %d %d %u\n", i, s.medal[i].threshold, s.medal[i].best_threshold,
                     (unsigned)s.medal[i].best_date);
        std::fprintf(f, "medal_earned %d %d\n", i, s.medal[i].earned_count);
    }
    for (int i = 0; i < s.board_count && i < LEADERBOARD_SIZE; ++i) {
        std::fprintf(f, "board %d %d %d %u\n", s.board[i].score, s.board[i].level, s.board[i].medal_mask,
                     (unsigned)s.board[i].date);
        std::fprintf(f, "board_seconds %d\n", s.board[i].seconds);
    }
    for (int i = 0; i < s.history_count && i < MEDAL_HISTORY; ++i) {
        std::fprintf(f, "hist %u %d %d\n", (unsigned)s.history[i].date, s.history[i].tier, s.history[i].score);
    }
    bool ok = std::ferror(f) == 0;
    if (std::fflush(f) != 0) ok = false;
    if (ok && fsync(fd) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;
    if (ok && std::rename(temporary, path) != 0) ok = false;
    if (!ok) { unlink(temporary); return false; }
    /* Persist the directory entry as well as the file's contents. */
    char *slash = std::strrchr(path, '/');
    if (slash) *slash = '\0';
    int parent = open(path, O_RDONLY | O_DIRECTORY);
    if (parent < 0) return false;
    ok = fsync(parent) == 0;
    if (close(parent) != 0) ok = false;
    return ok;
}

const char *save_medal_mask_text(int mask, char *buf, int cap)
{
    if (cap <= 0) return buf;
    buf[0] = '\0';
    int n = 0;
    for (int i = MEDAL_COUNT - 1; i >= 0; --i) {
        if (!(mask & (1 << i))) continue;
        const char *sh = medal_short(i);
        for (int k = 0; sh[k] && n < cap - 1; ++k) buf[n++] = sh[k];
        if (n < cap - 1) buf[n++] = ' ';
    }
    if (n > 0) n--; /* drop the trailing space */
    buf[n < cap ? n : cap - 1] = '\0';
    return buf;
}

} /* namespace mss */
