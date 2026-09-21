/* game.hpp — entities, waves, collisions, scoring and the forever level loop.
 *
 * Everything is a fixed-capacity static pool: no allocation happens after
 * construction, so the memory footprint is a few kilobytes and completely
 * predictable.  The simulation runs on a fixed 1/60 s step.
 */
#ifndef MINI_GAME_HPP
#define MINI_GAME_HPP

#include <cstdint>

#include "art.hpp"
#include "director.hpp"
#include "minui.h"
#include "rng.hpp"
#include "save.hpp"

namespace mss {

constexpr float PLAY_W = 384.0f;
constexpr float PLAY_H = 216.0f;
constexpr float HUD_H = 15.0f;

constexpr int MAX_ENEMIES = Caps::MAX_ENEMIES;
constexpr int MAX_PBULLETS = 24;
constexpr int MAX_EBULLETS = Caps::MAX_ENEMY_BULLETS;
constexpr int MAX_PARTICLES = 144;
constexpr int MAX_STARS = 56;
constexpr int MAX_ALLIES = 4;
constexpr int MAX_ABULLETS = 48;
constexpr float FLEET_TOP = PLAY_H - 23.0f;

enum AllyKind : int { AK_SCOUT = 0, AK_WING, AK_CRUISER, AK_TITAN, AK_COUNT };
enum AllyAction : int { ALLY_GUARD = 0, ALLY_ATTACK, ALLY_EVADE, ALLY_ESCORT, ALLY_ACTIONS };
struct AllySpec {
    const char *name;
    int cost, hp, damage, volley;
    float speed, reload;
};
const AllySpec &ally_spec(int kind);

struct Ally {
    bool alive = false;
    int kind = 0, hp = 0, action = ALLY_GUARD;
    float x = 0, y = 0, fire_cd = 0, hurt = 0, decision_t = 0;
    bool policy_valid = false;
    float features[12]{}, reward = 0;
};

enum EnemyKind : int { EK_GRUNT = 0, EK_WASP, EK_BRUTE, EK_GHOST, EK_COUNT };

/* ML actions the shared policy chooses between. */
enum PolicyAction : int { ACT_ADVANCE = 0, ACT_STRAFE_L, ACT_STRAFE_R, ACT_RETREAT, ACT_COUNT };

struct Enemy {
    bool alive;
    int kind;
    float x, y;
    float vx, vy;
    float base_speed;
    int hp, max_hp;
    float fire_cd;
    float shot_guard; /* real time since last volley, independent of trick reloads */
    float bob_phase;
    float origin_x;
    /* learned behaviour */
    int action;
    bool policy_valid;
    float decision_t;
    float reward_acc;
    float features[8];
    /* trick support */
    float trick_t;
    float lane_drift;
    bool diving;
    float entered; /* seconds since spawn */
};

struct Bullet {
    bool alive;
    float x, y, vx, vy;
    int kind;     /* 0 = player, 1 = enemy small, 2 = enemy big */
    bool policy_valid;
    int action;
    float features[12]; /* firing policy snapshot; enemies use the first eight */
    float life;
    int damage = 1; /* allied projectiles; other pools keep their existing damage */
};

struct Particle {
    bool alive;
    float x, y, vx, vy;
    float life, max_life;
    int size;
    uint32_t col;
    uint8_t kind; /* 0 spark, 1 debris, 2 flash */
};

struct Star {
    float x, y, speed;
    uint8_t bright;
};

/* Single-pixel starfield palette (0x00RRGGBB). Tier 3 is the brightest
 * foreground star; every tier stays far below gameplay/UI brightness so the
 * backdrop never competes with ships and bullets. */
uint32_t star_color(uint8_t bright);

struct GameInput {
    float mx; /* -1..1 */
    float my; /* -1..1 */
    bool fire;
};

struct RunStats {
    int score = 0;
    int level = 1;
    int kills = 0;
    int deaths = 0;
    int medal_mask = 0;
    double seconds = 0.0;
    int shots = 0;
    int hits = 0;
    int ally_kills = 0, allies_bought = 0, allies_lost = 0, intercepted = 0;
};

class Game {
  public:
    /* keep_learning carries the enemy policy and player models across runs of
     * the same session, so the game remembers how you play. */
    void reset(Rng &rng, int starting_level, int record_score, bool keep_learning);
    void update(const GameInput &in, float dt);
    void draw(Mui &m, bool debug = false) const;
    /* Menu backdrop: a calm starfield with a drifting ship. */
    void update_attract(float dt);
    void draw_attract(Mui &m) const;

    bool over() const { return over_; }
    const RunStats &stats() const { return st_; }
    const Director &director() const { return dir_; }
    Director &director() { return dir_; }
    /* Highest score the medal ladder should be built from right now. */
    int ladder_best() const { return st_.score > record_score_ ? st_.score : record_score_; }
    int record_score() const { return record_score_; }
    void set_record_score(int v) { record_score_ = v; }
    float player_x() const { return p_.x; }
    float player_y() const { return p_.y; }
    int player_hp() const { return p_.hp; }
    bool medal_just_earned() const { return medal_banner_t_ > 0.0f; }
    int last_medal() const { return last_medal_; }
    /* Fairness telemetry, also used by the debug overlay and the self-tests. */
    int hot_lanes() const { return hot_count_; }
    int enemies_alive() const;
    /* Read-only observation for the scripted integration pilot. */
    int enemy_snapshot(Enemy *out, int capacity) const;
    int enemy_bullets() const;
    int allies_alive() const;
    int credits() const { return credits_; }
    bool recruit(int kind); /* one purchase per key press, no score deduction */
    const ml::Mlp &ally_policy() const { return ally_policy_; }
    /* Diagnostic snapshot of the inbound shot schedule (lane + seconds to the
     * playfield bottom).  Used by the debug overlay and the fairness tests. */
    int scheduled_shots(ScheduledShot *out, int max_shots) const;
    bool lane_is_marked(int lane) const { return lane >= 0 && lane < LANES && lane_hot_[lane] != 0; }

  private:
    friend struct SimulationTestAccess;
    struct Player {
        float x = PLAY_W * 0.5f, y = PLAY_H - 34.0f;
        float vx = 0.0f, vy = 0.0f;
        int hp = 3;
        float fire_cd = 0.0f;
        float invuln = 0.0f;
        float engine_phase = 0.0f;
        float hurt_flash = 0.0f;
    };

    /* ---- helpers ---- */
    void update_step(const GameInput &in, float dt);
    void finish_policy(Enemy &e, float reward);
    void spawn_wave(int trick);
    int spawn_enemy(EnemyKind kind, float x, float y, float vx, float vy);
    void spawn_player_bullet();
    bool enemy_fire(int idx, float aim_x, int patterns);
    void kill_enemy(int idx, bool by_player);
    void damage_player(float amount);
    void add_particle(float x, float y, float vx, float vy, float life, int size, uint32_t col, uint8_t kind);
    void explode(float x, float y, uint32_t col, int scale);
    void update_player_bullets(float dt);
    void update_enemy_bullets(float dt);
    void update_enemies(float dt);
    void award_points(int64_t points);
    void reset_allies(bool keep_learning);
    void update_allies(float dt);
    void update_ally_bullets(float dt);
    void ally_fire(Ally &a, const Enemy &target);
    void finish_ally_policy(Ally &a, float reward);
    void damage_ally(Ally &a, int amount, bool protecting);
    bool intercept_bullet(Bullet &b);
    void draw_fleet(Mui &m) const;
    void update_particles(float dt);
    void update_stars(float dt);
    void compute_lanes();
    void feed_director(float dt);
    void check_level_progress();
    void check_medals();
    float enemy_fire_interval(int kind) const;
    float enemy_speed(int kind) const;
    float cap_v_dive() const;
    int lane_of(float x) const;
    float lane_center(int lane) const;
    void policy_apply(Enemy &e, float dt);
    void policy_decide(Enemy &e);
    void trick_behaviour(Enemy &e, float dt);
    void draw_hud(Mui &m) const;
    void draw_banner(Mui &m) const;

    Rng rng_{};
    Player p_{};
    Enemy enemies_[MAX_ENEMIES]{};
    Bullet pbullets_[MAX_PBULLETS]{};
    Bullet ebullets_[MAX_EBULLETS]{};
    Ally allies_[MAX_ALLIES]{};
    Bullet abullets_[MAX_ABULLETS]{};
    ml::Mlp ally_policy_{};
    Rng ally_rng_{}; /* purchases and allied decisions don't consume director randomness */
    int credits_ = 0;
    float fleet_message_t_ = 0;
    char fleet_message_[48]{};
    Particle parts_[MAX_PARTICLES]{};
    Star stars_[MAX_STARS]{};
    Director dir_{};
    RunStats st_{};
    bool over_ = false;
    int record_score_ = 0;

    /* wave / spawn state */
    float spawn_timer_ = 0.0f;
    int wave_index_ = 0;
    int kills_for_level_ = 0;
    int level_target_kills_ = 10;
    int next_score_checkpoint_ = 2000;
    int last_trick_ = TRICK_NONE;

    /* fairness bookkeeping */
    uint8_t lane_hot_[LANES]{};
    int hot_count_ = 0;
    float volley_timer_ = 0.0f;
    /* Inbound shot schedule shared by the fairness gate and the overlay.
     * Rebuilt every frame in compute_lanes() and appended to as enemies fire. */
    ScheduledShot sched_[MAX_EBULLETS]{};
    int sched_count_ = 0;

    /* feedback */
    float banner_t_ = 0.0f;
    char banner_[32]{};
    float medal_banner_t_ = 0.0f;
    int last_medal_ = MEDAL_NONE;
    float shake_ = 0.0f;
    float hit_flash_ = 0.0f;
    float star_scroll_ = 0.0f;
    float star_seed_ = 0.0f;

    /* rolling player telemetry for the director */
    float time_since_damage_ = 99.0f;
    float near_miss_accum_ = 0.0f;
    float near_miss_rate_ = 0.0f;
    float shot_accum_ = 0.0f;
    float hit_accum_ = 0.0f;
    float fire_rate_window_ = 0.0f;
    float fire_rate_ = 0.0f;
    int shots_1s_ = 0;
    float dodge_window_ = 0.0f;
    float last_dir_ = 0.0f;
    float dir_changes_ = 0.0f;
    float decay_t_ = 0.0f;
    float lane_hot_prev_[LANES]{}; /* smooth corridor occupancy for the HUD graph */
    float attract_t_ = 0.0f;
    float attract_ship_x_ = PLAY_W * 0.5f;
};

} /* namespace mss */

#endif /* MINI_GAME_HPP */
