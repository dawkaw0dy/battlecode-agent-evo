// Fights with enemy champions: a short focus-fire simulation decides between engaging and
// fleeing; fleeing walks home through the tiles the fewest enemies (and awake camps) can hit.
#include <cambc.h>
#include "bot.h"
#include <cmath>
#include <cstdio>

using cambc::champion_type;
namespace vision = cambc::vision;

namespace {

#ifndef EVO_ENGAGE
#define EVO_ENGAGE -60
#endif
constexpr float kEngageBias = EVO_ENGAGE;   // fight value needed to engage
constexpr int kAvoidRounds = 20;          // rounds a fled-from area stays avoided

bool has_reset(champion_type t) {
    switch (t) {
        case champion_type::vesper_vale: case champion_type::vendetta: case champion_type::ryth:
        case champion_type::willow: case champion_type::varg: case champion_type::tavi:
        case champion_type::issac: case champion_type::marcella:
            return true;
        default:
            return false;
    }
}

// Focus multiplier of the damaging spells in a kit (both slots).
float spell_mult(champion_type t) {
    switch (t) {
        case champion_type::cassi: return 2.0f;
        case champion_type::stellar: return 3.4f;
        case champion_type::prism: return 2.5f;
        case champion_type::marcella: return 2.0f;
        case champion_type::tavi: return 1.4f;
        case champion_type::ren: return 0.75f;
        case champion_type::ryth: return 1.0f;
        case champion_type::heart_of_ruin: return 1.5f;
        default: return 0.0f;
    }
}

}  // namespace

bool dash_capable(champion_type t) {
    return t == champion_type::vesper_vale || t == champion_type::vendetta || t == champion_type::issac
        || t == champion_type::ren || t == champion_type::heart_of_ruin;
}

void champion_power(champion_type t, float str, float focus, float* dps, float* burst) {
    const float basic = str * (has_reset(t) ? 1.125f : 1.0f);
    const float spell = spell_mult(t) * focus;
    *dps = basic + spell / 8.0f;
    *burst = spell;
}

float Bot::camp_danger_at(vec2 t) const {
    float d = 0;
    for (const auto& c : awake_)
        if (cheb(t, c.pos) <= c.range) d += c.str;
    return d;
}

float Bot::danger_at(vec2 t, bool dash_reach) const {
    float d = 0;
    for (const auto& e : threats_) {
        if (e.channel) continue;
        int reach = e.range <= 1 ? 2 : e.range;
        if (dash_reach && e.dasher) reach += 2;
        if (cheb(t, e.pos) <= reach) d += e.dps;
    }
    return d;
}

// Simulate a focus-fire exchange between the champions near us. Positive favours us.
float Bot::fight_value(bool* i_die) const {
    struct U {
        float hp, dps, burst;
        int delay;
        bool me;
    };
    std::vector<U> us, them;
    auto delay_of = [](int d, int range, bool dasher) {
        int dl = range <= 1 ? std::max(0, d - 2) : std::max(0, d - range);
        if (dasher) dl = std::max(0, dl - 2);
        return dl;
    };
    // Nearest enemy distance for each of our units, and vice versa.
    auto nearest_enemy = [&](vec2 p) {
        int best = kFar;
        for (const auto& e : threats_) best = std::min(best, cheb(p, e.pos));
        return best;
    };
    us.push_back({hp_, my_dps_, my_burst_, delay_of(nearest_enemy(pos_), range_, kit_.dash >= 0), true});
    for (const auto& a : allies_)
        if (cheb(a.pos, pos_) <= 7) us.push_back({a.hp, a.dps, 0.0f, delay_of(nearest_enemy(a.pos), a.range, false), false});
    float gem_bonus = 0;
    for (const auto& e : threats_) {
        int d = kFar;
        for (const auto& u : us) (void)u;
        d = std::min(d, cheb(e.pos, pos_));
        for (const auto& a : allies_)
            if (cheb(a.pos, pos_) <= 7) d = std::min(d, cheb(e.pos, a.pos));
        if (d > 10) continue;
        // A channelling enemy loses its first turns.
        const int extra = e.channel ? 2 : 0;
        them.push_back({e.hp, e.dps, e.burst, delay_of(d, e.range, e.dasher) + extra, false});
        gem_bonus += static_cast<float>(e.gems);
    }
    *i_die = false;
    if (them.empty()) return 1000.0f;

    float dealt = 0, taken = 0;
    int kills = 0, deaths = 0;
    auto lowest = [](std::vector<U>& v) {
        int k = -1;
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].hp > 0 && (k < 0 || v[i].hp < v[static_cast<size_t>(k)].hp)) k = static_cast<int>(i);
        return k;
    };
    auto apply = [&](std::vector<U>& v, float dmg, float& sum, int& dead) {
        while (dmg > 0) {
            const int k = lowest(v);
            if (k < 0) return;
            auto& u = v[static_cast<size_t>(k)];
            const float hit = std::min(dmg, u.hp);
            u.hp -= hit;
            dmg -= hit;
            sum += hit;
            if (u.hp <= 0) ++dead;
        }
    };
    for (int t = 0; t < 12; ++t) {
        float d_us = 0, d_them = 0;
        for (const auto& u : us)
            if (u.hp > 0 && t >= u.delay) d_us += u.dps + (t == u.delay ? u.burst : 0.0f);
        for (const auto& u : them)
            if (u.hp > 0 && t >= u.delay) d_them += u.dps + (t == u.delay ? u.burst : 0.0f);
        apply(them, d_us, dealt, kills);
        apply(us, d_them, taken, deaths);
        if (us[0].hp <= 0) *i_die = true;
        if (lowest(them) < 0 || lowest(us) < 0) break;
    }
    float v = dealt - taken + 500.0f * static_cast<float>(kills - deaths);
    if (kills > 0) v += 6.0f * gem_bonus / static_cast<float>(them.size());
    if (*i_die) v -= 300.0f + 8.0f * static_cast<float>(gems_);
    return v;
}

bool Bot::handle_enemies() {
    if (threats_.empty()) return false;
    int nearest = kFar;
    for (const auto& e : threats_) nearest = std::min(nearest, cheb(e.pos, pos_));
    if (nearest > 10) return false;

    bool i_die = false;
    const float v = fight_value(&i_die);
    if (kTrace && round_ >= kTraceFrom && round_ < kTraceTo) {
        std::printf("F r%d s%d (%d,%d) hp%.0f v%.0f die%d danger%.0f camp%.0f |", round_, slot_, pos_.x, pos_.y, hp_, v, i_die ? 1 : 0,
            danger_at(pos_, true), camp_danger_at(pos_));
        for (const auto& e : enemies_) std::printf(" e(%d,%d)%.0f/%d", e.pos.x, e.pos.y, e.hp, static_cast<int>(e.type));
        std::printf("\n");
    }
    // A kill within reach this turn is always taken.
    if (!attacked_ && cambc::self().can_attack()) {
        const float hit = str_ * (kit_.reset >= 0 ? 2.0f : 1.0f) + my_burst_;
        for (const auto& e : enemies_)
            if (cheb(e.pos, pos_) <= range_ && e.hp <= 0.9f * hit) return engage();
    }
    if (v > kEngageBias && !i_die) {
        // Favourable: hit whoever we can reach this turn; never chase (equal speeds, it only drags us away).
        const int strike = ranged_ ? range_ : 2 + (kit_.dash >= 0 ? 2 : 0);
        for (const auto& e : enemies_) {
            const int d = cheb(e.pos, pos_);
            if (d <= strike || (e.channel && d <= strike + 3)) return engage();
        }
        return false;
    }

    // Not worth fighting: leave if anything can reach us soon, or a stronger force is close.
    const bool threatened = danger_at(pos_, true) > 0 || (v < -100.0f && nearest <= 7);
    if (at_spawn_ && v < 0 && nearest <= 8) return hold_spawn();
    if (!threatened) return false;
    // Remember where the danger is: the next targets avoid it for a while.
    avoid_pos_ = threats_.front().pos;
    avoid_until_ = round_ + kAvoidRounds;
    return flee();
}

bool Bot::engage() {
    auto& me = cambc::self();
    act_ = kActFight;
    const EnemyView* best = nullptr;
    float best_score = 1e9f;
    for (const auto& e : enemies_) {
        const int d = cheb(e.pos, pos_);
        if (d > 10) continue;
        float s = e.hp / std::max(10.0f, my_dps_) + 2.0f * static_cast<float>(std::max(0, d - range_));
        if (e.channel) s -= 3.0f;
        if (s < best_score) {
            best_score = s;
            best = &e;
        }
    }
    if (!best) return false;
    const vec2 tp = best->pos;

    auto try_hit = [&]() {
        if (attacked_ || !me.can_attack() || cheb(tp, pos_) > range_) return false;
        try {
            const auto target = vision::champion_at(tp);
            if (!cast_ && !moved_) {
                for (int slot : {kit_.cc, kit_.nuke}) {
                    if (slot < 0 || cast_) continue;
                    const float r = slot == kit_.cc ? kit_.cc_range : kit_.nuke_range;
                    if (static_cast<float>(cheb(tp, pos_)) > r) continue;
                    const auto ab = me.abilities()[static_cast<size_t>(slot)];
                    if (ab.is_ready() && ab.use(target)) cast_ = true;
                }
            }
            if (!vision::has_champion(tp)) return true;
            me.attack(target);
            attacked_ = true;
            if (!cast_ && kit_.reset >= 0 && vision::has_champion(tp)) {
                const auto ab = me.abilities()[static_cast<size_t>(kit_.reset)];
                if (ab.is_ready() && ab.use()) {
                    cast_ = true;
                    if (vision::has_champion(tp) && me.can_attack()) me.attack(vision::champion_at(tp));
                }
            }
        } catch (...) {
            ++warns_;
        }
        return true;
    };

    if (cheb(tp, pos_) <= range_) {
        try_hit();
        return true;
    }
    // Spells with a longer reach than the basic attack.
    if (!cast_ && !moved_) {
        try {
            for (int slot : {kit_.cc, kit_.nuke}) {
                if (slot < 0 || cast_) continue;
                const float r = slot == kit_.cc ? kit_.cc_range : kit_.nuke_range;
                const bool excl = slot == kit_.cc ? kit_.cc_excl : kit_.nuke_excl;
                if (excl || static_cast<float>(cheb(tp, pos_)) > r) continue;
                const auto ab = me.abilities()[static_cast<size_t>(slot)];
                if (ab.is_ready() && ab.use(vision::champion_at(tp))) cast_ = true;
            }
        } catch (...) {
            ++warns_;
        }
    }
    bitboard goals;
    goals.clear();
    mark_square(goals, tp, ranged_ ? range_ : 1);
    step_towards(goals, true);
    if (!ranged_) try_hit();
    return true;
}

bool Bot::flee() {
    auto& me = cambc::self();
    act_ = kActFlee;
    flee_until_ = round_ + 6;
    const auto blocked = blocked_now();
    auto cost = [&](vec2 t) {
        return (danger_at(t, true) + camp_danger_at(t)) * 10.0f + static_cast<float>(std::min(home_dist(t), 300));
    };
    const float stay = cost(pos_);
    vec2 best = pos_;
    float best_c = stay;
    if (me.can_move() && !moved_ && !move_failed_) {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const vec2 t{pos_.x + dx, pos_.y + dy};
                if ((dx == 0 && dy == 0) || !in_bounds(t) || blocked.is_set(t)) continue;
                const float c = cost(t);
                if (c < best_c) {
                    best_c = c;
                    best = t;
                }
            }
    }
    // Dash when walking leaves us within reach.
    if (kit_.dash >= 0 && !cast_ && danger_at(best, true) > 0) {
        try {
            const auto ab = me.abilities()[static_cast<size_t>(kit_.dash)];
            if (ab.is_ready()) {
                vec2 dbest = pos_;
                float dc = best_c;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const vec2 t{pos_.x + dx, pos_.y + dy};
                        if (std::max(std::abs(dx), std::abs(dy)) != 2 || !in_bounds(t) || blocked.is_set(t)) continue;
                        const float c = cost(t) + 5.0f;
                        if (c < dc) {
                            dc = c;
                            dbest = t;
                        }
                    }
                if (dbest != pos_ && ab.use(dbest)) {
                    cast_ = true;
                    pos_ = dbest;
                    return true;
                }
            }
        } catch (...) {
            ++warns_;
        }
    }
    // Cornered: if moving does not get us out of reach, hit back instead.
    const bool cornered = danger_at(best, false) >= danger_at(pos_, false) && danger_at(pos_, false) > 0;
    auto hit_back = [&]() {
        if (attacked_ || !me.can_attack()) return false;
        const EnemyView* t = nullptr;
        for (const auto& e : enemies_)
            if (cheb(e.pos, pos_) <= range_ && (!t || e.hp < t->hp)) t = &e;
        if (!t) return false;
        try {
            me.attack(vision::champion_at(t->pos));
            attacked_ = true;
        } catch (...) {
            ++warns_;
        }
        return true;
    };
    if (best == pos_ || (cornered && ranged_)) {
        hit_back();
        return true;
    }
    walk_to(best);
    if (!ranged_) hit_back();
    return true;
}

// Enemies wait outside our spawn: stay inside it (they cannot enter) on the tile farthest from them,
// hit back what comes into range, and leave only once the fight is even or they are gone.
bool Bot::hold_spawn() {
    auto& me = cambc::self();
    act_ = kActFlee;
    const auto& spawn = cambc::map::spawn_region(team::ally);
    const auto blocked = blocked_now();
    auto cost = [&](vec2 t) { return danger_at(t, true) * 10.0f - static_cast<float>(cheb(t, threats_.front().pos)); };
    vec2 best = pos_;
    float best_c = cost(pos_);
    if (me.can_move() && !moved_ && !move_failed_)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const vec2 t{pos_.x + dx, pos_.y + dy};
                if ((dx == 0 && dy == 0) || !in_bounds(t) || !spawn.is_set(t) || blocked.is_set(t)) continue;
                const float c = cost(t);
                if (c < best_c) {
                    best_c = c;
                    best = t;
                }
            }
    if (ranged_ || best == pos_) {
        // Ranged: shooting beats shuffling; melee shuffles then hits.
        const EnemyView* t = nullptr;
        for (const auto& e : enemies_)
            if (cheb(e.pos, pos_) <= range_ && (!t || e.hp < t->hp)) t = &e;
        if (t && !attacked_ && me.can_attack()) {
            try {
                me.attack(vision::champion_at(t->pos));
                attacked_ = true;
                return true;
            } catch (...) {
                ++warns_;
            }
        }
    }
    if (best != pos_) walk_to(best);
    if (!ranged_ && !attacked_ && me.can_attack()) {
        for (const auto& e : enemies_)
            if (cheb(e.pos, pos_) <= range_) {
                try {
                    me.attack(vision::champion_at(e.pos));
                    attacked_ = true;
                } catch (...) {
                    ++warns_;
                }
                break;
            }
    }
    return true;
}
