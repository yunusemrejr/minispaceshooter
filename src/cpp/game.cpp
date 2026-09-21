/* game.cpp — the playfield simulation and its renderer. */
#include "game.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio.h"

namespace mss {

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * clampf(t, 0.0f, 1.0f); }
static inline float sq(float v) { return v * v; }

namespace {

struct KindStats {
    int hp;
    float speed;      /* px/s base */
    float fire;       /* seconds between shots at neutral pressure */
    int score;
    float bullet_mul; /* multiplies the director's bullet speed */
    int w, h;
    int patterns; /* bullets per volley */
};

const KindStats KINDS[EK_COUNT] = {
    /* GRUNT */ {1, 20.0f, 1.90f, 100, 0.95f, 9, 8, 1},
    /* WASP  */ {1, 30.0f, 1.50f, 150, 1.10f, 9, 8, 1},
    /* BRUTE */ {3, 12.0f, 2.60f, 300, 0.80f, 13, 11, 3},
    /* GHOST */ {2, 24.0f, 1.70f, 250, 1.00f, 9, 9, 2},
};

const char *KIND_NAMES[EK_COUNT] = {"DRONE", "WASP", "BRUTE", "GHOST"};

} /* namespace */

uint32_t star_color(uint8_t bright)
{
    /* Muted slate tiers on the near-black gradient: visible depth cueing that
     * stays far below ship, bullet and UI brightness. */
    if (bright >= 3) return 0x55638Cu;
    if (bright == 2) return 0x39435Eu;
    return 0x232B45u;
}

/* ------------------------------------------------------------------- reset */
void Game::reset(Rng &rng, int starting_level, int record_score, bool keep_learning)
{
    rng_ = rng;
    starting_level = starting_level < 1 ? 1 : starting_level;
    record_score_ = counter(record_score);
    p_ = Player();
    for (int i = 0; i < MAX_ENEMIES; ++i) enemies_[i] = Enemy();
    for (int i = 0; i < MAX_PBULLETS; ++i) pbullets_[i] = Bullet();
    for (int i = 0; i < MAX_EBULLETS; ++i) ebullets_[i] = Bullet();
    for (int i = 0; i < MAX_PARTICLES; ++i) parts_[i] = Particle();
    for (int i = 0; i < MAX_STARS; ++i) {
        Star &s = stars_[i];
        s.x = rng_.between(0.0f, PLAY_W);
        s.y = rng_.between(0.0f, PLAY_H);
        /* Mostly dim depth pixels; bright foreground stars stay rare. */
        int slot = i % 8;
        int layer = slot == 0 ? 2 : (slot < 4 ? 1 : 0);
        s.speed = 6.0f + (float)layer * 8.0f + rng_.between(0.0f, 3.0f);
        s.bright = (uint8_t)(layer == 2 ? 3 : (layer == 1 ? 2 : 1));
    }
    st_ = RunStats();
    st_.level = starting_level;
    level_target_kills_ = counter(8 + (int64_t)starting_level * 2);
    if (level_target_kills_ > 60) level_target_kills_ = 60;
    next_score_checkpoint_ = counter((int64_t)starting_level * 2000);
    spawn_timer_ = 1.4f;
    wave_index_ = 0;
    kills_for_level_ = 0;
    last_trick_ = TRICK_NONE;
    hot_count_ = 0;
    sched_count_ = 0;
    std::memset(lane_hot_, 0, sizeof(lane_hot_));
    std::memset(lane_hot_prev_, 0, sizeof(lane_hot_prev_));
    banner_t_ = 0.0f;
    medal_banner_t_ = 0.0f;
    last_medal_ = MEDAL_NONE;
    shake_ = 0.0f;
    hit_flash_ = 0.0f;
    volley_timer_ = 0.0f;
    time_since_damage_ = 99.0f;
    near_miss_accum_ = 0.0f;
    near_miss_rate_ = 0.0f;
    shot_accum_ = 0.0f;
    hit_accum_ = 0.0f;
    fire_rate_window_ = 0.0f;
    fire_rate_ = 0.0f;
    shots_1s_ = 0;
    dodge_window_ = 0.0f;
    last_dir_ = 0.0f;
    dir_changes_ = 0.0f;
    decay_t_ = 0.0f;
    over_ = false;
    dir_.reset(rng_, starting_level, keep_learning);
    reset_allies(keep_learning);
    std::snprintf(banner_, sizeof(banner_), "LEVEL %d", starting_level);
    banner_t_ = 2.0f;
}

/* ------------------------------------------------------------------ helpers */
int Game::lane_of(float x) const
{
    int l = (int)(x / (PLAY_W / (float)LANES));
    if (l < 0) l = 0;
    if (l >= LANES) l = LANES - 1;
    return l;
}

int Game::enemies_alive() const
{
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) n += enemies_[i].alive ? 1 : 0;
    return n;
}

int Game::enemy_bullets() const
{
    int n = 0;
    for (int i = 0; i < MAX_EBULLETS; ++i) n += ebullets_[i].alive ? 1 : 0;
    return n;
}

int Game::enemy_snapshot(Enemy *out, int capacity) const
{
    if (!out || capacity <= 0) return 0;
    int count = 0;
    for (const Enemy &e : enemies_) {
        if (e.alive && count < capacity) out[count++] = e;
    }
    return count;
}

int Game::scheduled_shots(ScheduledShot *out, int max_shots) const
{
    if (!out || max_shots <= 0) return 0;
    int n = sched_count_ < max_shots ? sched_count_ : max_shots;
    for (int i = 0; i < n; ++i) out[i] = sched_[i];
    return n;
}

float Game::lane_center(int lane) const
{
    float w = PLAY_W / (float)LANES;
    return (float)lane * w + w * 0.5f;
}

float Game::enemy_speed(int kind) const { return KINDS[kind].speed * dir_.output().enemy_speed_mul; }

float Game::enemy_fire_interval(int kind) const
{
    /* The director's global cadence scales every ship; the hard floor keeps
     * any single enemy from becoming a bullet hose. */
    float global = dir_.output().fire_interval / 1.75f;
    float iv = KINDS[kind].fire * global;
    if (iv < Caps::MIN_FIRE_INTERVAL) iv = Caps::MIN_FIRE_INTERVAL;
    return iv;
}

void Game::add_particle(float x, float y, float vx, float vy, float life, int size, uint32_t col, uint8_t kind)
{
    for (int i = 0; i < MAX_PARTICLES; ++i) {
        Particle &pt = parts_[i];
        if (pt.alive) continue;
        pt.alive = true;
        pt.x = x;
        pt.y = y;
        pt.vx = vx;
        pt.vy = vy;
        pt.life = life;
        pt.max_life = life;
        pt.size = size;
        pt.col = col;
        pt.kind = kind;
        return;
    }
}

void Game::explode(float x, float y, uint32_t col, int scale)
{
    int n = 5 + scale * 4;
    if (n > 16) n = 16;
    for (int i = 0; i < n; ++i) {
        float a = rng_.uni() * 6.28318f;
        float sp = rng_.between(18.0f, 60.0f) * (0.6f + 0.4f * (float)scale);
        add_particle(x, y, std::cos(a) * sp, std::sin(a) * sp, rng_.between(0.22f, 0.62f),
                     scale > 1 ? 2 : 1, (i % 3 == 0) ? art::color('G') : col, 0);
    }
    add_particle(x, y, 0.0f, 0.0f, 0.13f, scale > 1 ? 2 : 1, art::color('G'), 2);
    if (scale > 1) add_particle(x, y, 0.0f, 0.0f, 0.20f, 2, art::color('o'), 2);
}

/* ------------------------------------------------------------------ director */
/* Rebuilds the inbound-shot schedule and the per-frame corridor flags.
 *
 * Every descending bullet contributes the corridor it will sweep where it
 * leaves the playfield and how long that takes, both measured on the bullet's
 * straight trajectory with the time NOT clamped at zero.  That matters: a
 * bullet sitting in the bottom despawn strip would otherwise report t = 0 and a
 * lane equal to wherever it currently is, letting it slide into a fresh
 * corridor with no veto and break the escape-corridor guarantee.  With the raw
 * trajectory both the lane and the relative arrival times stay exactly
 * invariant for the whole life of a bullet, which is what makes the director's
 * window rule provable rather than approximate. */
void Game::compute_lanes()
{
    sched_count_ = 0;
    std::memset(lane_hot_, 0, sizeof(lane_hot_));
    hot_count_ = 0;
    for (int i = 0; i < MAX_EBULLETS; ++i) {
        const Bullet &b = ebullets_[i];
        if (!b.alive || b.vy < 1.0f) continue;
        float t = (PLAY_H - b.y) / b.vy; /* negative once past the bottom */
        ScheduledShot s;
        s.lane = (uint8_t)lane_of(clampf(b.x + b.vx * t, 0.0f, PLAY_W - 1.0f));
        s.t_cross = t;
        if (sched_count_ < MAX_EBULLETS) sched_[sched_count_++] = s;
        /* The overlay/diagnostic view: corridors that will take fire inside the
         * next HOT_WINDOW, and only those.  The lower bound matters: a bullet
         * that already passed the playfield bottom is behind the player and
         * must not keep a corridor marked, or the marked set could span wider
         * than HOT_WINDOW and break the guarantee it is supposed to express. */
        if (t >= 0.0f && t <= Caps::HOT_WINDOW) lane_hot_[s.lane] = 1;
    }
    for (int l = 0; l < LANES; ++l) hot_count_ += lane_hot_[l] ? 1 : 0;
}

void Game::feed_director(float dt)
{
    time_since_damage_ = clampf(time_since_damage_ + dt, 0.0f, 99.0f);

    /* near-miss accumulation was gathered by the bullet update */
    near_miss_rate_ += (near_miss_accum_ - near_miss_rate_) * clampf(dt * 0.5f, 0.0f, 1.0f);
    near_miss_accum_ *= 0.9f;

    fire_rate_window_ += dt;
    if (fire_rate_window_ >= 1.0f) {
        fire_rate_window_ -= 1.0f;
        fire_rate_ = (float)shots_1s_;
        shots_1s_ = 0;
    }
    /* rolling accuracy: hits over shots, both decayed */
    shot_accum_ *= (1.0f - dt * 0.12f);
    hit_accum_ *= (1.0f - dt * 0.12f);

    float dodge_rate = dir_changes_;
    dir_changes_ *= (1.0f - dt * 0.25f);

    DirectorInput di;
    di.dt = dt;
    di.player_x = p_.x / PLAY_W;
    di.player_vx = p_.vx / PLAY_W;
    di.player_fire_rate = fire_rate_;
    di.player_accuracy = shot_accum_ > 1.0f ? clampf(hit_accum_ / shot_accum_, 0.0f, 1.0f) : 0.35f;
    di.player_dodge_rate = dodge_rate;
    di.near_miss_rate = near_miss_rate_;
    int hits_recent = 0;
    (void)hits_recent;
    di.hits_taken_recent = 3 - p_.hp;
    di.time_since_damage = time_since_damage_;
    int alive = 0, bullets = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) alive += enemies_[i].alive ? 1 : 0;
    for (int i = 0; i < MAX_EBULLETS; ++i) bullets += ebullets_[i].alive ? 1 : 0;
    di.enemies_alive = alive;
    di.enemy_bullets = bullets;
    di.lanes_hot = hot_count_;
    di.level = st_.level;
    di.score = (float)st_.score;
    di.player_hp = p_.hp;
    dir_.observe(di);

    if (dir_.output().trick != last_trick_) {
        if (dir_.output().trick != TRICK_NONE) {
            std::snprintf(banner_, sizeof(banner_), "%s", trick_name(dir_.output().trick));
            banner_t_ = 1.6f;
            aud_play(SFX_TRICK, 1.0f);
        }
        last_trick_ = dir_.output().trick;
    }
}

/* ----------------------------------------------------------------- spawning */
int Game::spawn_enemy(EnemyKind kind, float x, float y, float vx, float vy)
{
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        Enemy &e = enemies_[i];
        if (e.alive) continue;
        e = Enemy();
        e.alive = true;
        e.kind = (int)kind;
        e.x = clampf(x, (float)KINDS[kind].w * 0.5f, PLAY_W - (float)KINDS[kind].w * 0.5f);
        e.y = y;
        e.vx = vx;
        e.vy = vy;
        e.base_speed = enemy_speed((int)kind);
        e.max_hp = KINDS[kind].hp + (st_.level >= 12 && kind == EK_BRUTE ? 1 : 0);
        e.hp = e.max_hp;
        e.fire_cd = rng_.between(0.6f, 1.6f) + enemy_fire_interval((int)kind) * 0.5f;
        e.bob_phase = rng_.between(0.0f, 6.28f);
        e.origin_x = e.x;
        e.action = ACT_ADVANCE;
        e.policy_valid = false;
        e.decision_t = rng_.between(0.2f, 0.6f);
        e.reward_acc = 0.02f;
        e.trick_t = rng_.between(0.3f, 1.2f);
        e.lane_drift = rng_.chance(0.5f) ? -1.0f : 1.0f;
        e.entered = 0.0f;
        for (int k = 0; k < 8; ++k) e.features[k] = 0.0f;
        return i;
    }
    return -1;
}

void Game::spawn_wave(int trick)
{
    int alive = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) alive += enemies_[i].alive ? 1 : 0;
    int room = dir_.output().max_enemies - alive;
    if (room <= 0) return;

    int budget = 1 + st_.level / 4 + rng_.range(2);
    if (budget > room) budget = room;
    if (budget > 4) budget = 4;
    if (trick == TRICK_SIEGE && budget < 3) budget = 3;
    if (budget > room) budget = room;

    /* composition: better kinds appear as levels rise, and the current trick
     * nudges the mix (siege wants brutes, decoy wants one fast rusher). */
    int weights[EK_COUNT] = {0, 0, 0, 0};
    weights[EK_GRUNT] = 5;
    weights[EK_WASP] = st_.level >= 3 ? 3 : 0;
    weights[EK_BRUTE] = st_.level >= 5 ? 2 : 0;
    weights[EK_GHOST] = st_.level >= 8 ? 2 : 0;
    if (trick == TRICK_SIEGE) {
        weights[EK_BRUTE] += 6;
        weights[EK_GRUNT] += 2;
    }
    if (trick == TRICK_DECOY) {
        weights[EK_WASP] += 5;
        weights[EK_GHOST] += 2;
    }
    if (trick == TRICK_LANE_DENY) weights[EK_WASP] += 3;
    int total_w = 0;
    for (int i = 0; i < EK_COUNT; ++i) total_w += weights[i];

    int formation = rng_.range(5);
    if (trick == TRICK_PINCER || trick == TRICK_SPLIT) formation = 2;
    if (trick == TRICK_SIEGE) formation = 0;

    for (int i = 0; i < budget; ++i) {
        int pick = rng_.range(total_w > 0 ? total_w : 1);
        int kind = EK_GRUNT;
        for (int k = 0; k < EK_COUNT; ++k) {
            if (pick < weights[k]) {
                kind = k;
                break;
            }
            pick -= weights[k];
        }
        float x = PLAY_W * 0.5f;
        float y = -14.0f - (float)i * 4.0f;
        float vx = 0.0f;
        switch (formation) {
        case 0: /* line */
            x = 44.0f + (float)i * 74.0f + rng_.between(-6.0f, 6.0f);
            y = HUD_H + 10.0f - (float)(i % 2) * 10.0f;
            break;
        case 1: /* v */
            x = PLAY_W * 0.5f + ((float)i - (float)budget * 0.5f) * 46.0f;
            y = HUD_H + 8.0f + std::fabs((float)i - (float)budget * 0.5f) * 12.0f;
            break;
        case 2: /* two columns, closing in from the edges (pincer / split) */
            if (i % 2 == 0) {
                x = 26.0f + (float)(i / 2) * 22.0f;
                vx = 8.0f;
            } else {
                x = PLAY_W - 26.0f - (float)(i / 2) * 22.0f;
                vx = -8.0f;
            }
            y = HUD_H + 12.0f + (float)(i / 2) * 12.0f;
            break;
        case 3: /* scatter */
            x = rng_.between(30.0f, PLAY_W - 30.0f);
            y = HUD_H + rng_.between(8.0f, 40.0f);
            break;
        default: /* echelon from one side */
            x = 40.0f + (float)i * 30.0f * (rng_.chance(0.5f) ? 1.0f : -1.0f) + 60.0f;
            y = HUD_H + 12.0f + (float)i * 9.0f;
            break;
        }
        int idx = spawn_enemy((EnemyKind)kind, x, y, vx, 0.0f);
        if (idx < 0) break;
    }
    wave_index_ = counter((int64_t)wave_index_ + 1);
}

void Game::spawn_player_bullet()
{
    for (int i = 0; i < MAX_PBULLETS; ++i) {
        Bullet &b = pbullets_[i];
        if (b.alive) continue;
        b.alive = true;
        b.kind = 0;
        b.policy_valid = false;
        b.x = p_.x;
        b.y = p_.y - 6.0f;
        b.vx = 0.0f;
        b.vy = -200.0f;
        b.life = 2.0f;
        shots_1s_++;
        st_.shots = counter((int64_t)st_.shots + 1);
        shot_accum_ += 1.0f;
        aud_play(SFX_LASER, rng_.between(0.94f, 1.07f));
        return;
    }
}

/* ------------------------------------------------------------------- firing */
bool Game::enemy_fire(int idx, float aim_x, int patterns)
{
    Enemy &e = enemies_[idx];
    if (e.shot_guard > 0.0f) return false;
    const KindStats &k = KINDS[e.kind];
    float speed = clampf(dir_.output().bullet_speed * k.bullet_mul, 24.0f, Caps::ENEMY_BULLET_SPEED);
    float y = e.y + (float)k.h * 0.5f;
    float t_player = (p_.y - y) / speed;
    if (t_player <= 0.0f) return false;

    int spawned = 0;
    int live_bullets = 0;
    for (int i = 0; i < MAX_EBULLETS; ++i) live_bullets += ebullets_[i].alive ? 1 : 0;

    for (int p = 0; p < patterns; ++p) {
        if (live_bullets >= dir_.output().max_bullets) break;
        float spread = (patterns == 1) ? 0.0f : ((float)p - (float)(patterns - 1) * 0.5f) * 26.0f;
        float tx = clampf(aim_x + spread, 6.0f, PLAY_W - 6.0f);
        /* Cap the vector magnitude, not just its vertical component. Limit
         * shallow angles so shots remain readable and continue downfield. */
        float vx = clampf((tx - e.x) / t_player, -speed * 1.25f, speed * 1.25f);
        float scale = speed / std::sqrt(vx * vx + speed * speed);
        vx *= scale;
        float vy = speed * scale;
        float t_bottom = (PLAY_H - y) / vy;
        float t_contact = (p_.y - 5.0f - y) / vy;

        /* The fairness gate: refuse the shot if it would close the last open
         * corridor, arrive with no reaction time, or land during grace. */
        ScheduledShot cand;
        cand.lane = (uint8_t)lane_of(clampf(e.x + vx * t_bottom, 0.0f, PLAY_W - 1.0f));
        cand.t_cross = t_bottom;
        if (!dir_.may_fire_into(cand.lane, t_bottom, t_contact, sched_, sched_count_)) continue;

        bool placed = false;
        for (int i = 0; i < MAX_EBULLETS; ++i) {
            Bullet &b = ebullets_[i];
            if (b.alive) continue;
            b.alive = true;
            b.kind = (patterns > 1) ? 2 : 1;
            b.policy_valid = e.policy_valid;
            b.action = e.action;
            for (int j = 0; j < 8; ++j) b.features[j] = e.features[j];
            b.x = e.x;
            b.y = y;
            b.vx = vx;
            b.vy = vy;
            b.life = t_bottom + 0.5f;
            live_bullets++;
            spawned++;
            placed = true;
            break;
        }
        /* Later shots in this same frame must see this one. */
        if (placed && sched_count_ < MAX_EBULLETS) sched_[sched_count_++] = cand;
    }
    if (spawned > 0) {
        e.shot_guard = Caps::MIN_FIRE_INTERVAL;
        aud_play(SFX_ENEMY_SHOT, rng_.between(0.9f, 1.1f));
        e.reward_acc += 0.05f;
    }
    return spawned > 0;
}

/* ------------------------------------------------------------------ policy */
void Game::finish_policy(Enemy &e, float reward)
{
    if (e.policy_valid) {
        dir_.policy_learn(e.features, e.action, dir_.policy_advantage(e.reward_acc + reward), 0.010f);
    }
    e.policy_valid = false;
    e.reward_acc = 0.0f;
}

void Game::policy_decide(Enemy &e)
{
    /* Train on the reward collected since the previous decision, then choose
     * a new action from the shared softmax policy. */
    finish_policy(e, 0.0f);
    e.reward_acc = 0.02f;

    float f[8];
    f[0] = clampf((p_.x - e.x) / (PLAY_W * 0.5f), -1.5f, 1.5f);
    f[1] = clampf((p_.y - e.y) / (PLAY_H * 0.5f), -1.5f, 1.5f);
    f[2] = clampf(p_.vx / 90.0f, -1.5f, 1.5f);
    f[3] = clampf(e.y / PLAY_H, 0.0f, 1.2f);
    f[4] = dir_.output().aggression;
    f[5] = clampf((float)hot_count_ / (float)LANES, 0.0f, 1.0f);
    f[6] = dir_.intent_bias();
    f[7] = e.max_hp > 0 ? (float)e.hp / (float)e.max_hp : 1.0f;

    for (int i = 0; i < 8; ++i) e.features[i] = f[i];
    const float *out = dir_.policy_forward(f);
    float probs[ACT_COUNT];
    dir_.policy_probs(probs, ACT_COUNT);
    float r = rng_.uni();
    int action = ACT_COUNT - 1;
    float acc = 0.0f;
    for (int i = 0; i < ACT_COUNT; ++i) {
        acc += probs[i];
        if (r <= acc) {
            action = i;
            break;
        }
    }
    (void)out;
    e.action = action;
    e.policy_valid = true;
    e.decision_t = rng_.between(0.45f, 0.75f);
}

void Game::policy_apply(Enemy &e, float dt)
{
    e.decision_t -= dt;
    if (e.decision_t <= 0.0f) policy_decide(e);

    float speed = e.base_speed;
    float vy = speed * 0.55f; /* a gentle baseline descent keeps waves moving */
    float vx = 0.0f;
    switch (e.action) {
    case ACT_ADVANCE: vy = speed * 1.15f; break;
    case ACT_STRAFE_L: vx = -speed * 0.9f; vy = speed * 0.35f; break;
    case ACT_STRAFE_R: vx = speed * 0.9f; vy = speed * 0.35f; break;
    case ACT_RETREAT: vy = -speed * 0.5f; break;
    default: break;
    }
    e.vx = lerpf(e.vx, vx, clampf(dt * 3.0f, 0.0f, 1.0f));
    e.vy = lerpf(e.vy, vy, clampf(dt * 3.0f, 0.0f, 1.0f));
    /* Hard caps: no ship may ever exceed the human-playable speed limits. */
    float cap_h = Caps::ENEMY_STRAFE_SPEED;
    float cap_v = Caps::ENEMY_DIVE_SPEED;
    e.vx = clampf(e.vx, -cap_h, cap_h);
    e.vy = clampf(e.vy, -cap_v, cap_v);
}

/* ------------------------------------------------------------ trick behaviour */
void Game::trick_behaviour(Enemy &e, float dt)
{
    const DirectorOutput &o = dir_.output();
    if (o.trick == TRICK_NONE) return;
    float inten = o.trick_intensity;
    float strafe = Caps::ENEMY_STRAFE_SPEED * (0.5f + 0.5f * inten);

    switch (o.trick) {
    case TRICK_FEINT_DIVE: {
        e.trick_t -= dt;
        if (e.trick_t <= 0.0f) {
            e.diving = !e.diving;
            e.trick_t = e.diving ? 0.85f : 0.75f;
        }
        if (e.diving) {
            e.vy = Caps::ENEMY_DIVE_SPEED * 0.85f * inten;
            e.vx = clampf((p_.x - e.x) * 1.2f, -strafe, strafe);
        } else {
            e.vy = -e.base_speed * 0.6f;
        }
        break;
    }
    case TRICK_PINCER: {
        float side = (e.origin_x < PLAY_W * 0.5f) ? -1.0f : 1.0f;
        float target = clampf(p_.x + side * 58.0f, 16.0f, PLAY_W - 16.0f);
        e.vx = clampf((target - e.x) * 1.4f, -strafe, strafe);
        e.vy = e.base_speed * 0.30f;
        break;
    }
    case TRICK_LANE_DENY: {
        /* Hold a line near the top and plug corridors around the player. */
        float hold_y = HUD_H + 26.0f + (e.origin_x / PLAY_W) * 14.0f;
        e.vy = clampf((hold_y - e.y) * 1.3f, -cap_v_dive(), cap_v_dive());
        int pl = lane_of(p_.x);
        int target_lane = (pl + ((int)(e.origin_x) % 2 == 0 ? 1 : -1));
        if (target_lane < 0) target_lane = 0;
        if (target_lane >= LANES) target_lane = LANES - 1;
        e.vx = clampf((lane_center(target_lane) - e.x) * 1.2f, -strafe, strafe);
        break;
    }
    case TRICK_AMBUSH: {
        e.vy = e.base_speed * 0.25f;
        if (o.trick_progress > 0.40f) {
            /* everyone accelerates their reload together: one shared volley */
            e.fire_cd -= dt * 2.5f * (1.0f + inten);
        } else {
            e.fire_cd += dt; /* frozen while the silence lasts */
        }
        break;
    }
    case TRICK_DECOY: {
        if (e.kind == EK_WASP) {
            e.vy = Caps::ENEMY_DIVE_SPEED * 0.9f;
            e.vx = clampf((p_.x - e.x) * 0.8f, -strafe, strafe);
        } else {
            e.vy = e.base_speed * 0.18f;
        }
        break;
    }
    case TRICK_RHYTHM_BREAK: {
        e.vy = e.base_speed * 0.4f;
        e.vx = clampf((p_.x - e.x) * 0.5f, -strafe * 0.7f, strafe * 0.7f);
        break;
    }
    case TRICK_SIEGE: {
        e.vy = e.base_speed * 0.45f;
        e.vx = clampf((lane_center(lane_of(e.origin_x)) - e.x) * 0.8f, -strafe * 0.5f, strafe * 0.5f);
        break;
    }
    case TRICK_SPLIT: {
        if (o.trick_progress < 0.55f) {
            float edge = (e.lane_drift < 0.0f) ? 26.0f : PLAY_W - 26.0f;
            e.vx = clampf((edge - e.x) * 1.5f, -strafe, strafe);
            e.vy = e.base_speed * 0.35f;
        } else {
            e.vx = clampf((p_.x - e.x) * 1.1f, -strafe, strafe);
            e.vy = e.base_speed * 0.7f;
        }
        break;
    }
    default:
        break;
    }
}

float Game::cap_v_dive() const { return Caps::ENEMY_DIVE_SPEED; }

/* ------------------------------------------------------------------ updates */
void Game::update_enemies(float dt)
{
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        Enemy &e = enemies_[i];
        if (!e.alive) continue;
        e.entered += dt;
        e.shot_guard = clampf(e.shot_guard - dt, 0.0f, Caps::MIN_FIRE_INTERVAL);
        if (dir_.output().trick == TRICK_NONE || e.entered < 0.8f) {
            policy_apply(e, dt);
        } else {
            finish_policy(e, 0.0f);
            trick_behaviour(e, dt);
        }
        e.reward_acc += dt * 0.02f; /* small survival reward */

        e.x += e.vx * dt;
        e.y += e.vy * dt;
        float hw = (float)KINDS[e.kind].w * 0.5f;
        if (e.x < hw) {
            e.x = hw;
            e.vx = std::fabs(e.vx);
        }
        if (e.x > PLAY_W - hw) {
            e.x = PLAY_W - hw;
            e.vx = -std::fabs(e.vx);
        }
        /* stay inside the playfield vertically: the director may pull ships up */
        float top = HUD_H + 4.0f;
        if (e.y < top) {
            e.y = top;
            if (e.vy < 0.0f) e.vy = 0.0f;
        }

        /* A ship that drifts off the bottom is a wasted spawn: recycle it. */
        if (e.y > PLAY_H + 16.0f || e.entered > 40.0f) {
            finish_policy(e, -0.4f);
            e.alive = false;
            continue;
        }

        /* Firing: aim where the intent model says the player is going. */
        e.fire_cd -= dt;
        if (e.fire_cd <= 0.0f && e.shot_guard <= 0.0f) {
            float lead = dir_.intent_bias() * 38.0f * (0.4f + 0.6f * dir_.output().aggression);
            float aim = clampf(p_.x + lead, 4.0f, PLAY_W - 4.0f);
            if (dir_.output().trick == TRICK_LANE_DENY) {
                int pl = lane_of(p_.x);
                int alt = (pl + (i % 2 == 0 ? 1 : -1));
                if (alt < 0) alt = 0;
                if (alt >= LANES) alt = LANES - 1;
                aim = lane_center(alt);
            }
            int patterns = KINDS[e.kind].patterns;
            if (dir_.output().trick == TRICK_SIEGE && e.kind == EK_BRUTE) patterns = 3;
            bool fired = enemy_fire(i, aim, patterns);
            float iv = enemy_fire_interval(e.kind);
            /* rhythm break alternates long and short reloads, within the cap */
            if (dir_.output().trick == TRICK_RHYTHM_BREAK) {
                e.diving = !e.diving;
                iv *= e.diving ? 0.62f : 1.85f;
                if (iv < Caps::MIN_FIRE_INTERVAL) iv = Caps::MIN_FIRE_INTERVAL;
            }
            e.fire_cd = fired ? iv * rng_.between(0.92f, 1.12f) : 0.12f;
            if (fired && e.fire_cd < Caps::MIN_FIRE_INTERVAL) e.fire_cd = Caps::MIN_FIRE_INTERVAL;
        }

        /* Escorts can absorb a ram before it reaches the player. */
        for (Ally &a : allies_) {
            if (!a.alive || a.hurt > 0.0f) continue;
            const Sprite &sprite = art::ally[a.kind];
            if (std::fabs(e.x - a.x) < hw + (float)sprite.w * 0.5f &&
                std::fabs(e.y - a.y) < (float)(KINDS[e.kind].h + sprite.h) * 0.5f) {
                damage_ally(a, e.kind == EK_BRUTE ? 3 : 2, true);
                kill_enemy(i, true);
                st_.ally_kills = counter((int64_t)st_.ally_kills + 1);
                break;
            }
        }
        if (!e.alive) continue;
        /* Ramming costs the player but also destroys the ship. */
        if (p_.invuln <= 0.0f && std::fabs(e.x - p_.x) < (hw + 4.0f) &&
            std::fabs(e.y - p_.y) < ((float)KINDS[e.kind].h * 0.5f + 4.0f)) {
            kill_enemy(i, false);
            damage_player(1.0f);
        }
    }
}

void Game::update_player_bullets(float dt)
{
    for (int i = 0; i < MAX_PBULLETS; ++i) {
        Bullet &b = pbullets_[i];
        if (!b.alive) continue;
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        b.life -= dt;
        if (b.y < HUD_H - 4.0f || b.life <= 0.0f) {
            b.alive = false;
            continue;
        }
        for (int k = 0; k < MAX_ENEMIES; ++k) {
            Enemy &e = enemies_[k];
            if (!e.alive) continue;
            float hw = (float)KINDS[e.kind].w * 0.5f + 1.0f;
            float hh = (float)KINDS[e.kind].h * 0.5f + 1.0f;
            if (std::fabs(b.x - e.x) < hw && std::fabs(b.y - e.y) < hh) {
                b.alive = false;
                e.hp--;
                hit_accum_ += 1.0f;
                st_.hits = counter((int64_t)st_.hits + 1);
                e.reward_acc -= 0.35f;
                if (e.hp <= 0) {
                    kill_enemy(k, true);
                } else {
                    aud_play(SFX_HIT_ENEMY, rng_.between(0.95f, 1.15f));
                    explode(b.x, b.y, art::color('G'), 1);
                }
                break;
            }
        }
    }
}

void Game::update_enemy_bullets(float dt)
{
    for (int i = 0; i < MAX_EBULLETS; ++i) {
        Bullet &b = ebullets_[i];
        if (!b.alive) continue;
        float prev_y = b.y;
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        b.life -= dt;
        if (b.y > PLAY_H + 6.0f || b.x < -8.0f || b.x > PLAY_W + 8.0f || b.life <= 0.0f) {
            b.alive = false;
            continue;
        }
        if (intercept_bullet(b)) continue;
        /* near miss: crossed the player's row with no contact */
        if (prev_y <= p_.y && b.y > p_.y) {
            float d = std::fabs(b.x - p_.x);
            if (d < 16.0f) near_miss_accum_ += 1.0f;
        }
        if (p_.invuln > 0.0f) continue;
        /* slightly forgiving hitbox (1px inset) so grazes feel fair */
        if (std::fabs(b.x - p_.x) < 4.0f && std::fabs(b.y - p_.y) < 5.0f) {
            b.alive = false;
            if (b.policy_valid) {
                dir_.policy_learn(b.features, b.action, dir_.policy_advantage(1.0f), 0.010f);
            }
            damage_player(1.0f);
        }
    }
}

void Game::update_particles(float dt)
{
    for (int i = 0; i < MAX_PARTICLES; ++i) {
        Particle &pt = parts_[i];
        if (!pt.alive) continue;
        pt.life -= dt;
        if (pt.life <= 0.0f) {
            pt.alive = false;
            continue;
        }
        pt.x += pt.vx * dt;
        pt.y += pt.vy * dt;
        pt.vx *= (1.0f - dt * 1.6f);
        pt.vy *= (1.0f - dt * 1.6f);
    }
}

void Game::update_stars(float dt)
{
    for (int i = 0; i < MAX_STARS; ++i) {
        Star &s = stars_[i];
        s.y += s.speed * dt * (1.0f + dir_.output().pressure * 0.5f);
        if (s.y > PLAY_H) {
            s.y -= PLAY_H;
            s.x = rng_.between(0.0f, PLAY_W);
        }
    }
}

void Game::kill_enemy(int idx, bool by_player)
{
    Enemy &e = enemies_[idx];
    uint32_t col = e.kind == EK_BRUTE ? art::color('o') : (e.kind == EK_GHOST ? art::color('v') : art::color('e'));
    explode(e.x, e.y, col, e.kind == EK_BRUTE ? 2 : 1);
    e.alive = false;

    /* Terminal policy reward: dying is the strongest negative signal. */
    finish_policy(e, -1.0f);

    if (by_player) {
        st_.kills = counter((int64_t)st_.kills + 1);
        kills_for_level_ = counter((int64_t)kills_for_level_ + 1);
        award_points(KINDS[e.kind].score + (int64_t)st_.level * 5);
        dir_.event_enemy_killed();
        shake_ = e.kind == EK_BRUTE ? 2.0f : 1.0f;
        if (e.kind == EK_BRUTE) aud_play(SFX_EXPLODE_BIG, rng_.between(0.95f, 1.05f));
        else aud_play(SFX_EXPLODE, rng_.between(0.9f, 1.2f));
    }
}

void Game::damage_player(float amount)
{
    if (p_.invuln > 0.0f || over_) return;
    (void)amount;
    p_.hp--;
    p_.invuln = 1.8f;
    p_.hurt_flash = 0.35f;
    shake_ = 3.0f;
    hit_flash_ = 0.25f;
    time_since_damage_ = 0.0f;
    dir_.event_player_damage();
    for (Ally &a : allies_) if (a.alive && a.policy_valid) a.reward -= 0.5f;
    explode(p_.x, p_.y, art::color('C'), 1);
    aud_play(SFX_PLAYER_HIT, 1.0f);

    /* Mercy: clear the nearby curtain of bullets so the respawn is survivable. */
    for (int i = 0; i < MAX_EBULLETS; ++i) {
        Bullet &b = ebullets_[i];
        if (!b.alive) continue;
        if (sq(b.x - p_.x) + sq(b.y - p_.y) < 90.0f * 90.0f) {
            b.alive = false;
            add_particle(b.x, b.y, 0.0f, -18.0f, 0.25f, 1, art::color('W'), 0);
        }
    }
    if (p_.hp <= 0) {
        over_ = true;
        st_.deaths = counter((int64_t)st_.deaths + 1);
        dir_.event_player_death();
        for (Ally &a : allies_) if (a.alive) finish_ally_policy(a, -1.0f);
        explode(p_.x, p_.y, art::color('C'), 3);
        explode(p_.x, p_.y, art::color('o'), 2);
        aud_play(SFX_EXPLODE_BIG, 0.85f);
        aud_play(SFX_GAMEOVER, 1.0f);
    }
}

void Game::check_level_progress()
{
    if (over_ || st_.level == INT_MAX) return;
    if ((st_.score < INT_MAX && st_.score >= next_score_checkpoint_) || kills_for_level_ >= level_target_kills_) {
        st_.level++;
        award_points(120 + (int64_t)st_.level * 30);
        kills_for_level_ = 0;
        level_target_kills_ = counter(8 + (int64_t)st_.level * 2);
        if (level_target_kills_ > 60) level_target_kills_ = 60;
        next_score_checkpoint_ = counter((int64_t)next_score_checkpoint_ + 2500 + (int64_t)st_.level * 250);
        dir_.event_level_up(st_.level);
        std::snprintf(banner_, sizeof(banner_), "LEVEL %d", st_.level);
        if (st_.level % 5 == 0 && p_.hp < 3) {
            ++p_.hp;
            std::snprintf(banner_, sizeof(banner_), "LEVEL %d  HULL +1", st_.level);
        }
        banner_t_ = 2.0f;
        aud_play(SFX_LEVELUP, 1.0f);
    }
}

void Game::check_medals()
{
    int best = ladder_best();
    int t[MEDAL_COUNT];
    medal_thresholds(best, t);
    for (int i = 0; i < MEDAL_COUNT; ++i) {
        if (t[i] <= 0) continue;
        if (st_.score < t[i]) continue;
        int bit = 1 << i;
        if (st_.medal_mask & bit) continue;
        st_.medal_mask |= bit;
        last_medal_ = i;
        medal_banner_t_ = 2.6f;
        std::snprintf(banner_, sizeof(banner_), "%s  %d", medal_name(i), t[i]);
        banner_t_ = 2.6f;
        aud_play(SFX_MEDAL, 1.0f + 0.05f * (float)i);
    }
}

/* ---------------------------------------------------------------- the frame */
void Game::update(const GameInput &in, float dt)
{
    if (over_ || !std::isfinite(dt) || dt <= 0.0f) return;
    dt = clampf(dt, 0.0f, 0.1f);
    GameInput input = in;
    input.mx = std::isfinite(in.mx) ? clampf(in.mx, -1.0f, 1.0f) : 0.0f;
    input.my = std::isfinite(in.my) ? clampf(in.my, -1.0f, 1.0f) : 0.0f;
    float magnitude = std::sqrt(input.mx * input.mx + input.my * input.my);
    if (magnitude > 1.0f) { input.mx /= magnitude; input.my /= magnitude; }
    int steps = (int)std::ceil(dt / (1.0f / 60.0f));
    for (int i = 0; i < steps && !over_; ++i) update_step(input, dt / (float)steps);
}

void Game::update_step(const GameInput &in, float dt)
{
    st_.seconds += (double)dt;
    if (st_.seconds > (double)INT_MAX) st_.seconds = (double)INT_MAX;

    update_stars(dt);
    compute_lanes();
    feed_director(dt);

    /* ---- player ---- */
    if (p_.invuln > 0.0f) p_.invuln -= dt;
    float ax = in.mx;
    float ay = in.my;
    float speed = 74.0f;
    float target_vx = clampf(ax, -1.0f, 1.0f) * speed;
    float target_vy = clampf(ay, -1.0f, 1.0f) * speed * 0.9f;
    p_.vx = lerpf(p_.vx, target_vx, clampf(dt * 14.0f, 0.0f, 1.0f));
    p_.vy = lerpf(p_.vy, target_vy, clampf(dt * 14.0f, 0.0f, 1.0f));
    p_.x += p_.vx * dt;
    p_.y += p_.vy * dt;
    p_.x = clampf(p_.x, 6.0f, PLAY_W - 6.0f);
    p_.y = clampf(p_.y, HUD_H + 12.0f, FLEET_TOP - 8.0f);
    p_.engine_phase += dt * 18.0f;
    if (p_.engine_phase >= 6.2831853f) p_.engine_phase -= 6.2831853f;
    if (p_.hurt_flash > 0.0f) p_.hurt_flash -= dt;
    if (in.mx > 0.35f || in.mx < -0.35f) {
        float dir = in.mx > 0.0f ? 1.0f : -1.0f;
        if (dir != last_dir_) {
            dir_changes_ += 1.0f;
            last_dir_ = dir;
        }
    }

    p_.fire_cd -= dt;
    if (in.fire && p_.fire_cd <= 0.0f) {
        p_.fire_cd = 0.255f;
        spawn_player_bullet();
    }

    /* ---- spawning ---- */
    spawn_timer_ -= dt;
    if (spawn_timer_ <= 0.0f) {
        int alive = 0;
        for (int i = 0; i < MAX_ENEMIES; ++i) alive += enemies_[i].alive ? 1 : 0;
        if (alive == 0 || alive < dir_.output().max_enemies) {
            spawn_wave(dir_.output().trick);
            spawn_timer_ = dir_.output().spawn_interval * rng_.between(0.85f, 1.15f);
            if (spawn_timer_ < Caps::SPAWN_MIN_INTERVAL) spawn_timer_ = Caps::SPAWN_MIN_INTERVAL;
        } else {
            spawn_timer_ = 0.4f;
        }
    }

    update_allies(dt);
    update_enemies(dt);
    update_player_bullets(dt);
    update_ally_bullets(dt);
    update_enemy_bullets(dt);
    compute_lanes(); /* diagnostics describe surviving bullets at the end of the step */
    update_particles(dt);
    check_level_progress();
    check_medals();

    /* ---- presentation timers ---- */
    if (banner_t_ > 0.0f) banner_t_ -= dt;
    if (medal_banner_t_ > 0.0f) medal_banner_t_ -= dt;
    if (shake_ > 0.0f) shake_ = shake_ > dt * 6.0f ? shake_ - dt * 6.0f : 0.0f;
    if (hit_flash_ > 0.0f) hit_flash_ -= dt;
    if (fleet_message_t_ > 0.0f) fleet_message_t_ -= dt;

    /* lane occupancy smoothing for the debug overlay */
    for (int i = 0; i < LANES; ++i) {
        float target = lane_hot_[i] ? 1.0f : 0.0f;
        lane_hot_prev_[i] += (target - lane_hot_prev_[i]) * clampf(dt * 6.0f, 0.0f, 1.0f);
    }
}

/* ----------------------------------------------------------------- attract */
void Game::update_attract(float dt)
{
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    dt = clampf(dt, 0.0f, 0.1f);
    attract_t_ += dt;
    if (attract_t_ >= 62.831853f) attract_t_ -= 62.831853f;
    update_stars(dt);
    attract_ship_x_ = PLAY_W * 0.5f + std::sin(attract_t_ * 0.6f) * 70.0f;
    p_.x = attract_ship_x_;
    p_.y = 134.0f + std::sin(attract_t_ * 1.1f) * 7.0f;
    p_.engine_phase += dt * 14.0f;
    if (p_.engine_phase >= 6.2831853f) p_.engine_phase -= 6.2831853f;
    if (rng_.chance(dt * 1.5f)) {
        add_particle(attract_ship_x_ + rng_.between(-4.0f, 4.0f), p_.y + 6.0f, rng_.between(-6.0f, 6.0f),
                     rng_.between(20.0f, 40.0f), 0.6f, 1, art::color('c'), 0);
    }
    update_particles(dt);
}

/* ------------------------------------------------------------------- drawing */
static void draw_sprite_centered(Mui &m, const Sprite &s, float x, float y, int ox, int oy, uint32_t tint,
                                 int tint_alpha)
{
    int px = (int)(x + 0.5f) - s.w / 2 + ox;
    int py = (int)(y + 0.5f) - s.h / 2 + oy;
    if (tint_alpha > 0) mui_blit_tint(&m, s.px, s.w, s.h, px, py, tint, tint_alpha);
    else mui_blit(&m, s.px, s.w, s.h, px, py, 0);
}

void Game::draw_hud(Mui &m) const
{
    const MuiTheme &th = m.th;
    mui_rect(&m, 0, 0, (int)PLAY_W, (int)HUD_H, MUI_RGB(0x08, 0x0A, 0x14));
    mui_hline(&m, 0, (int)HUD_H - 1, (int)PLAY_W, th.panel_edge);
    mui_hline(&m, 0, (int)HUD_H, (int)PLAY_W, MUI_RGB(0x03, 0x04, 0x08));

    if (st_.score < 10000000) mui_textf(&m, 3, 3, th.text_strong, 1, "%07d", st_.score);
    else if (st_.score < 1000000000) mui_textf(&m, 3, 3, th.text_strong, 1, "%.1fM", (double)st_.score / 1000000.0);
    else mui_textf(&m, 3, 3, th.text_strong, 1, "%.2fB", (double)st_.score / 1000000000.0);
    if (st_.level < 10000) mui_textf(&m, 62, 3, th.accent, 1, "LV%02d", st_.level);
    else if (st_.level < 1000000) mui_textf(&m, 62, 3, th.accent, 1, "LV%dK", st_.level / 1000);
    else mui_textf(&m, 62, 3, th.accent, 1, "LV%dM", st_.level / 1000000);
    /* lives as hearts */
    for (int i = 0; i < 3; ++i) {
        const Sprite &s = (i < p_.hp) ? art::heart_full : art::heart_empty;
        mui_blit(&m, s.px, s.w, s.h, 108 + i * 9, 4, 0);
    }
    /* how close the next medal is */
    int best = ladder_best();
    int t[MEDAL_COUNT];
    medal_thresholds(best, t);
    int next = MEDAL_COUNT - 1;
    for (int i = 0; i < MEDAL_COUNT; ++i) {
        if (st_.score < t[i]) {
            next = i;
            break;
        }
    }
    mui_text_right(&m, (int)PLAY_W - 3, 3, medal_name(next), t[next] > 0 ? th.warn : th.text_dim, 1);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d", t[next]);
    mui_text_right(&m, (int)PLAY_W - 3 - mui_text_w(medal_name(next), 1) - 4, 3, buf, th.text_dim, 1);

    /* medal ladder bar across the bottom of the HUD */
    int bar_x = 3, bar_w = (int)PLAY_W - 6;
    mui_rect(&m, bar_x, (int)HUD_H - 4, bar_w, 2, MUI_RGB(0x16, 0x1A, 0x2A));
    if (t[MEDAL_COUNT - 1] > 0) {
        int fill = (int)((float)st_.score / (float)t[MEDAL_COUNT - 1] * (float)bar_w);
        if (fill > bar_w) fill = bar_w;
        if (fill > 0) mui_rect(&m, bar_x, (int)HUD_H - 4, fill, 2, th.accent);
        for (int i = 0; i < MEDAL_COUNT; ++i) {
            int tx = bar_x + (int)((float)t[i] / (float)t[MEDAL_COUNT - 1] * (float)bar_w);
            if (tx > bar_x && tx < bar_x + bar_w) mui_vline(&m, tx, (int)HUD_H - 5, 4, th.text_dim);
        }
    }
}

void Game::draw_banner(Mui &m) const
{
    if (banner_t_ <= 0.0f || banner_[0] == '\0') return;
    const char *label = medal_banner_t_ > 0.0f ? banner_ : banner_;
    int a = (int)(clampf(banner_t_ / 0.6f, 0.0f, 1.0f) * 220.0f);
    int w = mui_text_w(label, 1) + 20;
    int x = (int)(PLAY_W * 0.5f) - w / 2;
    int y = (int)HUD_H + 20;
    mui_rect_blend(&m, x, y, w, 13, MUI_RGB(0x0A, 0x0D, 0x18), a);
    mui_rect_outline(&m, x, y, w, 13, medal_banner_t_ > 0.0f ? m.th.warn : m.th.accent);
    mui_text_center(&m, (int)(PLAY_W * 0.5f), y + 3, label, m.th.text_strong, 1);
}

void Game::draw(Mui &m, bool debug) const
{
    /* ---- background: gradient + parallax stars ---- */
    for (int y = 0; y < (int)PLAY_H; ++y) {
        float k = (float)y / PLAY_H;
        int r = (int)lerpf(7.0f, 2.0f, k);
        int g = (int)lerpf(9.0f, 4.0f, k);
        int b = (int)lerpf(22.0f, 10.0f, k);
        mui_hline(&m, 0, y, (int)PLAY_W, MUI_RGB(r, g, b));
    }
    for (int i = 0; i < MAX_STARS; ++i) {
        const Star &s = stars_[i];
        mui_px(&m, (int)s.x, (int)s.y, star_color(s.bright));
    }

    int ox = 0, oy = 0;
    if (shake_ > 0.01f) {
        ox = (int)(std::sin(st_.seconds * 61.0f) * shake_);
        oy = (int)(std::cos(st_.seconds * 47.0f) * shake_);
    }

    /* ---- enemies ---- */
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const Enemy &e = enemies_[i];
        if (!e.alive) continue;
        const Sprite *s = &art::enemy_grunt;
        switch (e.kind) {
        case EK_WASP: s = &art::enemy_wasp; break;
        case EK_BRUTE: s = &art::enemy_brute; break;
        case EK_GHOST: s = &art::enemy_ghost; break;
        default: break;
        }
        int alpha = 0;
        uint32_t tint = 0;
        if (e.kind == EK_GHOST && (dir_.output().trick != TRICK_NONE)) {
            tint = art::color('C');
            alpha = (int)(dir_.output().trick_intensity * 70.0f);
        } else if (e.hp < e.max_hp) {
            tint = art::color('w');
            alpha = 60;
        }
        draw_sprite_centered(m, *s, e.x, e.y, ox, oy, tint, alpha);
        /* damage pips for armoured ships */
        if (e.max_hp > 1 && e.hp < e.max_hp) {
            mui_rect(&m, (int)e.x - 6 + ox, (int)e.y + s->h / 2 + 2 + oy, 12, 1, art::color('d'));
            mui_rect(&m, (int)e.x - 6 + ox, (int)e.y + s->h / 2 + 2 + oy,
                     (int)(12.0f * (float)e.hp / (float)e.max_hp), 1, art::color('R'));
        }
    }

    /* ---- allies: upward silhouettes and persistent health bars ---- */
    for (const Ally &a : allies_) {
        if (!a.alive) continue;
        const Sprite &sprite = art::ally[a.kind];
        draw_sprite_centered(m, sprite, a.x, a.y, ox, oy, art::color('w'), a.hurt > 0 ? 120 : 0);
        int x = (int)a.x - sprite.w / 2 + ox, y = (int)a.y + sprite.h / 2 + 2 + oy;
        mui_rect(&m, x, y, sprite.w, 2, art::color('d'));
        mui_rect(&m, x, y, (sprite.w * a.hp + ally_spec(a.kind).hp - 1) / ally_spec(a.kind).hp, 2, art::color('E'));
    }
    for (const Bullet &b : abullets_) {
        if (!b.alive) continue;
        draw_sprite_centered(m, art::bullet_ally, b.x, b.y, ox, oy, art::color('G'), b.damage > 1 ? 120 : 0);
    }
    /* ---- bullets ---- */
    for (int i = 0; i < MAX_PBULLETS; ++i) {
        const Bullet &b = pbullets_[i];
        if (!b.alive) continue;
        draw_sprite_centered(m, art::bullet_player, b.x, b.y, ox, oy, 0, 0);
    }
    for (int i = 0; i < MAX_EBULLETS; ++i) {
        const Bullet &b = ebullets_[i];
        if (!b.alive) continue;
        const Sprite &s = b.kind == 2 ? art::bullet_big : art::bullet_enemy;
        draw_sprite_centered(m, s, b.x, b.y, ox, oy, 0, 0);
    }

    /* ---- particles ---- */
    for (int i = 0; i < MAX_PARTICLES; ++i) {
        const Particle &pt = parts_[i];
        if (!pt.alive) continue;
        float k = pt.life / (pt.max_life > 0.0f ? pt.max_life : 1.0f);
        if (pt.kind == 2) {
            const Sprite &s = pt.size > 1 ? art::flash_big : art::flash_small;
            draw_sprite_centered(m, s, pt.x, pt.y, ox, oy, 0, 0);
            continue;
        }
        int r = (int)(((pt.col >> 16) & 0xFF) * k + 24.0f * (1.0f - k));
        int g = (int)(((pt.col >> 8) & 0xFF) * k + 8.0f * (1.0f - k));
        int b = (int)((pt.col & 0xFF) * k);
        mui_rect(&m, (int)pt.x + ox, (int)pt.y + oy, pt.size, pt.size, MUI_RGB(r, g, b));
    }

    /* ---- player: always visible, tinted while invulnerable ---- */
    if (!over_) {
        uint32_t tint = 0;
        int alpha = 0;
        if (p_.invuln > 0.0f) {
            tint = art::color('w');
            alpha = ((int)(p_.invuln * 14.0f) % 2 == 0) ? 35 : 115;
        }
        draw_sprite_centered(m, art::player, p_.x, p_.y, ox, oy, tint, alpha);
        /* engine flame flickers with movement */
        uint32_t flame = (int)(p_.engine_phase) % 2 == 0 ? art::color('y') : art::color('o');
        mui_rect(&m, (int)p_.x - 1 + ox, (int)p_.y + 6 + oy, 3, 2, flame);
    }

    /* ---- fairness corridor overlay (debug only) ---- */
    if (debug) {
        float lw = PLAY_W / (float)LANES;
        for (int i = 0; i < LANES; ++i) {
            int a = (int)(lane_hot_prev_[i] * 46.0f);
            if (a > 0) mui_rect_blend(&m, (int)((float)i * lw), (int)HUD_H, (int)lw, (int)(PLAY_H - HUD_H),
                                      art::color('R'), a);
        }
        mui_textf(&m, 4, (int)HUD_H + 4, m.th.text_dim, 1, "INBOUND %d/%d  BUDGET %d", hot_count_, LANES,
                  dir_.output().max_hot_lanes);
    }

    if (hit_flash_ > 0.0f) {
        int a = (int)(hit_flash_ * 500.0f);
        if (a > 160) a = 160;
        mui_rect_blend(&m, 0, 0, (int)PLAY_W, (int)PLAY_H, art::color('R'), a);
    }

    draw_hud(m);
    draw_banner(m);
    draw_fleet(m);

    if (debug) {
        const DirectorOutput &o = dir_.output();
        int y = (int)HUD_H + 14;
        mui_textf(&m, 4, y, m.th.good, 1, "ML policy %d params  L1 %.1f", dir_.policy_net().params(),
                  (double)dir_.policy_net().weight_l1());
        mui_textf(&m, 4, y + 9, m.th.good, 1, "intent %+.2f  stress %.2f  style %s", (double)dir_.intent_bias(),
                  (double)dir_.stress_prob(), dir_.style_label());
        mui_textf(&m, 4, y + 18, m.th.good, 1, "%s  p %.2f mercy %.2f weird %.2f", o.phase_label, (double)o.pressure,
                  (double)o.mercy, (double)o.weirdness);
        mui_textf(&m, 4, y + 27, m.th.good, 1, "trick %s (%d tried)  dom %.2f", trick_name(o.trick),
                  dir_.tricks_tried(), (double)o.dominance);
        int by = y + 40;
        for (int i = 1; i < TRICK_COUNT; ++i) {
            float v = dir_.bandit().value(i - 1);
            mui_textf(&m, 4, by + (i - 1) * 8, m.th.text_dim, 1, "%-13s %+.2f", trick_name(i), (double)v);
        }
        int counts[EK_COUNT] = {0, 0, 0, 0};
        for (int i = 0; i < MAX_ENEMIES; ++i) {
            if (enemies_[i].alive) counts[enemies_[i].kind]++;
        }
        mui_textf(&m, 4, by + TRICK_COUNT * 8 + 3, m.th.good, 1, "%s %d  %s %d  %s %d  %s %d", KIND_NAMES[0], counts[0],
                  KIND_NAMES[1], counts[1], KIND_NAMES[2], counts[2], KIND_NAMES[3], counts[3]);
        mui_textf(&m, 4, by + TRICK_COUNT * 8 + 12, m.th.good, 1,
                  "cap: enemies %d bullets %d/%d speed %.0f fire %.2f", dir_.output().max_enemies, enemy_bullets(),
                  dir_.output().max_bullets, (double)dir_.output().bullet_speed, (double)dir_.output().fire_interval);
        mui_textf(&m, 4, by + TRICK_COUNT * 8 + 21, m.th.good, 1,
                  "ALLY ML %u updates  KILLS %d  BLOCKS %d", ally_policy_.steps, st_.ally_kills, st_.intercepted);
    }
}

void Game::draw_attract(Mui &m) const
{
    for (int y = 0; y < (int)PLAY_H; ++y) {
        float k = (float)y / PLAY_H;
        int r = (int)lerpf(6.0f, 2.0f, k);
        int g = (int)lerpf(8.0f, 3.0f, k);
        int b = (int)lerpf(20.0f, 9.0f, k);
        mui_hline(&m, 0, y, (int)PLAY_W, MUI_RGB(r, g, b));
    }
    for (int i = 0; i < MAX_STARS; ++i) {
        const Star &s = stars_[i];
        mui_px(&m, (int)s.x, (int)s.y, star_color(s.bright));
    }
    for (int i = 0; i < MAX_PARTICLES; ++i) {
        const Particle &pt = parts_[i];
        if (!pt.alive) continue;
        float k = pt.life / (pt.max_life > 0.0f ? pt.max_life : 1.0f);
        int b = (int)((pt.col & 0xFF) * k);
        mui_rect(&m, (int)pt.x, (int)pt.y, pt.size, pt.size, MUI_RGB((int)(30 * k), (int)(120 * k), b));
    }
    draw_sprite_centered(m, art::player, attract_ship_x_, p_.y, 0, 0, 0, 0);
    mui_rect(&m, (int)attract_ship_x_ - 1, (int)p_.y + 6, 3, 2, art::color('o'));
    /* keep the lower third readable for the taglines and menu chrome */
    mui_rect_blend(&m, 0, (int)PLAY_H - 46, (int)PLAY_W, 46, MUI_RGB(0x05, 0x06, 0x0C), 190);
}

} /* namespace mss */
