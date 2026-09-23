/* director.cpp — difficulty curve, fairness vetoes and trick scheduling. */
#include "director.hpp"

#include <cmath>

namespace mss {

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * clampf(t, 0.0f, 1.0f); }

const char *trick_name(int id)
{
    switch (id) {
    case TRICK_FEINT_DIVE: return "FEINT DIVE";
    case TRICK_PINCER: return "PINCER";
    case TRICK_LANE_DENY: return "LANE DENY";
    case TRICK_AMBUSH: return "AMBUSH";
    case TRICK_DECOY: return "DECOY";
    case TRICK_RHYTHM_BREAK: return "RHYTHM BREAK";
    case TRICK_SIEGE: return "SIEGE";
    case TRICK_SPLIT: return "SPLIT";
    default: return "steady";
    }
}

const char *phase_name(int id)
{
    switch (id) {
    case PHASE_CALM: return "calm";
    case PHASE_BUILD: return "build";
    case PHASE_SPIKE: return "spike";
    case PHASE_TRICK: return "trick";
    case PHASE_COOLDOWN: return "cooldown";
    default: return "?";
    }
}

/* Fixed duration per trick; the bandit learns which ones work on you. */
static float trick_duration(int id)
{
    switch (id) {
    case TRICK_FEINT_DIVE: return 5.0f;
    case TRICK_PINCER: return 6.0f;
    case TRICK_LANE_DENY: return 5.5f;
    case TRICK_AMBUSH: return 4.5f;
    case TRICK_DECOY: return 5.0f;
    case TRICK_RHYTHM_BREAK: return 6.5f;
    case TRICK_SIEGE: return 7.0f;
    case TRICK_SPLIT: return 6.0f;
    default: return 4.0f;
    }
}

/* ------------------------------------------------------------------- reset */
void Director::reset(Rng &rng, int starting_level, bool keep_learning)
{
    keep_learning = keep_learning && initialized_;
    rng_ = rng;
    level_ = starting_level < 1 ? 1 : starting_level;
    run_time_ = 0.0f;
    kills_since_level_ = 0;
    phase_ = PHASE_CALM;
    phase_time_ = 0.0f;
    phase_length_ = 3.2f;
    weirdness_ = 1.0f - std::exp(-(float)(level_ - 1) / 22.0f);
    mercy_ = 0.55f;   /* start gentle and let the model take over */
    dominance_ = 0.30f;
    grace_ = 2.0f;
    calm_bonus_ = 0.0f;
    base_pressure_ = 0.10f;
    trick_ = TRICK_NONE;
    trick_time_ = 0.0f;
    trick_cooldown_ = 3.0f;
    damage_timer_ = 99.0f;
    damage_flag_ = 0;
    intent_head_ = 0;
    intent_filled_ = 0;
    intent_bias_ = 0.0f;
    last_player_x_ = 0.5f;
    dir_change_timer_ = 0.0f;
    last_dir_ = 0;
    stress_head_ = 0;
    stress_filled_ = 0;
    stress_prob_ = 0.0f;
    x_ema_ = 0.5f;
    x_var_ema_ = 0.0f;
    fire_ema_ = 0.0f;
    dodge_ema_ = 0.0f;
    style_cluster_ = 0;
    style_kind_ = DRIFTER;
    style_label_ = "reading...";
    style_timer_ = 0.0f;
    for (int i = 0; i < 5; ++i) hits_window_[i] = 0;
    hits_window_idx_ = 0;
    hits_window_t_ = 0.0f;
    accuracy_ema_.init(0.3f, 0.02f);
    near_miss_ema_.init(0.0f, 0.05f);

    if (!keep_learning) {
        policy_.init(8, 10, 4, rng_, 0.010f);
        stress_.init(STRESS_DIM, 0.20f);
        intent_.init(6, 0.15f);
        bandit_.init(TRICK_COUNT - 1); /* every arm corresponds to a real trick */
        policy_reward_ema_.init(0.0f, 0.03f);
        float sample[4] = {0.3f, 0.5f, 0.3f, 0.3f};
        style_.init(4, 4, rng_, sample);
        style_seeded_ = false;
        for (int i = 0; i < ml::MAX_ARMS; ++i) seen_arm_[i] = 0;
        seen_arm_[TRICK_NONE] = 1;
    }
    initialized_ = true;
    stress_.lr = 0.20f;
    out_ = DirectorOutput();
    out_.grace = grace_;
}

/* ----------------------------------------------------------------- output */
float Director::compute_dominance(const DirectorInput &in) const
{
    /* Fraction of hull remaining.  The stock ship reads exactly as before
     * (max 3 -> divide by 2); upgraded hulls scale with their own capacity so
     * a 13-hull ship with 7 left is not read as "maximum dominance". */
    int span = in.player_max_hp > 1 ? in.player_max_hp - 1 : 1;
    float hp_term = (float)(in.player_hp - 1) / (float)span;
    float calm_term = clampf(in.time_since_damage / 15.0f, 0.0f, 1.0f);
    float acc_term = clampf(in.player_accuracy * 2.2f, 0.0f, 1.0f);
    float kill_term = clampf((float)in.level / 12.0f, 0.0f, 1.0f);
    float d = 0.30f * hp_term + 0.32f * calm_term + 0.26f * acc_term + 0.12f * kill_term;
    return clampf(d, 0.0f, 1.0f);
}

void Director::observe(const DirectorInput &raw)
{
    if (!initialized_ || !std::isfinite(raw.dt) || raw.dt <= 0.0f) return;
    DirectorInput in = raw;
    in.dt = clampf(raw.dt, 0.0f, 0.1f);
    if (in.level < 1) in.level = 1;
    in_ = in;
    float dt = in.dt;
    run_time_ = clampf(run_time_ + dt, 0.0f, 900.0f);
    level_ = in.level;

    /* Rolling 5-second hit counter in one-second buckets. */
    hits_window_t_ += dt;
    while (hits_window_t_ >= 1.0f) {
        hits_window_t_ -= 1.0f;
        hits_window_idx_ = (hits_window_idx_ + 1) % 5;
        hits_window_[hits_window_idx_] = 0;
    }
    damage_timer_ = clampf(damage_timer_ + dt, 0.0f, 99.0f);
    stress_.lr += (0.20f - stress_.lr) * dt;

    update_player_model(in);
    update_stress_model(in);
    update_style_model(in);

    dominance_ += (compute_dominance(in) - dominance_) * clampf(dt * 0.5f, 0.0f, 1.0f);

    /* Mercy: rises fast when the stress model predicts a hit, decays slowly. */
    float stress = stress_prob_;
    if (stress > 0.45f) {
        mercy_ += dt * (1.2f + 2.6f * stress);
    } else if (stress < 0.30f) {
        mercy_ -= dt * 0.42f;
    }
    if (in.player_hp <= 1) mercy_ += dt * 0.5f; /* last life: be kinder */
    int recent = 0;
    for (int i = 0; i < 5; ++i) recent += hits_window_[i];
    if (recent >= 3) mercy_ = mercy_ > 0.85f ? mercy_ : 0.85f;
    mercy_ = clampf(mercy_, 0.0f, 1.0f);

    if (trick_ != TRICK_NONE) {
        trick_time_ += dt;
        trick_stress_acc_ += dt * clampf(stress_prob_ + in.near_miss_rate * 0.1f, 0.0f, 1.0f);
    }
    tick_phase(in);

    /* ---- pressure ---- */
    float p = compute_pressure(in);
    if (phase_ == PHASE_CALM) p *= 0.42f;
    if (phase_ == PHASE_SPIKE) p *= 1.12f;
    if (phase_ == PHASE_BUILD) p *= lerpf(0.55f, 1.0f, phase_time_ / (phase_length_ > 0.01f ? phase_length_ : 1.0f));
    p = clampf(p, 0.03f, 1.0f);

    out_.pressure = p;
    out_.mercy = mercy_;
    out_.weirdness = weirdness_;
    out_.dominance = dominance_;
    out_.stress_prob = stress_prob_;
    out_.phase = phase_;
    out_.phase_label = phase_name(phase_);
    out_.grace = grace_ > 0.0f ? grace_ : 0.0f;

    /* ---- derived, always inside the human caps ---- */
    out_.bullet_speed = clampf(lerpf(34.0f, Caps::ENEMY_BULLET_SPEED, p), 30.0f, Caps::ENEMY_BULLET_SPEED);
    out_.fire_interval = clampf(lerpf(1.75f, Caps::MIN_FIRE_INTERVAL, p), Caps::MIN_FIRE_INTERVAL, 2.5f);
    out_.enemy_speed_mul = clampf(lerpf(0.62f, 1.18f, p), 0.5f, 1.25f);
    out_.spawn_interval = clampf(lerpf(2.70f, 0.95f, p), Caps::SPAWN_MIN_INTERVAL, 3.2f);
    out_.max_enemies = (int)(lerpf(2.0f, (float)Caps::MAX_ENEMIES, p) + 0.5f);
    out_.max_bullets = (int)(lerpf(5.0f, (float)Caps::MAX_ENEMY_BULLETS, p) + 0.5f);
    /* The escape-corridor budget: never all five lanes. */
    int hot_budget = 2 + (level_ / 12);
    out_.max_hot_lanes = hot_budget > Caps::MAX_HOT_LANES_HARD ? Caps::MAX_HOT_LANES_HARD : hot_budget;
    out_.aggression = clampf(p * 0.85f + weirdness_ * 0.15f, 0.0f, 1.0f);
    out_.coordination = clampf(p * 0.62f + (phase_ == PHASE_SPIKE ? 0.28f : 0.0f) +
                                   (trick_ == TRICK_AMBUSH ? 0.35f : 0.0f) + weirdness_ * 0.10f,
                               0.0f, 1.0f);

    /* ---- trick in flight ---- */
    if (trick_ != TRICK_NONE) {
        out_.trick = trick_;
        out_.trick_progress = clampf(trick_time_ / (trick_length_ > 0.01f ? trick_length_ : 1.0f), 0.0f, 1.0f);
        /* ramp in fast, hold, ease out: intensity is about commitment, not speed */
        float t = out_.trick_progress;
        float ramp = t < 0.15f ? t / 0.15f : (t > 0.85f ? (1.0f - t) / 0.15f : 1.0f);
        out_.trick_intensity = clampf(ramp * (0.35f + 0.65f * weirdness_), 0.0f, 1.0f);
    } else {
        out_.trick = TRICK_NONE;
        out_.trick_intensity = 0.0f;
        out_.trick_progress = 0.0f;
    }
    out_.trick_choices_used = tricks_tried();

    /* grace decays last so the same frame that consumed it still reports it */
    if (grace_ > 0.0f) grace_ -= dt;
}

float Director::compute_pressure(const DirectorInput &in) const
{
    float level_term = 1.0f - std::exp(-(float)(level_ - 1) / 13.0f);
    float base = 0.10f + 0.90f * level_term;
    float run_term = clampf(run_time_ / 900.0f, 0.0f, 1.0f) * 0.06f;
    float audience = 1.0f + 0.22f * dominance_;          /* push when you are cruising */
    float brake = 1.0f - 0.55f * mercy_;                 /* back off when you struggle */
    float p = (base + run_term) * audience * brake - calm_bonus_;
    (void)in;
    return clampf(p, 0.02f, 1.0f);
}

/* ------------------------------------------------------------- phase machine */
void Director::tick_phase(const DirectorInput &in)
{
    phase_time_ += in.dt;
    bool stressed = mercy_ > 0.45f || in.player_hp <= 1;
    switch (phase_) {
    case PHASE_CALM:
        if (phase_time_ >= phase_length_) {
            phase_ = PHASE_BUILD;
            phase_time_ = 0.0f;
            phase_length_ = lerpf(6.0f, 3.4f, out_.pressure) * (stressed ? 1.5f : 1.0f);
        }
        break;
    case PHASE_BUILD:
        if (phase_time_ >= phase_length_) {
            if (stressed) {
                phase_ = PHASE_COOLDOWN;
                phase_time_ = 0.0f;
                phase_length_ = 2.5f;
                calm_bonus_ = 0.12f;
                break;
            }
            bool may_spike = dominance_ > 0.34f && mercy_ < 0.40f && level_ >= 3 && trick_cooldown_ <= 0.0f;
            if (may_spike) {
                phase_ = PHASE_SPIKE;
                phase_time_ = 0.0f;
                phase_length_ = 1.8f + 1.6f * dominance_;
            } else {
                start_trick(rng_, in);
            }
        }
        break;
    case PHASE_SPIKE:
        if (phase_time_ >= phase_length_) {
            /* A spike is always followed by a visible calm: the shock needs a
             * "safe" contrast or it just reads as a difficulty wall. */
            start_trick(rng_, in);
        }
        break;
    case PHASE_TRICK:
        if (phase_time_ >= trick_length_) end_trick();
        break;
    case PHASE_COOLDOWN:
        if (phase_time_ >= phase_length_) {
            phase_ = PHASE_CALM;
            phase_time_ = 0.0f;
            phase_length_ = lerpf(2.0f, 3.6f, mercy_);
            calm_bonus_ = 0.0f;
        }
        break;
    default:
        break;
    }
    if (trick_cooldown_ > 0.0f) trick_cooldown_ -= in.dt;
}

void Director::start_trick(Rng &rng, const DirectorInput &in)
{
    /* Choose an arm.  The bandit explores trick variety; the player style
     * occasionally overrides it so the repertoire feels aimed at *you*. */
    int arm = bandit_.select(rng, 0.95f - 0.45f * weirdness_) + 1;
    if (bandit_.total >= (uint32_t)(TRICK_COUNT - 1) && style_timer_ > 12.0f && rng.chance(0.45f)) {
        int preferred[3];
        int n = 0;
        if (style_kind_ == DRIFTER) { /* tight horizontal play: squeeze it */
            preferred[n++] = TRICK_PINCER;
            preferred[n++] = TRICK_LANE_DENY;
            preferred[n++] = TRICK_SIEGE;
        } else if (style_kind_ == WEAVER) { /* constant weaver: change the rhythm */
            preferred[n++] = TRICK_RHYTHM_BREAK;
            preferred[n++] = TRICK_AMBUSH;
            preferred[n++] = TRICK_FEINT_DIVE;
        } else if (style_kind_ == SPRAYER) { /* sprayer: make shots expensive */
            preferred[n++] = TRICK_DECOY;
            preferred[n++] = TRICK_SPLIT;
            preferred[n++] = TRICK_SIEGE;
        } else {
            preferred[n++] = TRICK_SPLIT;
            preferred[n++] = TRICK_FEINT_DIVE;
            preferred[n++] = TRICK_PINCER;
        }
        arm = preferred[rng.range(n)];
    }
    if (arm <= TRICK_NONE || arm >= TRICK_COUNT) {
        /* fall back to a plain pressure build rather than an empty trick */
        phase_ = PHASE_SPIKE;
        phase_time_ = 0.0f;
        phase_length_ = 2.0f + 1.5f * dominance_;
        trick_cooldown_ = 3.0f + 2.0f * mercy_;
        (void)in;
        return;
    }

    trick_ = arm;
    trick_arm_ = arm;
    trick_time_ = 0.0f;
    trick_length_ = trick_duration(arm);
    trick_stress_acc_ = 0.0f;
    trick_fair_ = 1.0f;
    phase_ = PHASE_TRICK;
    phase_time_ = 0.0f;
    /* A fresh trick is always preceded by a real lull: the calm before it. */
    calm_bonus_ = 0.10f;
}

void Director::end_trick()
{
    /* Reward = how much it stressed the player, discounted when it was unfair
     * (a hit that the invariants should have prevented) or when it killed. */
    float stress = clampf(trick_stress_acc_ / (trick_length_ > 0.01f ? trick_length_ : 1.0f), 0.0f, 1.0f);
    float fairness = in_.player_hp > 0 ? trick_fair_ : 0.0f;
    float reward = clampf(stress * 0.75f + 0.15f, 0.0f, 1.0f) * fairness;
    if (in_.player_hp <= 0) reward = -1.0f;
    if (seen_arm_[trick_arm_] == 0) reward += 0.12f; /* novelty: try everything once */
    seen_arm_[trick_arm_] = 1;
    bandit_.update(trick_arm_ - 1, reward);

    trick_ = TRICK_NONE;
    trick_time_ = 0.0f;
    trick_cooldown_ = clampf(3.6f - 1.6f * weirdness_, 1.2f, 3.6f) + 2.0f * mercy_;
    phase_ = PHASE_COOLDOWN;
    phase_time_ = 0.0f;
    phase_length_ = 1.2f + 2.0f * mercy_;
    calm_bonus_ = 0.06f + 0.12f * mercy_;
}

/* ------------------------------------------------------------ player models */
void Director::update_player_model(const DirectorInput &in)
{
    float dt = in.dt;
    float dx = in.player_x - last_player_x_;
    if (std::fabs(dx) > 0.004f) {
        int dir = dx > 0 ? 1 : -1;
        if (dir != last_dir_) {
            dir_change_timer_ = 0.0f;
            last_dir_ = dir;
        }
    }
    dir_change_timer_ = clampf(dir_change_timer_ + dt, 0.0f, 2.0f);
    last_player_x_ = in.player_x;

    float x[6];
    /* features fed to the intent predictor */
    x[0] = in.player_x * 2.0f - 1.0f;
    x[1] = clampf(in.player_vx * 2.5f, -1.5f, 1.5f);
    x[2] = clampf(dir_change_timer_ / 1.5f, 0.0f, 1.0f);
    x[3] = clampf(in.player_fire_rate / 4.0f, 0.0f, 1.5f);
    x[4] = clampf((float)in.enemy_bullets / 24.0f, 0.0f, 1.5f);
    x[5] = clampf((float)in.enemies_alive / 12.0f, 0.0f, 1.5f);

    /* label: did the player actually move right over the delay window? */
    int old = intent_head_; /* consume the oldest sample before replacing it */
    if (intent_filled_ >= INTENT_DELAY) {
        float past_x = intent_ring_[old][0] * 0.5f + 0.5f;
        float displacement = in.player_x - past_x;
        float label = std::fabs(displacement) <= 0.002f ? 0.5f : (displacement > 0.0f ? 1.0f : 0.0f);
        intent_.train(intent_ring_[old], label);
    }
    for (int i = 0; i < 6; ++i) intent_ring_[intent_head_][i] = x[i];
    intent_head_ = (intent_head_ + 1) % INTENT_DELAY;
    if (intent_filled_ < INTENT_DELAY) intent_filled_++;
    /* Lead bias in [-1, 1]: how strongly the enemy should aim ahead. */
    float prediction = intent_.prob(x); /* inference uses today's features */
    float conf = std::fabs(prediction - 0.5f) * 2.0f;
    intent_bias_ += ((prediction * 2.0f - 1.0f) * clampf(conf * 1.4f, 0.0f, 1.0f) - intent_bias_) * 0.06f;
    intent_bias_ = clampf(intent_bias_, -1.0f, 1.0f);
}

void Director::update_stress_model(const DirectorInput &in)
{
    float f[STRESS_DIM];
    f[0] = out_.pressure;
    f[1] = clampf((float)in.enemies_alive / 12.0f, 0.0f, 1.0f);
    f[2] = clampf((float)in.enemy_bullets / 24.0f, 0.0f, 1.0f);
    f[3] = clampf(in.near_miss_rate / 4.0f, 0.0f, 1.5f);
    int recent = 0;
    for (int i = 0; i < 5; ++i) recent += hits_window_[i];
    f[4] = clampf((float)recent / 3.0f, 0.0f, 1.0f);
    f[5] = 1.0f - clampf(in.time_since_damage / 3.0f, 0.0f, 1.0f);

    int old = stress_head_;
    if (stress_filled_ >= STRESS_DELAY) {
        float label = damage_flag_ > 0 ? 1.0f : 0.0f;
        stress_.train(stress_ring_[old], label);
    }
    for (int i = 0; i < STRESS_DIM; ++i) stress_ring_[stress_head_][i] = f[i];
    stress_head_ = (stress_head_ + 1) % STRESS_DELAY;
    if (stress_filled_ < STRESS_DELAY) stress_filled_++;
    if (damage_flag_ > 0) damage_flag_--;

    stress_prob_ = stress_.prob(f);
}

void Director::update_style_model(const DirectorInput &in)
{
    float dt = in.dt;
    float x = in.player_x;
    x_ema_ += (x - x_ema_) * clampf(dt * 0.6f, 0.0f, 1.0f);
    float dev = x - x_ema_;
    x_var_ema_ += (dev * dev - x_var_ema_) * clampf(dt * 0.35f, 0.0f, 1.0f);
    fire_ema_ += (in.player_fire_rate - fire_ema_) * clampf(dt * 0.35f, 0.0f, 1.0f);
    dodge_ema_ += (in.player_dodge_rate - dodge_ema_) * clampf(dt * 0.35f, 0.0f, 1.0f);

    float feat[4];
    feat[0] = clampf(x_var_ema_ * 26.0f, 0.0f, 1.0f);
    feat[1] = clampf(x, 0.0f, 1.0f);
    feat[2] = clampf(fire_ema_ / 4.0f, 0.0f, 1.0f);
    feat[3] = clampf(dodge_ema_ / 3.0f, 0.0f, 1.0f);

    if (!style_seeded_) {
        /* seed centroids from the first real sample */
        float s[4];
        for (int i = 0; i < 4; ++i) s[i] = feat[i];
        style_.init(4, 4, rng_, s);
        style_seeded_ = true;
    } else {
        style_.update(feat);
    }
    style_timer_ = clampf(style_timer_ + dt, 0.0f, 13.0f);

    if (style_timer_ > 12.0f) {
        style_cluster_ = style_.dominant();
        const float *c = style_.c[style_cluster_];
        if (c[2] > 0.62f) { style_label_ = "SPRAYER"; style_kind_ = SPRAYER; }
        else if (c[0] > 0.45f) { style_label_ = "WEAVER"; style_kind_ = WEAVER; }
        else if (c[1] < 0.28f || c[1] > 0.72f) { style_label_ = "HUGGER"; style_kind_ = HUGGER; }
        else { style_label_ = "DRIFTER"; style_kind_ = DRIFTER; }
    }
}

/* ------------------------------------------------------------------ events */
void Director::event_player_damage()
{
    damage_timer_ = 0.0f;
    damage_flag_ = STRESS_DELAY; /* label the two seconds leading up to this hit */
    hits_window_[hits_window_idx_]++;
    mercy_ = 1.0f;
    grace_ = Caps::POST_HIT_GRACE;
    calm_bonus_ = 0.10f;
    if (trick_ != TRICK_NONE) {
        trick_fair_ = 0.70f; /* it connected once: still fair, but less impressive */
        trick_stress_acc_ += 0.35f;
    }
    /* A hit means our model of "safe" was wrong: relearn harder for a moment. */
    stress_.lr = 0.30f;
}

void Director::event_player_death()
{
    /* A terminal hit must label pending observations before reset discards them. */
    for (int i = 0; i < stress_filled_; ++i) stress_.train(stress_ring_[i], 1.0f);
    stress_filled_ = 0;
    in_.player_hp = 0;
    mercy_ = 1.0f;
    grace_ = 2.4f;
    phase_ = PHASE_CALM;
    phase_time_ = 0.0f;
    phase_length_ = 4.0f;
    dominance_ = 0.1f;
    if (trick_ != TRICK_NONE) {
        trick_fair_ = 0.0f;
        end_trick();
    }
}

void Director::event_enemy_killed()
{
    kills_since_level_ = counter((int64_t)kills_since_level_ + 1);
    if (trick_ != TRICK_NONE) trick_stress_acc_ += 0.05f;
}

void Director::event_level_up(int level)
{
    if (trick_ != TRICK_NONE) end_trick();
    level_ = level < 1 ? 1 : level;
    weirdness_ = 1.0f - std::exp(-(float)(level_ - 1) / 22.0f);
    kills_since_level_ = 0;
    /* New level: give the player one guaranteed breath, then ramp again. */
    phase_ = PHASE_CALM;
    phase_time_ = 0.0f;
    phase_length_ = 2.6f;
    calm_bonus_ = 0.08f;
    grace_ = grace_ > 0.8f ? grace_ : 0.8f;
}

/* ----------------------------------------------------------------- fairness */
int Director::lanes_in_window(const ScheduledShot *shots, int n, float t_cross, uint8_t *lane_flags) const
{
    uint8_t seen[LANES];
    for (int i = 0; i < LANES; ++i) seen[i] = 0;
    int count = 0;
    if (shots) {
        for (int i = 0; i < n; ++i) {
            if (shots[i].lane >= LANES) continue;
            float d = shots[i].t_cross - t_cross;
            if (d < -Caps::HOT_WINDOW || d > Caps::HOT_WINDOW) continue;
            if (!seen[shots[i].lane]) {
                seen[shots[i].lane] = 1;
                count++;
            }
        }
    }
    if (lane_flags) {
        for (int i = 0; i < LANES; ++i) lane_flags[i] = seen[i];
    }
    return count;
}

bool Director::may_fire_into(int lane, float t_bottom, float t_player, const ScheduledShot *shots, int n) const
{
    if (lane < 0 || lane >= LANES || !std::isfinite(t_bottom) ||
        !std::isfinite(t_player) || n < 0 || n > Caps::MAX_ENEMY_BULLETS || (!shots && n > 0)) return false;
    if (grace_ > 0.0f) return false;                        /* post-hit grace */
    if (t_player < Caps::MIN_REACTION_TIME) return false;    /* human reaction floor */

    int budget = out_.max_hot_lanes;
    if (budget < 1) budget = 1;
    if (budget > Caps::MAX_HOT_LANES_HARD) budget = Caps::MAX_HOT_LANES_HARD;

    uint8_t flags[LANES];
    int count = lanes_in_window(shots, n, t_bottom, flags);
    if (flags[lane]) {
        /* The corridor is already taking fire: adding to it costs the player
         * no escape route, so it stays allowed while the budget is respected. */
        return count <= budget;
    }
    /* Opening a fresh corridor is allowed only while one stays clear. */
    return (count + 1) <= budget;
}

} /* namespace mss */
