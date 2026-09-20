/* allies.cpp — score-funded escort fleet and a separate shared online policy.
 * Neural decisions select a role; bounded steering/aiming implements it.
 * Projectile credit keeps the firing observation even if its ship is gone.
 */
#include "game.hpp"
#include "audio.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mss {
namespace {
const AllySpec SPECS[AK_COUNT] = {
    {"SCOUT",    600,  2, 1, 1, 86.0f, 0.95f},
    {"WING",    1600,  5, 1, 2, 78.0f, 0.85f},
    {"CRUISER", 3600, 10, 2, 2, 66.0f, 0.75f},
    {"TITAN",   7200, 18, 3, 3, 58.0f, 0.65f},
};
float bounded(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float distance2(float x, float y) { return x * x + y * y; }
}
const AllySpec &ally_spec(int kind) { return SPECS[kind >= 0 && kind < AK_COUNT ? kind : AK_SCOUT]; }

int Game::allies_alive() const
{
    int count = 0;
    for (const Ally &a : allies_) count += a.alive ? 1 : 0;
    return count;
}

void Game::award_points(int64_t points)
{
    if (points <= 0) return;
    /* The wallet continues to refill even after the displayed score saturates. */
    st_.score = counter((int64_t)st_.score + points);
    credits_ = counter((int64_t)credits_ + points);
}

void Game::reset_allies(bool keep_learning)
{
    credits_ = 0;
    fleet_message_t_ = 0;
    fleet_message_[0] = '\0';
    for (Ally &a : allies_) a = Ally();
    for (Bullet &b : abullets_) b = Bullet();
    ally_rng_.seed(rng_.s ^ 0xA11E5123u);
    if (!keep_learning || ally_policy_.nin == 0) {
        ally_policy_.init(12, 12, ALLY_ACTIONS, ally_rng_, 0.012f);
        /* Useful initial preference; no fabricated training observations. */
        ally_policy_.b2[ALLY_GUARD] = 0.8f;
        ally_policy_.b2[ALLY_ATTACK] = 0.4f;
    }
}

bool Game::recruit(int kind)
{
    if (over_ || kind < 0 || kind >= AK_COUNT) return false;
    const AllySpec &spec = SPECS[kind];
    fleet_message_t_ = 2.5f;
    if (allies_alive() >= MAX_ALLIES) {
        std::snprintf(fleet_message_, sizeof(fleet_message_), "FLEET FULL - 4 SHIPS");
        return false;
    }
    if (credits_ < spec.cost) {
        std::snprintf(fleet_message_, sizeof(fleet_message_), "%s NEEDS %d MORE CREDITS", spec.name, spec.cost - credits_);
        return false;
    }
    for (int i = 0; i < MAX_ALLIES; ++i) {
        Ally &a = allies_[i];
        if (a.alive) continue;
        credits_ -= spec.cost;
        a = Ally();
        a.alive = true;
        a.kind = kind;
        a.hp = spec.hp;
        a.x = bounded(p_.x + (i % 2 ? 1.0f : -1.0f) * (22.0f + (float)(i / 2) * 22.0f), 12.0f, PLAY_W - 12.0f);
        a.y = bounded(p_.y - 21.0f, HUD_H + 12.0f, FLEET_TOP - 14.0f);
        a.hurt = 0.6f; /* deployment grace */
        a.fire_cd = 0.25f;
        st_.allies_bought = counter((int64_t)st_.allies_bought + 1);
        std::snprintf(fleet_message_, sizeof(fleet_message_), "%s DEPLOYED - %d HULL", spec.name, spec.hp);
        aud_play(SFX_RECRUIT, 1.0f + (float)kind * 0.1f);
        return true;
    }
    return false;
}

void Game::finish_ally_policy(Ally &a, float reward)
{
    if (a.policy_valid) ally_policy_.train_policy(a.features, a.action, bounded(a.reward + reward, -1.5f, 1.5f), 0.015f);
    a.policy_valid = false;
    a.reward = 0.0f;
}

void Game::damage_ally(Ally &a, int amount, bool protecting)
{
    if (!a.alive || a.hurt > 0.0f) return;
    a.hp -= amount;
    a.hurt = 0.22f;
    if (a.policy_valid) a.reward += protecting ? 0.45f : -0.35f;
    if (protecting) st_.intercepted = counter((int64_t)st_.intercepted + 1);
    if (a.hp <= 0) {
        a.hp = 0;
        a.alive = false;
        finish_ally_policy(a, -0.7f);
        st_.allies_lost = counter((int64_t)st_.allies_lost + 1);
        explode(a.x, a.y, art::color('E'), a.kind >= AK_CRUISER ? 2 : 1);
        aud_play(SFX_EXPLODE, 0.8f);
        std::snprintf(fleet_message_, sizeof(fleet_message_), "%s LOST - SLOT AVAILABLE", SPECS[a.kind].name);
        fleet_message_t_ = 2.5f;
    } else {
        add_particle(a.x, a.y, 0, 20, 0.2f, 2, art::color('E'), 0);
        aud_play(SFX_HIT_ENEMY, 0.8f);
    }
}

bool Game::intercept_bullet(Bullet &b)
{
    for (Ally &a : allies_) {
        if (!a.alive || a.hurt > 0.0f) continue;
        const Sprite &sprite = art::ally[a.kind];
        if (std::fabs(b.x - a.x) >= (float)sprite.w * 0.5f + 1.0f ||
            std::fabs(b.y - a.y) >= (float)sprite.h * 0.5f + 1.0f) continue;
        float t = b.vy > 0.0f ? (p_.y - b.y) / b.vy : -1.0f;
        bool protecting = t >= 0.0f && t < 2.0f && std::fabs(b.x + b.vx * t - p_.x) < 20.0f;
        b.alive = false;
        damage_ally(a, b.kind == 2 ? 2 : 1, protecting);
        return true;
    }
    return false;
}

void Game::ally_fire(Ally &a, const Enemy &target)
{
    const AllySpec &spec = SPECS[a.kind];
    int fired = 0;
    for (int n = 0; n < spec.volley; ++n) {
        for (Bullet &b : abullets_) {
            if (b.alive) continue;
            b = Bullet();
            b.alive = true;
            b.kind = 3;
            b.damage = spec.damage;
            float spread = ((float)n - (float)(spec.volley - 1) * 0.5f) * 4.0f;
            b.x = a.x + spread;
            b.y = a.y - (float)art::ally[a.kind].h * 0.5f;
            float lead = bounded((a.y - target.y) / 180.0f, 0.0f, 0.65f);
            float dx = target.x + target.vx * lead + spread - b.x;
            float dy = target.y + target.vy * lead - b.y;
            if (dy > -8.0f) dy = -8.0f;
            float length = std::sqrt(distance2(dx, dy));
            b.vx = 180.0f * dx / length;
            b.vy = 180.0f * dy / length;
            b.life = 2.0f;
            b.policy_valid = a.policy_valid;
            b.action = a.action;
            std::memcpy(b.features, a.features, sizeof(a.features));
            ++fired;
            break;
        }
    }
    a.fire_cd = fired ? spec.reload : 0.1f;
    if (fired) aud_play(SFX_ALLY_SHOT, 1.2f - (float)a.kind * 0.12f);
}

void Game::update_allies(float dt)
{
    for (int i = 0; i < MAX_ALLIES; ++i) {
        Ally &a = allies_[i];
        if (!a.alive) continue;
        const AllySpec &spec = SPECS[a.kind];
        a.hurt = bounded(a.hurt - dt, 0.0f, 1.0f);
        a.fire_cd -= dt;
        /* Prefer nearby threats to the player, including enemies below other enemies. */
        const Enemy *target = nullptr;
        float best = 1e9f;
        for (const Enemy &e : enemies_) {
            if (!e.alive || e.y >= a.y - 5.0f || e.y < HUD_H) continue;
            float d = distance2(e.x - p_.x, e.y - p_.y);
            if (d < best) { best = d; target = &e; }
        }
        const Bullet *threat = nullptr;
        best = 1e9f;
        for (const Bullet &b : ebullets_) {
            if (!b.alive || b.y > a.y + 8.0f) continue;
            float d = distance2(b.x - a.x, b.y - a.y);
            if (d < best) { best = d; threat = &b; }
        }
        float side = i % 2 ? 1.0f : -1.0f;
        float home_x = bounded(p_.x + side * (22.0f + 22.0f * (float)(i / 2)), 12.0f, PLAY_W - 12.0f);
        float home_y = bounded(p_.y - 22.0f - (float)(i / 2) * 12.0f, HUD_H + 12.0f, FLEET_TOP - 14.0f);
        a.decision_t -= dt;
        if (a.decision_t <= 0.0f) {
            finish_ally_policy(a, 0.0f);
            float f[12] = {
                target ? bounded((target->x - a.x) / PLAY_W, -1, 1) : 0,
                target ? bounded((target->y - a.y) / PLAY_H, -1, 1) : 0,
                (p_.x - a.x) / PLAY_W, (p_.y - a.y) / PLAY_H,
                (float)a.hp / (float)spec.hp,
                threat ? bounded((threat->x - a.x) / 80.0f, -1, 1) : 0,
                threat ? bounded((threat->y - a.y) / 80.0f, -1, 1) : 0,
                threat ? 1.0f : 0.0f, (float)a.kind / 3.0f, (float)p_.hp / 3.0f,
                target ? 1.0f : 0.0f, 1.0f
            };
            std::memcpy(a.features, f, sizeof(f));
            ally_policy_.forward(f);
            float probs[ALLY_ACTIONS];
            ally_policy_.softmax(probs, ALLY_ACTIONS);
            float roll = ally_rng_.uni();
            a.action = ALLY_ACTIONS - 1;
            for (int k = 0; k < ALLY_ACTIONS; ++k) {
                roll -= probs[k];
                if (roll <= 0.0f) { a.action = k; break; }
            }
            a.policy_valid = true;
            a.decision_t = 0.45f;
        }
        float tx = home_x, ty = home_y;
        if (a.action == ALLY_GUARD) {
            /* Find a shot that will cross the player's predicted corridor. */
            float soonest = 2.0f;
            for (const Bullet &b : ebullets_) {
                if (!b.alive || b.vy <= 0.0f || b.y >= home_y) continue;
                float t = (home_y - b.y) / b.vy;
                float impact = b.x + b.vx * ((p_.y - b.y) / b.vy);
                if (t < soonest && std::fabs(impact - p_.x) < 24.0f) {
                    tx = b.x + b.vx * t;
                    soonest = t;
                }
            }
        } else if (a.action == ALLY_ATTACK && target) {
            tx = target->x + side * 6.0f;
            ty = bounded(target->y + 52.0f, p_.y - 66.0f, home_y);
        } else if (a.action == ALLY_EVADE && threat) {
            tx = a.x + (a.x >= threat->x ? 35.0f : -35.0f);
            ty = home_y + 7.0f;
        }
        tx = bounded(tx, 12.0f, PLAY_W - 12.0f);
        ty = bounded(ty, HUD_H + 12.0f, FLEET_TOP - 14.0f);
        float dx = tx - a.x, dy = ty - a.y;
        float length = std::sqrt(distance2(dx, dy));
        if (length > 0.01f) {
            float step = bounded(length * 3.5f, 0.0f, spec.speed) * dt;
            if (step > length) step = length;
            a.x += dx * step / length;
            a.y += dy * step / length;
        }
        /* Small formation reward; actual hits, interceptions and losses dominate. */
        if (distance2(a.x - home_x, a.y - home_y) < 32.0f * 32.0f) a.reward += dt * 0.025f;
        if (target && a.fire_cd <= 0.0f) ally_fire(a, *target);
    }
}

void Game::update_ally_bullets(float dt)
{
    for (Bullet &b : abullets_) {
        if (!b.alive) continue;
        b.x += b.vx * dt; b.y += b.vy * dt; b.life -= dt;
        if (b.x < -8 || b.x > PLAY_W + 8 || b.y < HUD_H - 4 || b.life <= 0) {
            b.alive = false;
            if (b.policy_valid) ally_policy_.train_policy(b.features, b.action, -0.025f, 0.015f);
            continue;
        }
        for (int k = 0; k < MAX_ENEMIES; ++k) {
            Enemy &e = enemies_[k];
            if (!e.alive) continue;
            float hw = e.kind == EK_BRUTE ? 7.5f : 5.5f;
            float hh = e.kind == EK_BRUTE ? 6.5f : (e.kind == EK_GHOST ? 5.5f : 5.0f);
            if (std::fabs(b.x - e.x) >= hw || std::fabs(b.y - e.y) >= hh) continue;
            b.alive = false;
            e.hp -= b.damage;
            e.reward_acc -= 0.35f;
            if (b.policy_valid) ally_policy_.train_policy(b.features, b.action, e.hp <= 0 ? 0.9f : 0.4f, 0.015f);
            if (e.hp <= 0) {
                kill_enemy(k, true);
                st_.ally_kills = counter((int64_t)st_.ally_kills + 1);
            } else {
                aud_play(SFX_HIT_ENEMY, 1.0f);
                add_particle(b.x, b.y, 0, 8, 0.15f, 2, art::color('E'), 0);
            }
            break;
        }
    }
}

void Game::draw_fleet(Mui &m) const
{
    int y = (int)FLEET_TOP;
    mui_rect(&m, 0, y, (int)PLAY_W, 23, MUI_RGB(8, 13, 22));
    mui_hline(&m, 0, y, (int)PLAY_W, m.th.panel_edge);
    char wallet[24];
    if (credits_ < 1000000) std::snprintf(wallet, sizeof(wallet), "CREDIT %d", credits_);
    else std::snprintf(wallet, sizeof(wallet), "CREDIT %dM", credits_ / 1000000);
    mui_text(&m, 4, y + 3, wallet, m.th.good, 1);
    mui_textf(&m, 120, y + 3, m.th.text, 1, "FLEET %d/4", allies_alive());
    if (fleet_message_t_ > 0) mui_text_right(&m, 380, y - 10, fleet_message_, m.th.good, 1);
    mui_text_right(&m, 380, y + 3, "1-4 BUY  |  KILLS REFILL", m.th.text_dim, 1);
    for (int i = 0; i < AK_COUNT; ++i) {
        const AllySpec &spec = SPECS[i];
        uint32_t color = credits_ >= spec.cost && allies_alive() < MAX_ALLIES ? m.th.good : m.th.text_dim;
        if (i == AK_CRUISER) mui_textf(&m, 4 + i * 96, y + 13, color, 1, "%d:CRSR %d", i + 1, spec.cost);
        else mui_textf(&m, 4 + i * 96, y + 13, color, 1, "%d:%s %d", i + 1, spec.name, spec.cost);
    }
}
} /* namespace mss */
