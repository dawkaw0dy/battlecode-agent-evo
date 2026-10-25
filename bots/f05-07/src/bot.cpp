// Big-game hunter: the whole team moves as one pack. The leader (lowest live slot) picks a camp group
// in shared memory; everyone clears it together, captures, banks, and raids golems and monoliths once the
// pack can absorb their damage. Hurt champions step out of boss range and recall, so the tank rotates;
// camps never regenerate, so a boss is worn down over several trips.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace bc = cambc;
using bc::team;
using bc::vec2;

namespace {

int dist(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }

// Shared memory layout (8 bytes): [0] pack target group + 1, [1] flags (bit0: boss engage),
// [2..6] status per slot: 0 unknown, 0x01 dead, else 0x80 alive | 0x40 near target | 0x20 busy | hp/32.
constexpr int kStatus = 2;
constexpr int kSlots = 5;
constexpr std::uint8_t kAlive = 0x80, kNear = 0x40, kBusy = 0x20;

int pick_order(bc::champion_type t) {
    switch (t) {
        case bc::champion_type::vendetta: return 0;
        case bc::champion_type::marcella: return 1;
        case bc::champion_type::varg: return 2;
        case bc::champion_type::ryth: return 3;
        case bc::champion_type::vesper_vale: return 4;
        default: return 5 + static_cast<int>(t);
    }
}

float ab_range(const bc::ability& ab) {
    const auto& inf = ab.info();
    if (!inf) return 0.f;
    return std::visit([](const auto& p) -> float {
        if constexpr (requires { p.range; }) return p.range;
        else return 0.f;
    }, *inf);
}

bool ab_excl(const bc::ability& ab) {
    const auto& inf = ab.info();
    if (!inf) return true;
    return std::visit([](const auto& p) -> bool { return p.exclusive_with_movement; }, *inf);
}

const bc::bitboard& static_blocked() {
    static const bc::bitboard b = [] {
        auto t = bc::map::walls() | bc::map::holes();
        t |= bc::map::altars();
        t |= bc::map::spawn_region(team::enemy);
        return t;
    }();
    return b;
}

bc::bitboard blocked_now(vec2 self_pos) {
    auto b = static_blocked() | bc::vision::champion_tiles();
    b |= bc::vision::camp_tiles();
    b |= bc::vision::teleport_tiles();
    b.set(self_pos, false);
    return b;
}

}  // namespace

void Bot::box(bc::bitboard& b, vec2 p, int r) const {
    const int w = bc::map::width(), h = bc::map::height();
    if (p.x - r >= 0 && p.y - r >= 0 && p.x + r < w && p.y + r < h) {
        b.draw_chebyshev(p, static_cast<float>(r));
        return;
    }
    for (int y = std::max(0, p.y - r); y <= std::min(h - 1, p.y + r); ++y)
        for (int x = std::max(0, p.x - r); x <= std::min(w - 1, p.x + r); ++x) b.set(vec2{x, y});
}

void Bot::init() {
    auto& me = bc::self();
    ranged_ = me.attack_range() > 1;
    // Slot: rank of our champion type among the allies, in draft preference order.
    const auto allies = bc::champions(team::ally);
    team_n_ = static_cast<int>(allies.size());
    const int mine = pick_order(me.champion_name());
    slot_ = 0;
    for (const auto& a : allies)
        if (pick_order(a.champion_name()) < mine) ++slot_;

    handles_ = bc::map::camp_groups();
    groups_.reserve(handles_.size());
    for (const auto& g : handles_) {
        Group G;
        long sx = 0, sy = 0;
        for (const auto& c : g.camps()) {
            const vec2 p = c.spawn_position();
            G.tiles.push_back(p);
            sx += p.x;
            sy += p.y;
            switch (c.kind()) {
                case bc::camp_type::wolf: G.hp += 1350; G.str += 20; G.range = std::max(G.range, 3); break;
                case bc::camp_type::elk: G.hp += 2400; G.str += 60; G.range = std::max(G.range, 4); break;
                case bc::camp_type::monolith:
                    G.hp += 20000; G.str += 200; G.range = std::max(G.range, 5); G.boss = G.monolith = true; break;
                case bc::camp_type::golem: G.hp += 5000; G.str += 120; G.range = std::max(G.range, 4); G.boss = true; break;
                default: G.hp += 120; break;
            }
        }
        if (!G.tiles.empty()) {
            const vec2 mean{static_cast<int>(sx / static_cast<long>(G.tiles.size())),
                            static_cast<int>(sy / static_cast<long>(G.tiles.size()))};
            G.center = G.tiles[0];
            for (auto p : G.tiles)
                if (dist(p, mean) < dist(G.center, mean)) G.center = p;
        }
        G.gems = static_cast<int>(g.gem_value());
        G.gold = static_cast<int>(g.gold_value());
        G.max_respawn = g.max_respawn_timer();
        groups_.push_back(std::move(G));
    }
    altars_ = bc::map::altars().positions();
    const auto spawn = bc::map::spawn_region(team::ally).positions();
    long sx = 0, sy = 0;
    for (auto p : spawn) { sx += p.x; sy += p.y; }
    if (!spawn.empty()) {
        const vec2 mean{static_cast<int>(sx / static_cast<long>(spawn.size())),
                        static_cast<int>(sy / static_cast<long>(spawn.size()))};
        home_ = spawn[0];
        for (auto p : spawn)
            if (dist(p, mean) < dist(home_, mean)) home_ = p;
    }
    ready_ = true;
}

void Bot::shop() {
    auto& me = bc::self();
    using it = bc::item_type;
    for (int n = 0; n < 12; ++n) {
        it item = it::power_shard_of_strength;
        const int k = bought_;
        if (ranged_) {
            if (k < 5) item = it::monster_shard_of_strength;
            else if (k < 10) item = it::power_shard_of_strength;
            else item = (k % 2) ? it::power_shard_of_health : it::power_shard_of_strength;
        } else {
            if (k < 5) item = it::jungle_shard_of_health;
            else if (k < 10) item = it::monster_shard_of_strength;
            else item = (k % 2) ? it::power_shard_of_health : it::power_shard_of_strength;
        }
        if (me.gold_count() < item.cost() || !me.can_buy(item)) return;
        me.buy(item);
        ++bought_;
    }
}

void Bot::write_status(bool alive, bool near, bool busy) {
    if (slot_ >= kSlots) return;
    std::uint8_t v = 0x01;
    if (alive) {
        auto& me = bc::self();
        const float frac = me.max_health() > 0 ? me.health() / me.max_health() : 0.f;
        v = static_cast<std::uint8_t>(kAlive | (near ? kNear : 0) | (busy ? kBusy : 0)
                                      | std::clamp(static_cast<int>(frac * 31.f), 0, 31));
    }
    mem_[kStatus + slot_] = v;
    bc::shared_memory[kStatus + slot_] = v;
}

// Refresh remembered state of visible groups (all of them, or just the pack target).
void Bot::scan_groups(bool all) {
    const auto visible = bc::vision::visible_tiles();
    const int target = mem_[0] - 1;
    for (int i = 0; i < static_cast<int>(groups_.size()); ++i) {
        if (!all && i != target) continue;
        auto& G = groups_[i];
        bool seen = false;
        for (auto p : G.tiles)
            if (visible.is_set(p)) { seen = true; break; }
        if (!seen) continue;
        try {
            const auto st = handles_[i].state();
            G.last_seen = round_;
            if (std::holds_alternative<bc::map::camp_group_reward_available>(st)) {
                G.reward_ready = true;
                G.respawn_at = 0;
            } else if (const auto* r = std::get_if<bc::map::camp_group_respawning>(&st)) {
                G.reward_ready = false;
                G.respawn_at = round_ + std::max(1, r->timer);
                G.known_hp = -1;
            } else {
                G.reward_ready = false;
                G.respawn_at = 0;
                if (G.boss) {
                    float hp = 0;
                    bool any = false;
                    for (auto p : G.tiles)
                        if (visible.is_set(p) && bc::vision::has_camp(p)) {
                            hp += bc::vision::camp_at(p).health();
                            any = true;
                        }
                    if (any) G.known_hp = hp;
                }
            }
        } catch (const std::exception&) {
        }
    }
}

// Leader: choose the pack target and the boss engage flag.
void Bot::lead() {
    auto& me = bc::self();
    const vec2 pos = me.position();
    int avail = 0;
    int near = 0;
    for (int s = 0; s < std::min(team_n_, kSlots); ++s) {
        const std::uint8_t v = mem_[kStatus + s];
        if (v == 0 || ((v & kAlive) && !(v & kBusy))) ++avail;
        if ((v & kAlive) && (v & kNear)) ++near;
    }
    avail = std::max(1, avail);
    const float dps_one = std::max(30.f, me.strength()) * 1.25f;
    const float dps = dps_one * static_cast<float>(avail);
    const float budget = static_cast<float>(avail) * me.max_health() * 0.55f;
    const float gold_w = round_ < 900 ? 0.5f : 0.25f;
    const int cur = mem_[0] - 1;

    int best = -1;
    float best_score = 0;
    for (int i = 0; i < static_cast<int>(groups_.size()); ++i) {
        const auto& G = groups_[i];
        if (G.tiles.empty()) continue;
        const int travel = dist(pos, G.center);
        const int wait = G.respawn_at > round_ ? G.respawn_at - round_ : 0;
        const int eff_wait = std::max(0, wait - travel);
        if (eff_wait > 50) continue;
        float clear = 0, trip_cost = 0;
        if (!G.reward_ready) {
            const float hp = (G.boss && G.known_hp >= 0) ? G.known_hp : G.hp;
            clear = hp / dps;
            const float incoming = G.str * clear * (G.boss ? 0.85f : 0.5f);
            const float trips = incoming / budget;
            if (!G.boss && trips > 1.0f) continue;
            if (G.boss) {
                if (trips > 3.5f || avail < 2) continue;
                if (trips > 1.0f) trip_cost = std::ceil(trips - 1.0f) * (2.f * dist(home_, G.center) + 30.f);
            }
        }
        const float value = static_cast<float>(G.gems) + gold_w * static_cast<float>(G.gold);
        if (value <= 0) continue;
        float score = value / (static_cast<float>(travel + eff_wait) + clear + trip_cost + 8.f);
        if (i == cur) score *= G.boss ? 1.6f : 1.3f;
        if (score > best_score) { best_score = score; best = i; }
    }
    if (best != cur) {
        mem_[1] = 0;
        engage_since_ = -1;
        approach_ = 3;
    }
    mem_[0] = static_cast<std::uint8_t>(best + 1);
    if (best >= 0 && groups_[best].boss && !(mem_[1] & 1)) {
        if (near > 0 && engage_since_ < 0) engage_since_ = round_;
        if (near >= std::max(2, avail - 1) || (engage_since_ >= 0 && round_ - engage_since_ > 30))
            mem_[1] |= 1;
    }
    bc::shared_memory[0] = mem_[0];
    bc::shared_memory[1] = mem_[1];
}

bool Bot::step_to(const bc::bitboard& goals) {
    auto& me = bc::self();
    if (moved_ || move_failed_ || cast_excl_ || !me.can_move()) return false;
    const vec2 pos = me.position();
    const auto blocked = blocked_now(pos);
    auto g = goals;
    g &= ~blocked;
    if (g.is_set(pos) || g.is_empty()) return false;
    bc::distance_field field;
    bc::bitboard visited, dest;
    dest.clear();
    dest.set(pos);
    field.compute_to(blocked, g, dest, visited);
    const auto d = field.next_move_from(pos);
    if (!d) return false;
    const bool ok = me.move(*d);
    if (ok) moved_ = true;
    else move_failed_ = true;
    return ok;
}

// Step to the free neighbour that best increases distance from `from` (ties towards home).
bool Bot::step_away(vec2 from, int want) {
    auto& me = bc::self();
    if (moved_ || move_failed_ || cast_excl_ || !me.can_move()) return false;
    const vec2 pos = me.position();
    if (dist(pos, from) >= want) return false;
    const auto blocked = blocked_now(pos);
    const int w = bc::map::width(), h = bc::map::height();
    vec2 best = pos;
    int best_score = dist(pos, from) * 10 - dist(pos, home_);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const vec2 n{pos.x + dx, pos.y + dy};
            if ((dx == 0 && dy == 0) || n.x < 0 || n.y < 0 || n.x >= w || n.y >= h || blocked.is_set(n)) continue;
            const int s = dist(n, from) * 10 - dist(n, home_);
            if (s > best_score) { best_score = s; best = n; }
        }
    if (best == pos) return false;
    const bool ok = me.move(best - pos);
    if (ok) moved_ = true;
    else move_failed_ = true;
    return ok;
}

void Bot::after_attack_reset(vec2 p, bool camp) {
    auto& me = bc::self();
    if (!me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        if (me.can_attack()) return;
        try {
            if (camp) {
                if (!bc::vision::has_camp(p)) return;
                auto c = bc::vision::camp_at(p);
                if (c.health() <= 0) return;
                if (ab.use()) me.attack(c);
            } else {
                if (!bc::vision::has_champion(p)) return;
                auto e = bc::vision::champion_at(p);
                if (!e.is_enemy() || e.health() <= 0) return;
                if (ab.use()) me.attack(e);
            }
        } catch (const std::exception&) {
        }
        return;
    }
}

void Bot::cast_at_camp(vec2 p, bool boss) {
    auto& me = bc::self();
    if (!me.can_cast()) return;
    const int d = dist(me.position(), p);
    for (const auto& ab : me.abilities()) {
        if (!me.can_cast()) return;
        if (!ab.info() || !ab.is_ready()) continue;
        const bool excl = ab_excl(ab);
        if (excl && moved_) continue;
        const float r = ab_range(ab);
        try {
            if ((ab.is_damage() || (ab.is_cc() && boss)) && ab.can_target_camps() && d <= r) {
                auto c = bc::vision::camp_at(p);
                if (ab.use(c) && excl) cast_excl_ = true;
            } else if (ab.is_aoe_damage() && ab.can_target_ground() && d <= r) {
                if (ab.use(p) && excl) cast_excl_ = true;
            } else if (ab.is_cc() && ab.can_target_ground() && boss && d <= r) {
                if (ab.use(p) && excl) cast_excl_ = true;
            }
        } catch (const std::exception&) {
        }
    }
}

// Attack a visible camp at p if it is in range; casts first. Returns true if we acted on it.
bool Bot::attack_unit_camp(vec2 p) {
    auto& me = bc::self();
    if (!bc::vision::has_camp(p)) return false;
    const int d = dist(me.position(), p);
    const bool boss = [&] {
        try { auto c = bc::vision::camp_at(p); return c.is_monolith() || c.kind() == bc::camp_type::golem; }
        catch (const std::exception&) { return false; }
    }();
    if (ranged_ || d <= me.attack_range()) cast_at_camp(p, boss);
    if (d > me.attack_range() || !me.can_attack()) return false;
    try {
        if (!bc::vision::has_camp(p)) return true;
        auto c = bc::vision::camp_at(p);
        if (c.health() <= 0) return true;
        me.attack(c);
    } catch (const std::exception&) {
        return false;
    }
    after_attack_reset(p, true);
    return true;
}

// Sum of strengths of awake camps that can reach us.
float Bot::incoming_damage(bool& boss_awake) {
    auto& me = bc::self();
    const vec2 pos = me.position();
    boss_awake = false;
    bc::bitboard near;
    near.clear();
    box(near, pos, 5);
    near &= bc::vision::camp_tiles();
    float sum = 0;
    for (auto p : near.positions()) {
        try {
            auto c = bc::vision::camp_at(p);
            if (!c.is_awake()) continue;
            if (dist(pos, p) <= c.attack_range()) {
                sum += c.strength();
                if (c.is_monolith() || c.kind() == bc::camp_type::golem) boss_awake = true;
            }
        } catch (const std::exception&) {
        }
    }
    return sum;
}

void Bot::support_casts(float incoming) {
    auto& me = bc::self();
    if (!me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!me.can_cast()) return;
        if (!ab.info() || !ab.is_ready()) continue;
        if (ab_excl(ab) && moved_) continue;
        try {
            if (ab.is_shield() && ab.can_target_self() && incoming > 0) {
                ab.use();
            } else if (ab.is_heal() && ab.can_target_allies()) {
                bc::bitboard near;
                near.clear();
                box(near, me.position(), static_cast<int>(ab_range(ab)));
                near &= bc::vision::champion_tiles(team::ally);
                near.set(me.position(), false);
                float worst = 1e9f;
                vec2 tgt{-1, -1};
                for (auto p : near.positions()) {
                    auto a = bc::vision::champion_at(p);
                    const float missing = a.max_health() - a.health();
                    if (missing > ab.heal_amount() * 0.7f && a.health() < worst) { worst = a.health(); tgt = p; }
                }
                if (tgt.x >= 0) ab.use(bc::vision::champion_at(tgt));
            }
        } catch (const std::exception&) {
        }
    }
}

// Enemy champions: fight when we are not outnumbered, otherwise ask the caller to flee.
bool Bot::fight_enemies(bool& flee) {
    flee = false;
    auto& me = bc::self();
    const vec2 pos = me.position();
    bc::bitboard area;
    area.clear();
    box(area, pos, 6);
    auto enemies = area & bc::vision::champion_tiles(team::enemy);
    if (enemies.is_empty()) return false;
    auto allies = area & bc::vision::champion_tiles(team::ally);
    const int n_allies = static_cast<int>(allies.count());  // includes us
    vec2 weakest{-1, -1}, nearest{-1, -1};
    float weakest_hp = 1e9f;
    int n_enemies = 0;
    for (auto p : enemies.positions()) {
        try {
            auto e = bc::vision::champion_at(p);
            if (!e.is_alive()) continue;
            ++n_enemies;
            if (nearest.x < 0 || dist(pos, p) < dist(pos, nearest)) nearest = p;
            if (dist(pos, p) <= me.attack_range() && e.health() < weakest_hp) { weakest_hp = e.health(); weakest = p; }
        } catch (const std::exception&) {
        }
    }
    if (n_enemies == 0) return false;
    const bool favorable = n_allies >= n_enemies;
    if (!favorable && me.health() < me.max_health() * 0.6f) {
        flee = true;
        return false;
    }
    if (weakest.x >= 0) {
        try {
            auto e = bc::vision::champion_at(weakest);
            if (me.can_cast())
                for (const auto& ab : me.abilities()) {
                    if (!me.can_cast()) break;
                    if (!ab.info() || !ab.is_ready() || (ab_excl(ab) && moved_)) continue;
                    const float r = ab_range(ab);
                    const int d = dist(pos, weakest);
                    if ((ab.is_damage() || ab.is_cc()) && ab.can_target_enemies() && d <= r) {
                        if (ab.use(e) && ab_excl(ab)) cast_excl_ = true;
                    } else if (ab.is_aoe_damage() && ab.can_target_ground() && d <= r) {
                        if (ab.use(weakest) && ab_excl(ab)) cast_excl_ = true;
                    } else if (ab.is_cc() && ab.can_target_ground() && d <= r) {
                        if (ab.use(weakest) && ab_excl(ab)) cast_excl_ = true;
                    }
                }
            if (me.can_attack() && bc::vision::has_champion(weakest)) {
                auto e2 = bc::vision::champion_at(weakest);
                if (e2.is_alive() && e2.health() > 0) {
                    me.attack(e2);
                    after_attack_reset(weakest, false);
                }
            }
        } catch (const std::exception&) {
        }
        return true;
    }
    if (favorable && nearest.x >= 0 && dist(pos, nearest) <= 3) {
        bc::bitboard g;
        g.clear();
        box(g, nearest, ranged_ ? 3 : 1);
        step_to(g);
        if (!ranged_ && me.can_attack() && dist(me.position(), nearest) <= me.attack_range()) {
            try {
                auto e = bc::vision::champion_at(nearest);
                if (e.is_enemy() && e.is_alive()) {
                    me.attack(e);
                    after_attack_reset(nearest, false);
                }
            } catch (const std::exception&) {
            }
        }
        return true;
    }
    return false;
}

bool Bot::bank() {
    auto& me = bc::self();
    const vec2 pos = me.position();
    const auto locked = bc::vision::altar_lock_tiles(team::enemy);
    bc::bitboard goals;
    goals.clear();
    for (auto a : altars_) {
        if (locked.is_set(a)) continue;
        if (dist(pos, a) <= bc::constants::submit_gems_range) {
            try {
                me.submit_gems(a);
                return true;
            } catch (const std::exception&) {
            }
            continue;
        }
        box(goals, a, 1);
    }
    if (goals.is_empty()) {
        // Every altar locked: tributing at one breaks its lock.
        for (auto a : altars_) {
            if (dist(pos, a) <= bc::constants::submit_gems_range) {
                try { me.submit_gems(a); return true; } catch (const std::exception&) {}
            }
            box(goals, a, 1);
        }
    }
    if (!step_to(goals) && !moved_ && !move_failed_ && !altars_.empty()) {
        vec2 best = altars_[0];
        for (auto a : altars_)
            if (dist(pos, a) < dist(pos, best)) best = a;
        try { if (me.move_towards(best)) moved_ = true; } catch (const std::exception&) {}
    }
    return true;
}

// Work the pack target: capture, wait for a respawn, stage for a boss, or attack.
bool Bot::work_target(int t, bool engage) {
    auto& me = bc::self();
    const vec2 pos = me.position();
    auto& G = groups_[t];
    const bool on_tile = std::find(G.tiles.begin(), G.tiles.end(), pos) != G.tiles.end();
    if (G.reward_ready) {
        if (on_tile) return true;
        bc::bitboard g;
        g.clear();
        for (auto p : G.tiles) g.set(p);
        if (!step_to(g) && !moved_ && !move_failed_) {
            try { if (me.move_towards(G.center)) moved_ = true; } catch (const std::exception&) {}
        }
        return true;
    }
    if (G.respawn_at > round_) {
        if (on_tile) {
            step_away(G.center, 2);
            return true;
        }
        if (dist(pos, G.center) > 4) {
            bc::bitboard g;
            g.clear();
            box(g, G.center, 3);
            for (auto p : G.tiles) g.set(p, false);
            step_to(g);
        }
        return true;
    }
    // Alive (or unknown): find the camp to hit.
    const auto visible = bc::vision::visible_tiles();
    vec2 tgt{-1, -1};
    float tgt_hp = 1e9f;
    bool boss_tile = false, boss_awake = false;
    vec2 boss_pos{-1, -1};
    int in_vision = 0;
    for (auto p : G.tiles) {
        if (!visible.is_set(p)) continue;
        ++in_vision;
        if (!bc::vision::has_camp(p)) continue;
        try {
            auto c = bc::vision::camp_at(p);
            if (!c.is_alive()) continue;
            const float hp = c.health();
            const bool b = c.is_monolith() || c.kind() == bc::camp_type::golem;
            if (b) {
                boss_pos = p;
                if (c.is_awake()) boss_awake = true;
            }
            // Prefer camps in range, then the weakest (clusters die in one or two hits), then the nearest.
            const float key = hp + 400.f * static_cast<float>(std::max(0, dist(pos, p) - me.attack_range()));
            if (key < tgt_hp) { tgt_hp = key; tgt = p; boss_tile = b; }
        } catch (const std::exception&) {
        }
    }
    if (tgt.x < 0) {
        if (in_vision == static_cast<int>(G.tiles.size()) && in_vision > 0) {
            // Everything dead but no reward state seen yet: stand on a tile.
            G.reward_ready = true;
            return work_target(t, engage);
        }
        if (dist(pos, G.center) > 1) {
            bc::bitboard g;
            g.clear();
            box(g, G.center, 2);
            if (!step_to(g) && !moved_ && !move_failed_) {
                try { if (me.move_towards(G.center)) moved_ = true; } catch (const std::exception&) {}
            }
        }
        return true;
    }
    // Boss staging: hold outside its range until the leader calls the engage.
    if (G.boss && boss_pos.x >= 0 && !boss_awake && !engage) {
        const int d = dist(pos, boss_pos);
        if (d <= G.range) step_away(boss_pos, G.range + 1);
        else if (d > G.range + 3) {
            bc::bitboard g;
            g.clear();
            box(g, boss_pos, G.range + 2);
            bc::bitboard inner;
            inner.clear();
            box(inner, boss_pos, G.range);
            g &= ~inner;
            step_to(g);
        }
        return true;
    }
    (void)boss_tile;
    if (attack_unit_camp(tgt)) return true;
    // Approach.
    const int d = dist(pos, tgt);
    if (ranged_) {
        if (d <= approach_ && ++no_hit_turns_ > 2) {
            approach_ = std::max(1, approach_ - 1);
            no_hit_turns_ = 0;
        }
    }
    bc::bitboard g;
    g.clear();
    box(g, tgt, ranged_ ? approach_ : 1);
    if (!step_to(g) && !moved_ && !move_failed_) {
        try { if (me.move_towards(tgt)) moved_ = true; } catch (const std::exception&) {}
    }
    if (moved_ && !ranged_) attack_unit_camp(tgt);
    return true;
}

void Bot::play_turn() {
    if (!ready_) init();
    auto& me = bc::self();
    round_ = static_cast<int>(bc::round_number());
    moved_ = move_failed_ = cast_excl_ = false;
    for (int i = 0; i < 8; ++i) mem_[i] = bc::shared_memory[i];
    me.set_priority(20 - slot_);

    if (!me.is_alive()) {
        shop();
        write_status(false, false, false);
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        write_status(true, false, true);
        return;
    }
    const vec2 pos = me.position();
    const bool at_spawn = bc::map::spawn_region(team::ally).is_set(pos);
    if (at_spawn) {
        shop();
        if (me.health() < me.max_health() * 0.9f) {
            write_status(true, false, true);
            return;
        }
    }

    // Leader election: lowest slot that is alive and not busy (unknown counts as available).
    int leader = -1;
    for (int s = 0; s < std::min(team_n_, kSlots); ++s) {
        const std::uint8_t v = mem_[kStatus + s];
        if (s == slot_ || v == 0 || ((v & kAlive) && !(v & kBusy))) { leader = s; break; }
    }
    if (leader == slot_) {
        if (round_ - last_scan_ >= 3) {
            scan_groups(true);
            last_scan_ = round_;
        } else {
            scan_groups(false);
        }
        lead();
    } else {
        scan_groups(false);
    }
    int target = mem_[0] - 1;
    if (target >= static_cast<int>(groups_.size())) target = -1;
    if (target != last_target_) {
        last_target_ = target;
        approach_ = 3;
        no_hit_turns_ = 0;
    }
    const bool engage = (mem_[1] & 1) != 0;

    bool boss_awake = false;
    const float incoming = incoming_damage(boss_awake);
    const float hp = me.health(), mh = me.max_health();
    const int gems = static_cast<int>(me.gem_count());
    bool near = false;
    if (target >= 0) near = dist(pos, groups_[target].center) <= groups_[target].range + 3;

    support_casts(incoming);

    // Retreat from camp damage when it would kill us soon.
    if (incoming > 0 && hp < std::max(0.3f * mh, 2.5f * incoming)) {
        vec2 from = pos;
        bc::bitboard area;
        area.clear();
        box(area, pos, 5);
        area &= bc::vision::camp_tiles();
        int best = 99;
        for (auto p : area.positions())
            if (dist(pos, p) < best) { best = dist(pos, p); from = p; }
        step_away(from, 7);
        write_status(true, false, true);
        return;
    }

    bool flee = false;
    if (fight_enemies(flee)) {
        write_status(true, near, false);
        return;
    }
    if (flee) {
        bc::bitboard area;
        area.clear();
        box(area, pos, 6);
        area &= bc::vision::champion_tiles(team::enemy);
        vec2 from = pos;
        int best = 99;
        for (auto p : area.positions())
            if (dist(pos, p) < best) { best = dist(pos, p); from = p; }
        if (!step_away(from, 9) && incoming == 0 && !moved_) me.recall();
        write_status(true, false, true);
        return;
    }

    const bool capturing = target >= 0 && groups_[target].reward_ready
        && std::find(groups_[target].tiles.begin(), groups_[target].tiles.end(), pos) != groups_[target].tiles.end();
    const bool boss_target = target >= 0 && groups_[target].boss;

    // Heal: low health, or entering a boss fight below 75%.
    if (incoming == 0 && !capturing && !at_spawn
        && (hp < 0.4f * mh || (boss_target && !boss_awake && hp < 0.75f * mh && !near)
            || (me.gold_count() >= 300 && !near && dist(pos, home_) > 10))) {
        if (!moved_) {
            me.recall();
            write_status(true, false, true);
            return;
        }
    }

    // Bank gems.
    int altar_d = 999;
    for (auto a : altars_) altar_d = std::min(altar_d, dist(pos, a));
    const int total = static_cast<int>(bc::global_submitted_gems(team::ally));
    const bool want_bank = gems > 0
        && (gems >= 30 || (gems >= 10 && altar_d <= 6) || altar_d <= 2 || round_ > 1850
            || total + gems >= 1000 || (boss_target && (gems >= 5) && !(near && engage)));
    if (want_bank && !capturing && incoming == 0) {
        bank();
        write_status(true, false, true);
        return;
    }

    if (target >= 0) work_target(target, engage || boss_awake);
    write_status(true, near, false);
}
