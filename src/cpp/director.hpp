/* director.hpp — difficulty, fairness and the "trick" scheduler.
 *
 * The one rule this file exists to enforce:
 *
 *   Keep adapting without exceeding bounded speeds, counts and firing rules.
 *
 * How that is achieved:
 *
 *  1. Every raw difficulty axis has a hard human cap (Caps below).  Speeds,
 *     enemy counts and fire rates saturate and simply stop growing.
 *  2. Beyond the caps the game grows in *variety and cunning*: the director
 *     runs a UCB1 bandit over a repertoire of tactical tricks and keeps the
 *     ones that stress this particular player the most.
 *  3. Fairness invariants are vetoes, not heuristics: a shot is refused when
 *     it would leave no escape lane, arrive with too little reaction time, or
 *     land during the post-hit grace window.
 *  4. A learned stress model watches the player and backs the pressure off
 *     *before* they die; recovery is fast, escalation is slow, and the player
 *     model resets on death.
 *
 * These bounds constrain pressure; they do not prove every player position
 * has a reachable safe route through bullets and enemy bodies.
 */
#ifndef MINI_DIRECTOR_HPP
#define MINI_DIRECTOR_HPP

#include <cstdint>

#include "ml.hpp"
#include "limits.hpp"
#include "rng.hpp"

namespace mss {

/* Five vertical corridors; the fairness rules talk about these. */
constexpr int LANES = 5;

struct Caps {
    /* Hard ceilings.  Nothing in the game may exceed these, ever. */
    static constexpr float ENEMY_BULLET_SPEED = 74.0f;  /* px/s */
    static constexpr float ENEMY_STRAFE_SPEED = 36.0f;  /* px/s */
    static constexpr float ENEMY_DIVE_SPEED = 46.0f;    /* px/s */
    static constexpr float MIN_FIRE_INTERVAL = 0.55f;   /* seconds between shots per enemy */
    static constexpr int MAX_ENEMIES = 12;
    static constexpr int MAX_ENEMY_BULLETS = 24;
    static constexpr float MIN_REACTION_TIME = 0.30f;   /* s before a new shot can reach the player */
    static constexpr float POST_HIT_GRACE = 1.10f;      /* s without enemy fire after damage */
    static constexpr int MAX_HOT_LANES_HARD = 4;        /* of LANES=5: one corridor is always open */
    static constexpr float SPAWN_MIN_INTERVAL = 0.85f;
    /* Corridor scheduling window.  Two shots count as "arriving together" when
     * their arrival times are within this many seconds, and never more than
     * `max_hot_lanes` distinct corridors may be scheduled inside one window.
     * That is what makes the escape-corridor guarantee exact rather than hopeful. */
    static constexpr float HOT_WINDOW = 1.0f;
};

/* One inbound enemy shot as the fairness rules see it.
 *
 *   lane     corridor swept where the shot leaves the playfield
 *   t_cross  seconds until it gets there (same reference row for every shot)
 *
 * Both values are fixed at spawn time and every shot's t_cross decreases at
 * exactly 1 s/s for its entire life (it is measured on the bullet's straight
 * trajectory and never clamped at the playfield edge), so the relation "these
 * two shots arrive within HOT_WINDOW of each other" cannot change after the
 * fact.  That invariance is what lets the guarantee below be proven instead of
 * approximated. */
struct ScheduledShot {
    uint8_t lane = 0;
    float t_cross = 0.0f;
};

/* Tactical patterns the bandit chooses between.  Each has a fair "tell"
 * (a subtle sound + visual cue) before it commits. */
enum Trick : int {
    TRICK_NONE = 0,
    TRICK_FEINT_DIVE,  /* dive at the player, pull back at the last moment */
    TRICK_PINCER,      /* two columns close in from both edges, middle stays open */
    TRICK_LANE_DENY,   /* plug two lanes with slow shots while a shooter takes the third */
    TRICK_AMBUSH,      /* full silence, then one coordinated volley on entry */
    TRICK_DECOY,       /* a cheap rusher draws fire while a shooter lines up */
    TRICK_RHYTHM_BREAK,/* alternating fast/slow cadence to break the player's timing */
    TRICK_SIEGE,       /* armoured ships crawl down with wide spreads */
    TRICK_SPLIT,       /* formation splits, wraps around and regroups behind you */
    TRICK_COUNT
};

const char *trick_name(int id);

/* Phase machine: pressure is delivered in waves of tension and release. */
enum Phase : int {
    PHASE_CALM = 0,
    PHASE_BUILD,
    PHASE_SPIKE,
    PHASE_TRICK,
    PHASE_COOLDOWN
};

const char *phase_name(int id);

struct DirectorInput {
    float dt = 0.0f;
    float player_x = 0.0f;      /* normalised 0..1 */
    float player_vx = 0.0f;     /* normalised units/s */
    float player_fire_rate = 0.0f;
    float player_accuracy = 0.0f;  /* 0..1 rolling */
    float player_dodge_rate = 0.0f;
    float near_miss_rate = 0.0f;   /* per second, rolling */
    int hits_taken_recent = 0;     /* last 5 s */
    float time_since_damage = 99.0f;
    int enemies_alive = 0;
    int enemy_bullets = 0;
    int lanes_hot = 0;             /* count of corridors about to receive fire */
    int level = 1;
    float score = 0.0f;
    int player_hp = 3;
    /* Hull capacity, so the models can read "fraction of hull left" instead of
     * assuming the stock three hearts once the upgrade ladder is in play. */
    int player_max_hp = 3;
};

struct DirectorOutput {
    /* What the game should build this frame. */
    float enemy_speed_mul = 1.0f;
    float bullet_speed = 40.0f;
    float fire_interval = 1.4f;
    float spawn_interval = 2.2f;
    int max_enemies = 3;
    int max_bullets = 12;
    int max_hot_lanes = 2;
    float aggression = 0.2f;     /* 0..1: strafing / diving tendency */
    float coordination = 0.0f;   /* 0..1: how synchronised volleys are */
    float grace = 0.0f;          /* seconds of protected calm left */

    /* Trick in flight (or TRICK_NONE). */
    int trick = TRICK_NONE;
    float trick_intensity = 0.0f; /* 0..1 ramp inside the trick */
    float trick_progress = 0.0f;  /* 0..1 through its duration */
    int trick_choices_used = 0;

    /* Read-only telemetry for the HUD / debug overlay. */
    float pressure = 0.0f;
    float mercy = 0.0f;
    float weirdness = 0.0f;
    float dominance = 0.0f;
    float stress_prob = 0.0f;
    int phase = PHASE_CALM;
    const char *phase_label = "calm";
};

class Director {
  public:
    void reset(Rng &rng, int starting_level, bool keep_learning);
    void observe(const DirectorInput &in);
    /* Game events. */
    void event_player_damage();
    void event_player_death();
    void event_enemy_killed();
    void event_level_up(int level);

    /* ---- fairness gate ----
     * May a shot be fired into `lane`, reaching the playfield bottom in
     * `t_bottom` seconds and the player's row in `t_player` seconds, given the
     * shots already scheduled?  Returns false when the shot would break an
     * invariant (no reaction time, post-hit grace, or no corridor left free).
     *
     * Invariant it maintains: inside any HOT_WINDOW-sized interval, enemy fire
     * arrives in at most `max_hot_lanes` (<= 4) of the 5 corridors, therefore
     * at least one corridor is always free of inbound fire. */
    bool may_fire_into(int lane, float t_bottom, float t_player, const ScheduledShot *shots, int n) const;

    /* Distinct corridors receiving fire within HOT_WINDOW of `t_cross`.
     * `lane_flags` (optional, LANES entries) reports which ones. */
    int lanes_in_window(const ScheduledShot *shots, int n, float t_cross, uint8_t *lane_flags) const;

    /* Enemy policy learning: shared net + per-ship rewards. */
    const float *policy_forward(const float *features) { return policy_.forward(features); }
    void policy_probs(float *probs, int n) const { policy_.softmax(probs, n); }
    void policy_learn(const float *features, int action, float advantage, float entropy_bonus)
    {
        policy_.train_policy(features, action, advantage, entropy_bonus);
    }
    const ml::Mlp &policy_net() const { return policy_; }
    /* Turns a raw reward into a bounded advantage against a running baseline. */
    float policy_advantage(float reward)
    {
        float adv = reward - policy_reward_ema_.v;
        policy_reward_ema_.update(reward);
        if (adv > 1.0f) adv = 1.0f;
        if (adv < -1.0f) adv = -1.0f;
        return adv;
    }

    /* Where the player is heading (used for shot leading).  Returns -1..1. */
    float intent_bias() const { return intent_bias_; }
    float stress_prob() const { return stress_prob_; }
    const char *style_label() const { return style_label_; }
    int style_cluster() const { return style_cluster_; }

    const DirectorOutput &output() const { return out_; }
    /* True while the "I am about to overwhelm you" model wants a breather. */
    bool backing_off() const { return mercy_ > 0.45f; }

    int tricks_tried() const { return counter(bandit_.total); }
    const ml::Bandit &bandit() const { return bandit_; }
    const ml::Logit &stress_model() const { return stress_; }

  private:
    friend struct SimulationTestAccess;
    void update_player_model(const DirectorInput &in);
    void update_stress_model(const DirectorInput &in);
    void update_style_model(const DirectorInput &in);
    void tick_phase(const DirectorInput &in);
    void start_trick(Rng &rng, const DirectorInput &in);
    void end_trick();
    float compute_pressure(const DirectorInput &in) const;
    float compute_dominance(const DirectorInput &in) const;

    Rng rng_{};
    bool initialized_ = false;

    DirectorOutput out_{};
    DirectorInput in_{};

    /* phase machine */
    int phase_ = PHASE_CALM;
    float phase_time_ = 0.0f;
    float phase_length_ = 3.0f;

    /* difficulty state */
    float base_pressure_ = 0.12f;
    float weirdness_ = 0.0f;
    float mercy_ = 0.0f;
    float dominance_ = 0.3f;
    float grace_ = 1.6f;
    float calm_bonus_ = 0.0f;
    int level_ = 1;
    float run_time_ = 0.0f;
    int kills_since_level_ = 0;

    /* trick scheduling */
    int trick_ = TRICK_NONE;
    float trick_time_ = 0.0f;
    float trick_length_ = 4.0f;
    float trick_stress_acc_ = 0.0f;
    float trick_fair_ = 1.0f;
    int trick_arm_ = 0;
    float trick_cooldown_ = 2.5f;
    uint8_t seen_arm_[ml::MAX_ARMS];

    /* learned models */
    ml::Mlp policy_;
    ml::Logit stress_;
    ml::Logit intent_;
    ml::KMeans style_;
    ml::Bandit bandit_;

    /* intent predictor ring (delayed labels) */
    static constexpr int INTENT_DELAY = 15; /* frames at 60 Hz ~ 0.25 s */
    float intent_ring_[INTENT_DELAY][ml::MAX_IN];
    int intent_head_ = 0;
    int intent_filled_ = 0;
    float intent_bias_ = 0.0f;
    float last_player_x_ = 0.5f;
    float dir_change_timer_ = 0.0f;
    int last_dir_ = 0;

    /* stress model ring (~2 s of features) */
    static constexpr int STRESS_DELAY = 120;
    static constexpr int STRESS_DIM = 6;
    float stress_ring_[STRESS_DELAY][STRESS_DIM];
    int stress_head_ = 0;
    int stress_filled_ = 0;
    float stress_prob_ = 0.0f;
    float damage_timer_ = 99.0f;
    int damage_flag_ = 0; /* set when a hit happens inside the current window */

    /* style model */
    float x_ema_ = 0.5f;
    float x_var_ema_ = 0.0f;
    float fire_ema_ = 0.0f;
    float dodge_ema_ = 0.0f;
    int style_cluster_ = 0;
    enum Style { DRIFTER, WEAVER, SPRAYER, HUGGER };
    Style style_kind_ = DRIFTER;
    bool style_seeded_ = false;
    const char *style_label_ = "unknown";
    float style_timer_ = 0.0f;

    /* rolling player performance */
    ml::Ema accuracy_ema_{};
    ml::Ema near_miss_ema_{};
    ml::Ema policy_reward_ema_{};
    int hits_window_[5] = {0, 0, 0, 0, 0}; /* 5 one-second buckets */
    int hits_window_idx_ = 0;
    float hits_window_t_ = 0.0f;
};

} /* namespace mss */

#endif /* MINI_DIRECTOR_HPP */
