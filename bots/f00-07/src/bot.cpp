// Vision-ambush champion: farm camps, pass enemy sightings through shared memory, gang up on isolated
// enemies (above all ones channelling a tribute or a capture), and ward the approaches we farm.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

const char* g_phase = "start";

using cambc::bitboard;
using cambc::team;
using cambc::vec2;
namespace V = cambc::vision;
namespace M = cambc::map;

namespace {

constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1}, vec2{0, -1}, vec2{-1, -1},
    vec2{-1, 0}, vec2{-1, 1}};

// Shared memory layout (8 bytes):
//   0..2 enemy sighting x, y, round & 0xff     3 flags: bits 0-2 enemy count, bit 3 weak/isolated, bit 4 at altar
//   4..6 call to arms x, y, round & 0xff        7 team camp group index (0xff none)
float kTeamBonus = 2.5f;  // set per map in init_static
constexpr float kHealRecall = 0.5f;
constexpr float kGoldWeight = 0.3f;
constexpr float kEngage = 0.9f;
constexpr int kSightX = 0, kSightY = 1, kSightR = 2, kSightF = 3, kCallX = 4, kCallY = 5, kCallR = 6, kTeamGroup = 7;

int W = 0, H = 0, ROUND = 0;

int dist(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
bool inb(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < W && p.y < H; }

void square(bitboard& b, vec2 c, int r) {
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            const vec2 p{c.x + dx, c.y + dy};
            if (inb(p)) b.set(p);
        }
}

struct Field {
    std::unique_ptr<cambc::distance_field> f;
    bitboard reach;
    bool ready = false;
    int get(vec2 p) const { return ready && reach.is_set(p) ? f->get(p) : -1; }
};

struct Group {
    int index = 0;
    std::optional<M::camp_group> g;
    bitboard spawns;
    bitboard near;  // tiles within 2 of a spawn tile, minus the spawns: waiting spots
    vec2 center{};
    int gems = 0, gold = 0, respawn = 0;
    float hp = 0, str = 0;
    bool boss = false;
    int ready_round = 0;       // estimated round when the group is alive or claimable again
    int enemy_seen = -1000;    // last round an enemy champion was seen near the group
    int ban_until = 0;         // unreachable for now (blocked path)
    int capture_turns = 0;     // turns spent standing on a claimable group (contested if this grows)
    Field field;
};

struct Seen {
    cambc::champion h;
    vec2 p;
    float hp, maxhp, str, focus;
    int range;
    bool stunned;
};

// ---- static map knowledge -----------------------------------------------------------------------
bool g_init = false;
bitboard g_static_walls;   // walls | holes | altars | enemy spawn
bitboard g_bushes;
bitboard g_enemy_spawn_zone;  // enemy spawn tiles and everything within 3 of them
std::vector<Group> g_groups;
std::vector<vec2> g_altars;
vec2 g_my_spawn{}, g_enemy_spawn{};
Field g_spawn_field, g_altar_field;
std::vector<int> g_altar_enemy_seen;
std::vector<vec2> g_altar_post;  // ambush post next to each altar (bush preferred)

// ---- per-turn state -----------------------------------------------------------------------------
bool g_moved = false, g_stop_moving = false, g_attacked = false, g_cast = false;
std::vector<std::pair<vec2, int>> g_bad_steps;
std::vector<Seen> g_enemies, g_allies;
int g_target_group = -1;
int g_item_step = 0;
vec2 g_last_pos{-1, -1};
int g_progress_round = 0;  // last round our income rose or our target changed
int g_last_income = 0;
int g_last_target = -2;
int g_still = 0;           // consecutive turns without changing tile

// Living camps only: camp_tiles() also reports dead camps awaiting capture or respawn.
bitboard live_camps() { return V::awake_camp_tiles() | V::asleep_camp_tiles(); }

float dps_of(float str, float focus) { return str + 0.3f * focus; }

void init_static() {
    g_phase = "init_static";
    W = M::width();
    H = M::height();
    // Small maps: farm as a pack (fights are frequent); large maps: spread out to farm in parallel.
    kTeamBonus = W * H <= 2500 ? 2.5f : 1.2f;
    g_static_walls = M::walls() | M::holes() | M::altars() | M::spawn_region(team::enemy);
    g_bushes = M::bushes();
    for (auto p : M::spawn_region(team::enemy).positions()) square(g_enemy_spawn_zone, p, 3);
    {
        const auto mine = M::spawn_region(team::ally).positions();
        const auto theirs = M::spawn_region(team::enemy).positions();
        long sx = 0, sy = 0;
        for (auto p : mine) sx += p.x, sy += p.y;
        if (!mine.empty()) g_my_spawn = {static_cast<int>(sx / (long)mine.size()), static_cast<int>(sy / (long)mine.size())};
        sx = sy = 0;
        for (auto p : theirs) sx += p.x, sy += p.y;
        if (!theirs.empty()) g_enemy_spawn = {static_cast<int>(sx / (long)theirs.size()), static_cast<int>(sy / (long)theirs.size())};
    }
    int idx = 0;
    for (const auto& grp : M::camp_groups()) {
        Group G;
        G.index = idx++;
        G.g.emplace(grp);
        G.spawns = grp.spawn_tiles();
        G.gems = static_cast<int>(grp.gem_value());
        G.gold = static_cast<int>(grp.gold_value());
        G.respawn = grp.max_respawn_timer();
        long sx = 0, sy = 0, n = 0;
        for (const auto& c : grp.camps()) {
            const auto st = c.base_stats();
            G.hp += st.base_health;
            G.str += st.base_strength;
            if (c.kind() == cambc::camp_type::monolith) G.boss = true;
            const auto p = c.spawn_position();
            sx += p.x, sy += p.y, ++n;
            square(G.near, p, 2);
        }
        G.near &= ~G.spawns;
        G.near &= ~g_static_walls;
        if (n) G.center = {static_cast<int>(sx / n), static_cast<int>(sy / n)};
        if (cambc::self().position().x >= 0 && idx <= 30)
            std::cout << "grp " << G.index << " c " << G.center.x << "," << G.center.y << " gems " << G.gems << " gold "
                      << G.gold << " hp " << G.hp << " str " << G.str << " resp " << G.respawn << "\n";
        g_groups.push_back(std::move(G));
    }
    g_altars = M::altars().positions();
    g_altar_enemy_seen.assign(g_altars.size(), -1000);
    for (auto a : g_altars) {
        // Ambush post: a bush within 4 of the altar, nearest to it; otherwise a free tile at distance 3.
        vec2 best = a;
        int bestScore = 1 << 30;
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const vec2 p{a.x + dx, a.y + dy};
                if (!inb(p) || g_static_walls.is_set(p) || M::spawn_region(team::ally).is_set(p)) continue;
                const int d = dist(p, a);
                if (d < 2) continue;
                const int score = (g_bushes.is_set(p) ? 0 : 100) + std::abs(d - 3) * 5 + dist(p, g_my_spawn) / 4;
                if (score < bestScore) bestScore = score, best = p;
            }
        g_altar_post.push_back(best);
    }
    g_init = true;
}

bool build_field(Field& F, const bitboard& roots) {
    g_phase = "build_field";
    if (F.ready) return true;
    if (cambc::fuel_remaining() < static_cast<std::uint64_t>(W * H * 3 + 15000)) return false;
    if (!F.f) F.f = std::make_unique<cambc::distance_field>();
    const auto before = cambc::fuel_remaining();
    F.f->compute(g_static_walls & ~roots, roots, F.reach);
    std::cout << "field cost " << (before - cambc::fuel_remaining()) << " area " << W * H << "\n";
    F.ready = true;
    return true;
}

bool ensure_spawn_field() { return build_field(g_spawn_field, M::spawn_region(team::ally)); }
bool ensure_altar_field() {
    if (g_altar_field.ready) return true;
    bitboard roots;
    for (auto a : g_altars) square(roots, a, cambc::constants::submit_gems_range);
    roots &= ~g_static_walls;
    return build_field(g_altar_field, roots);
}
bool ensure_group_field(Group& G) { return build_field(G.field, G.spawns); }

// ---- movement -----------------------------------------------------------------------------------
bitboard dynamic_blocked() {
    auto b = g_static_walls | V::champion_tiles() | live_camps() | V::teleport_tiles();
    for (const auto& [p, r] : g_bad_steps)
        if (r >= ROUND && inb(p)) b.set(p);
    b.set(cambc::self().position(), false);
    return b;
}

float enemy_pressure(vec2 p) {
    float s = 0;
    for (const auto& e : g_enemies) {
        const int d = dist(p, e.p);
        const int reach = e.range + 1;
        if (d <= reach) s += (reach + 1 - d) * dps_of(e.str, e.focus);
    }
    return s;
}

bool do_move(vec2 to) {
    auto& me = cambc::self();
    if (g_stop_moving || g_moved || !me.can_move()) return false;
    if (dist(to, me.position()) != 1 || !inb(to)) return false;
    if (V::has_champion(to)) return false;
    bool ok = false;
    try {
        ok = me.move_absolute(to);
    } catch (const cambc::error& e) {
        // A unit hidden in a bush can make the engine report the tile as known-occupied; treat it as blocked.
        (void)e;
        g_bad_steps.emplace_back(to, ROUND + 3);
        g_stop_moving = true;
        return false;
    }
    if (!ok) {
        g_bad_steps.emplace_back(to, ROUND + 3);
        g_stop_moving = true;
    } else {
        g_moved = true;
    }
    return ok;
}

// Step along a dynamic distance field computed toward `goals`, limited to max_d steps.
bool step_dynamic(bitboard goals, int max_d, bool avoid) {
    g_phase = "step_dynamic";
    auto& me = cambc::self();
    const vec2 pos = me.position();
    if (goals.is_set(pos)) return false;
    const auto blocked = dynamic_blocked();
    goals &= ~blocked | live_camps();
    if (goals.is_empty()) return false;
    bitboard dest;
    dest.set(pos);
    bitboard visited;
    cambc::distance_field f;
    f.compute_to(blocked & ~goals, goals, dest, visited, static_cast<std::uint16_t>(max_d));
    if (!visited.is_set(pos)) return false;
    const int cur = f.get(pos);
    vec2 best = pos;
    int bestv = cur;
    float bestp = 1e9f;
    for (auto d : kDirs) {
        const vec2 n = pos + d;
        if (!inb(n) || blocked.is_set(n) || !visited.is_set(n)) continue;
        const int v = f.get(n);
        const float pr = avoid ? enemy_pressure(n) : 0.f;
        if (v < bestv || (v == bestv && best != pos && pr < bestp)) best = n, bestv = v, bestp = pr;
    }
    if (best == pos) return false;
    return do_move(best);
}

// Follow a cached static field; detour locally when units block the way.
bool step_field(const Field& F, bool avoid) {
    g_phase = "step_field";
    auto& me = cambc::self();
    if (g_stop_moving || g_moved || !me.can_move()) return false;
    const vec2 pos = me.position();
    const int cur = F.get(pos);
    if (cur <= 0) return false;
    const auto blocked = dynamic_blocked();
    vec2 best = pos;
    int bestv = cur;
    float bestp = 1e9f;
    for (auto d : kDirs) {
        const vec2 n = pos + d;
        if (!inb(n) || blocked.is_set(n)) continue;
        const int v = F.get(n);
        if (v < 0) continue;
        const float pr = avoid ? enemy_pressure(n) : 0.f;
        if (v < bestv || (v == bestv && best != pos && pr < bestp)) best = n, bestv = v, bestp = pr;
    }
    if (best != pos) return do_move(best);
    // Blocked: route around via a local search to tiles a few steps closer.
    bitboard goals;
    for (int dy = -5; dy <= 5; ++dy)
        for (int dx = -5; dx <= 5; ++dx) {
            const vec2 p{pos.x + dx, pos.y + dy};
            if (!inb(p) || blocked.is_set(p)) continue;
            const int v = F.get(p);
            if (v >= 0 && v <= cur - 2) goals.set(p);
        }
    if (goals.is_empty()) return false;
    return step_dynamic(goals, 14, avoid);
}

// Greedy fallback when no field could be computed (fuel): reduce Chebyshev distance.
bool step_greedy(vec2 target) {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const auto blocked = dynamic_blocked();
    vec2 best = pos;
    int bestv = dist(pos, target);
    for (auto d : kDirs) {
        const vec2 n = pos + d;
        if (!inb(n) || blocked.is_set(n)) continue;
        const int v = dist(n, target);
        if (v < bestv) best = n, bestv = v;
    }
    return best != pos && do_move(best);
}

// Move toward a set of goal tiles: static field when available, else a bounded dynamic search.
bool go_to(Field* F, const bitboard& goals, vec2 fallback, bool avoid) {
    if (F && F->ready && F->get(cambc::self().position()) > 0) return step_field(*F, avoid);
    const int d = dist(cambc::self().position(), fallback);
    if (d <= 12 && cambc::fuel_remaining() > 20000) return step_dynamic(goals, 30, avoid);
    return step_greedy(fallback);
}

bool go_to_tile(vec2 t, bool avoid) {
    bitboard g;
    g.set(t);
    return go_to(nullptr, g, t, avoid);
}

// Step to the neighbour that minimises enemy pressure, tie-broken toward our spawn.
bool retreat() {
    g_phase = "retreat";
    auto& me = cambc::self();
    if (g_stop_moving || g_moved || !me.can_move()) return false;
    const vec2 pos = me.position();
    const auto blocked = dynamic_blocked();
    ensure_spawn_field();
    auto score = [&](vec2 p) {
        const int sd = g_spawn_field.get(p);
        return enemy_pressure(p) * 4.f + (sd >= 0 ? sd : dist(p, g_my_spawn)) * 1.0f;
    };
    vec2 best = pos;
    float bestv = score(pos);
    for (auto d : kDirs) {
        const vec2 n = pos + d;
        if (!inb(n) || blocked.is_set(n)) continue;
        const float v = score(n);
        if (v < bestv) best = n, bestv = v;
    }
    return best != pos && do_move(best);
}

// ---- perception ---------------------------------------------------------------------------------
void scan_units() {
    g_phase = "scan_units";
    g_enemies.clear();
    g_allies.clear();
    for (int side = 0; side < 2; ++side) {
        const team t = side == 0 ? team::enemy : team::ally;
        for (auto p : V::champion_tiles(t).positions()) {
            if (!V::has_champion(p)) continue;
            auto c = V::champion_at(p);
            if (!c.is_visible()) continue;
            bool stunned = false;
            if (t == team::enemy) {
                const auto buffs = c.buffs();
                stunned = buffs.has_stun();
            }
            Seen s{c, p, c.health(), c.max_health(), c.strength(), c.focus(), c.attack_range(), stunned};
            if (s.hp <= 0) continue;
            (t == team::enemy ? g_enemies : g_allies).push_back(std::move(s));
        }
    }
}

int round_age(int stored) {
    // Rounds since a stored (round & 0xff) stamp, assuming it is not older than 255 rounds.
    return (ROUND - stored) & 0xff;
}

void update_memory() {
    g_phase = "update_memory";
    const auto visible = V::visible_tiles();
    for (auto& G : g_groups) {
        if ((G.spawns & visible).is_empty()) continue;
        g_phase = "update_memory/state";
        const auto st = G.g->state();
        if (std::holds_alternative<M::camp_group_respawning>(st))
            G.ready_round = ROUND + std::get<M::camp_group_respawning>(st).timer;
        else
            G.ready_round = ROUND;
        for (const auto& e : g_enemies)
            if (dist(e.p, G.center) <= 4) G.enemy_seen = ROUND;
    }
    for (std::size_t i = 0; i < g_altars.size(); ++i)
        for (const auto& e : g_enemies)
            if (dist(e.p, g_altars[i]) <= 2) g_altar_enemy_seen[i] = ROUND;

    g_phase = "update_memory/read";
    // Read the shared sighting: an enemy seen by a teammate near a group or altar counts as usage.
    const int sr = cambc::shared_memory[kSightR];
    const int sx = cambc::shared_memory[kSightX];
    const int sy = cambc::shared_memory[kSightY];
    if (sx != 0xff && round_age(sr) <= 2) {
        const vec2 sp{sx, sy};
        for (std::size_t i = 0; i < g_altars.size(); ++i)
            if (dist(sp, g_altars[i]) <= 2) g_altar_enemy_seen[i] = std::max(g_altar_enemy_seen[i], ROUND - round_age(sr));
        for (auto& G : g_groups)
            if (dist(sp, G.center) <= 4) G.enemy_seen = std::max(G.enemy_seen, ROUND - round_age(sr));
    }

    g_phase = "update_memory/write";
    // Write our own sighting: the most vulnerable enemy we see.
    if (!g_enemies.empty()) {
        const Seen* best = nullptr;
        float bestv = 1e9f;
        for (const auto& e : g_enemies) {
            const float v = e.hp - (e.stunned ? 200.f : 0.f);
            if (v < bestv) bestv = v, best = &e;
        }
        int near = 0;
        for (const auto& e : g_enemies)
            if (dist(e.p, best->p) <= 4) ++near;
        bool at_altar = false;
        for (auto a : g_altars)
            if (dist(best->p, a) <= 1) at_altar = true;
        const bool weak = near <= 1 || best->hp < best->maxhp * 0.4f;
        cambc::shared_memory[kSightX] = static_cast<std::uint8_t>(best->p.x);
        cambc::shared_memory[kSightY] = static_cast<std::uint8_t>(best->p.y);
        cambc::shared_memory[kSightR] = static_cast<std::uint8_t>(ROUND & 0xff);
        cambc::shared_memory[kSightF] = static_cast<std::uint8_t>(std::min(near, 7) | (weak ? 8 : 0) | (at_altar ? 16 : 0));
    }
}

// ---- combat -------------------------------------------------------------------------------------
float local_balance(vec2 at, int radius_enemy, int radius_ally) {
    float adps = 0, ahp = 0, edps = 0, ehp = 0;
    for (const auto& a : g_allies)
        if (dist(a.p, at) <= radius_ally) adps += dps_of(a.str, a.focus), ahp += a.hp;
    for (const auto& e : g_enemies)
        if (dist(e.p, at) <= radius_enemy) {
            const float m = e.stunned ? 0.5f : 1.f;
            edps += dps_of(e.str, e.focus) * m, ehp += e.hp;
        }
    if (edps <= 0) return 10.f;
    return (adps * ahp) / std::max(1.f, edps * ehp);
}

template <class F> void for_ability(F&& f) {
    auto& me = cambc::self();
    for (const auto& ab : me.abilities()) f(ab);
}

bool ability_excludes_walking(const cambc::ability& ab) {
    const auto& info = ab.info();
    if (!info) return false;
    return std::visit([](const auto& v) { return v.exclusive_with_movement; }, *info);
}

bool can_use(const cambc::ability& ab) {
    auto& me = cambc::self();
    if (g_cast || !me.can_cast() || !ab.is_ready()) return false;
    if (g_moved && ability_excludes_walking(ab)) return false;
    return true;
}

// Cast offensive spells at an enemy champion (stun first when the target is not stunned).
vec2 g_spell_hit{-1, -1};  // champion tile our spell hit this turn (it may have died)

bool cast_at_enemy(const Seen& t) {
    g_phase = "cast_at_enemy";
    bool used = false;
    for_ability([&](const cambc::ability& ab) {
        if (used || !can_use(ab)) return;
        if (!(ab.is_damage() || ab.is_cc() || ab.is_aoe_damage())) return;
        if (ab.is_cc() && t.stunned) return;
        const auto tiles = ab.valid_tiles();
        if (!tiles.is_set(t.p)) return;
        bool ok = false;
        if (ab.can_target_enemies()) ok = ab.use(t.h);
        else if (ab.can_target_ground()) ok = ab.use(t.p);
        if (ok) used = true, g_cast = true, g_spell_hit = t.p;
    });
    return used;
}

bool cast_at_camp(vec2 p) {
    g_phase = "cast_at_camp";
    bool used = false;
    if (!V::has_camp(p)) return false;
    for_ability([&](const cambc::ability& ab) {
        if (used || !can_use(ab)) return;
        if (!(ab.is_damage() || ab.is_aoe_damage())) return;
        const auto tiles = ab.valid_tiles();
        if (!tiles.is_set(p)) return;
        bool ok = false;
        if (ab.can_target_camps()) {
            auto c = V::camp_at(p);
            if (!c.is_visible()) return;
            ok = ab.use(c);
        } else if (ab.can_target_ground()) {
            ok = ab.use(p);
        }
        if (ok) used = true, g_cast = true;
    });
    return used;
}

// Heal the most hurt visible ally, shield ourselves when enemies are close.
void cast_support() {
    g_phase = "cast_support";
    auto& me = cambc::self();
    for_ability([&](const cambc::ability& ab) {
        if (!can_use(ab)) return;
        if (ab.is_heal()) {
            const auto tiles = ab.valid_tiles();
            const Seen* best = nullptr;
            float worst = 1.f;
            for (const auto& a : g_allies) {
                if (a.p == me.position() || !tiles.is_set(a.p)) continue;
                const float r = a.hp / std::max(1.f, a.maxhp);
                if (r < worst && a.maxhp - a.hp > 150.f) worst = r, best = &a;
            }
            if (best && ab.use(best->h)) g_cast = true;
        } else if (ab.is_shield()) {
            bool threat = false;
            for (const auto& e : g_enemies)
                if (dist(e.p, me.position()) <= e.range + 1) threat = true;
            if (!threat && me.health() < me.max_health() * 0.8f && !V::awake_camp_tiles().is_empty()) {
                for (auto p : V::awake_camp_tiles().positions())
                    if (dist(p, me.position()) <= 4) threat = true;
            }
            if (threat && ab.use()) g_cast = true;
        }
    });
}

// Attack reset only right after our own basic attack this turn; re-check the same handle afterwards.
void try_reset_and_attack(float hp_before, const cambc::unit& target) {
    g_phase = "try_reset_and_attack";
    auto& me = cambc::self();
    if (!g_attacked || g_cast || me.can_attack() || !me.can_cast()) return;
    // The first hit may have killed the target.
    if (hp_before <= me.strength() * 2.5f) return;
    for_ability([&](const cambc::ability& ab) {
        if (g_cast || !ab.is_attack_reset() || !ab.is_ready()) return;
        if (!target.is_visible() || target.health() <= 0) return;
        // The engine can reject a reset ("an available attack cannot be reset") in states can_attack() does
        // not reveal; the action then has no effect, so treat the rejection as "not cast".
        bool reset = false;
        try {
            reset = ab.use();
        } catch (const cambc::error&) {
            reset = false;
        }
        if (reset) {
            g_cast = true;
            if (!me.can_attack() || !target.is_visible() || target.health() <= 0) return;
            if (!me.attack_tiles().is_set(target.position())) return;
            me.attack(target);
        }
    });
}

bool attack_enemy(const Seen& t) {
    auto& me = cambc::self();
    if (!me.can_attack() || g_attacked) return false;
    if (!me.attack_tiles().is_set(t.p)) return false;
    if (!V::has_champion(t.p)) return false;
    auto c = V::champion_at(t.p);
    if (!c.is_visible()) return false;
    me.attack(c);
    g_attacked = true;
    try_reset_and_attack(t.hp, c);
    return true;
}

const Seen* pick_target(vec2 from, int max_d) {
    const Seen* best = nullptr;
    float bestv = 1e9f;
    auto& me = cambc::self();
    const float my_dmg = std::max(1.f, me.strength());
    const int cx = cambc::shared_memory[kCallX];
    const int cy = cambc::shared_memory[kCallY];
    const bool call_fresh = cx != 0xff && round_age(cambc::shared_memory[kCallR]) <= 1;
    for (const auto& e : g_enemies) {
        const int d = dist(from, e.p);
        if (d > max_d) continue;
        bool at_altar = false;
        for (auto a : g_altars)
            if (dist(e.p, a) <= 1) at_altar = true;
        float v = e.hp / my_dmg + std::max(0, d - me.attack_range()) * 1.5f;
        if (e.stunned) v -= 3;
        if (at_altar) v -= 4;
        if (call_fresh && dist(e.p, vec2{cx, cy}) <= 1) v -= 3;  // focus the teammates' target
        if (v < bestv) bestv = v, best = &e;
    }
    return best;
}

void broadcast_call(vec2 p) {
    cambc::shared_memory[kCallX] = static_cast<std::uint8_t>(p.x);
    cambc::shared_memory[kCallY] = static_cast<std::uint8_t>(p.y);
    cambc::shared_memory[kCallR] = static_cast<std::uint8_t>(ROUND & 0xff);
}

// Returns true when the turn was spent on combat (fight or flight).
bool combat_inner();

// Enemies resting on their own spawn are not a fight we can take or need to flee: ignore them unless adjacent.
bool combat() {
    g_phase = "combat";
    if (g_enemies.empty()) return false;
    const vec2 pos = cambc::self().position();
    std::vector<Seen> all;
    all.swap(g_enemies);
    for (const auto& e : all)
        if (!g_enemy_spawn_zone.is_set(e.p) || dist(e.p, pos) <= 2) g_enemies.push_back(e);
    const bool r = combat_inner();
    g_enemies.swap(all);
    return r;
}

bool combat_inner() {
    auto& me = cambc::self();
    if (g_enemies.empty()) return false;
    const vec2 pos = me.position();
    const float hp_ratio = me.health() / std::max(1.f, me.max_health());

    int closest = 99;
    for (const auto& e : g_enemies) closest = std::min(closest, dist(e.p, pos));
    if (closest > 7) return false;

    const float bal = local_balance(pos, 6, 5);
    const Seen* t = pick_target(pos, 7);
    bool engage = bal >= kEngage && hp_ratio > 0.25f;
    if (t) {
        // Finish off a nearly dead enemy even when the fight is slightly unfavourable.
        if (t->hp < me.strength() * 2.5f && hp_ratio > 0.3f && bal >= 0.5f) engage = true;
        // Do not dive deep into the enemy spawn.
        if (g_enemy_spawn_zone.is_set(t->p)) engage = false;
    }
    if (engage && t) {
        broadcast_call(t->p);
        if (cast_at_enemy(*t)) {}
        const bool in_range = me.attack_tiles().is_set(t->p);
        if (in_range) {
            attack_enemy(*t);
            cast_support();
            // Melee can still reposition; ranged has spent its allowance.
            return true;
        }
        // Approach the target; ranged champions stop at range.
        bitboard goals;
        square(goals, t->p, me.attack_range());
        goals &= ~g_static_walls;
        if (step_dynamic(goals, 12, false)) {
            if (me.can_attack() && me.attack_tiles().is_set(t->p)) attack_enemy(*t);
            if (!g_cast) cast_at_enemy(*t);
        }
        cast_support();
        // Attack anything else in range if the main target is out of reach.
        if (me.can_attack() && !g_attacked) {
            for (const auto& e : g_enemies)
                if (me.attack_tiles().is_set(e.p)) {
                    attack_enemy(e);
                    break;
                }
        }
        return true;
    }
    // Unfavourable: hit something in range if we cannot get away anyway, then retreat.
    if (closest <= 3) {
        cast_support();
        if (t && me.attack_range() > 1 && me.attack_tiles().is_set(t->p) && hp_ratio > 0.5f && bal > 0.6f) {
            cast_at_enemy(*t);
            attack_enemy(*t);
            return true;
        }
        if (retreat()) return true;
        if (t) {
            cast_at_enemy(*t);
            attack_enemy(*t);
        }
        return true;
    }
    // Enemies at 4-7 and the fight is not ours: keep distance only if they are heading for us.
    if (bal < 0.6f) return retreat();
    return false;
}

// ---- items --------------------------------------------------------------------------------------
void buy_items() {
    g_phase = "buy_items";
    auto& me = cambc::self();
    const auto type = me.champion_name();
    using T = cambc::champion_type;
    const bool focus_main = type == T::stellar || type == T::prism || type == T::cassi || type == T::willow;
    using I = cambc::item_type;
    const I primary = focus_main ? I::power_shard_of_focus : I::power_shard_of_strength;
    // 5 primary, 5 durability health, 5 primary, then health and primary alternately.
    for (int guard = 0; guard < 20; ++guard) {
        const auto counts = me.item_count();
        I item = primary;
        const int step = g_item_step;
        if (step < 5) item = primary;
        else if (step < 10) item = I::durability_shard_of_health;
        else if (step < 15) item = primary;
        else if (step < 20) item = I::power_shard_of_health;
        else item = (step / 5) % 2 ? I::power_shard_of_health : primary;
        (void)counts;
        if (!me.can_buy(item, 1)) return;
        me.buy(item, 1);
        ++g_item_step;
    }
}

// ---- wards --------------------------------------------------------------------------------------
void place_ward(vec2 focus_point) {
    g_phase = "place_ward";
    auto& me = cambc::self();
    if (me.ward_cooldown() > 0) return;
    if (me.buffs().has_stun() || (!me.can_move() && !me.can_attack() && !me.can_cast())) return;
    const vec2 pos = me.position();
    for (auto w : me.ward_positions())
        if (dist(w, focus_point) <= 3) return;  // already watching this area
    const auto visible = V::visible_tiles();
    // Only tiles we can see: a teammate's ward hidden in a bush would make the placement throw.
    const auto tiles = me.ward_placement_tiles() & visible & ~V::ward_tiles(team::ally);
    vec2 best = pos;
    float bestv = -1e9f;
    const auto own = me.ward_positions();
    for (auto p : tiles.positions()) {
        if (g_static_walls.is_set(p)) continue;
        if (std::find(own.begin(), own.end(), p) != own.end() || V::has_ward(p)) continue;
        // Teammates' wards are invisible to us: each champion type owns its own tile class (13 types).
        if (((p.x + 5 * p.y) % 13 + 13) % 13 != static_cast<int>(me.champion_name())) continue;
        float v = 0;
        // Reward hidden bush tiles around the candidate and closeness to the enemy side.
        for (int dy = -4; dy <= 4; dy += 2)
            for (int dx = -4; dx <= 4; dx += 2) {
                const vec2 q{p.x + dx, p.y + dy};
                if (inb(q) && g_bushes.is_set(q) && !visible.is_set(q)) v += 2;
            }
        if (g_bushes.is_set(p)) v += 4;
        v -= dist(p, g_enemy_spawn) * 0.15f;
        v -= dist(p, focus_point) * 0.5f;
        if (v > bestv) bestv = v, best = p;
    }
    if (bestv > -1e8f) me.place_ward(best);
}

// ---- gems ---------------------------------------------------------------------------------------
bool altar_usable(vec2 a) {
    return !V::has_altar_lock(a) || V::altar_lock_tiles(team::ally).is_set(a);
}

bool bank_gems(bool urgent) {
    g_phase = "bank_gems";
    auto& me = cambc::self();
    const vec2 pos = me.position();
    for (auto a : g_altars) {
        if (dist(pos, a) > cambc::constants::submit_gems_range) continue;
        if (altar_usable(a)) {
            g_phase = "submit";
            me.submit_gems(a);
            return true;
        }
        // Break an enemy lock with basic attacks.
        if (me.can_attack()) {
            const auto lock = V::altar_lock_at(a);
            if (me.attack_tiles().is_set(a)) {
                me.attack(lock);
                g_attacked = true;
                return true;
            }
        }
    }
    ensure_altar_field();
    bitboard goals;
    for (auto a : g_altars) square(goals, a, cambc::constants::submit_gems_range);
    goals &= ~g_static_walls;
    vec2 nearest = g_altars.empty() ? pos : g_altars[0];
    for (auto a : g_altars)
        if (dist(a, pos) < dist(nearest, pos)) nearest = a;
    go_to(&g_altar_field, goals, nearest, urgent);
    return true;
}

// ---- farming ------------------------------------------------------------------------------------
int group_distance(Group& G, vec2 pos) {
    if (!G.field.ready) ensure_group_field(G);
    if (G.field.ready) {
        const int d = G.field.get(pos);
        return d >= 0 ? d : 100000;  // unreachable on foot (island)
    }
    return dist(pos, G.center) * 5 / 4 + 1;
}

int choose_group() {
    g_phase = "choose_group";
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const float my_dps = std::max(10.f, dps_of(me.strength(), me.focus()));
    int helpers = 0;
    for (const auto& a : g_allies)
        if (dist(a.p, pos) <= 6) ++helpers;  // includes me
    helpers = std::max(1, helpers);
    const int team_group = cambc::shared_memory[kTeamGroup];
    int best = -1;
    float bestv = 0;
    for (auto& G : g_groups) {
        if (G.ban_until > ROUND) continue;
        const int d = group_distance(G, pos);
        if (d >= 100000) continue;
        const int wait = std::max(0, G.ready_round - ROUND);
        if (wait > 30) continue;
        const float n = static_cast<float>(helpers) + (G.index == team_group ? 1.f : 0.f);
        const float clear = G.hp / (my_dps * n);
        // Damage the tank takes while clearing.
        const float dmg = G.str * clear * 0.8f;
        const float budget = me.health() * 0.85f;
        float value = G.gems + kGoldWeight * G.gold;
        if (G.boss) {
            // Monolith: only with a strong pack; its damage moves to the next champion as each one retreats.
            if (helpers < 3 || dmg > 0.6f * helpers * me.health()) continue;
        } else if (dmg > budget) {
            value *= std::max(0.2f, budget / dmg);
        }
        float score = value / (std::max(static_cast<float>(d), static_cast<float>(wait)) + clear + 8.f);
        if (G.index == team_group) score *= kTeamBonus;
        if (G.index == g_target_group) score *= 1.25f;  // hysteresis
        if (ROUND - G.enemy_seen < 20) score *= 0.75f;
        if (score > bestv) bestv = score, best = G.index;
    }
    return best;
}

bool attack_camp_in_range(const Group* G) {
    g_phase = "attack_camp_in_range";
    auto& me = cambc::self();
    if (!me.can_attack() || g_attacked) return false;
    const auto in_range = live_camps() & me.attack_tiles();
    if (in_range.is_empty()) return false;
    vec2 best{-1, -1};
    float besthp = 1e9f;
    for (auto p : in_range.positions()) {
        if (G && !G->spawns.is_set(p)) continue;
        auto c = V::camp_at(p);
        if (!c.is_visible()) continue;
        const float hp = c.health();
        if (hp <= 0) continue;
        if (hp < besthp) besthp = hp, best = p;
    }
    if (best.x < 0) return false;
    if (g_enemies.empty() && !g_cast) cast_at_camp(best);
    if (!me.can_attack() || !V::has_camp(best)) return true;
    auto c = V::camp_at(best);
    if (!c.is_visible() || c.health() <= 0) return true;
    me.attack(c);
    g_attacked = true;
    try_reset_and_attack(besthp, c);
    return true;
}

void farm() {
    g_phase = "farm";
    auto& me = cambc::self();
    const vec2 pos = me.position();
    g_target_group = choose_group();
    {
        // No income for a long time on the same target: something blocks it (contest, path); drop it for a while.
        const int income = static_cast<int>(me.gold_count() + me.gem_count()) + static_cast<int>(g_item_step) * 30;
        if (income > g_last_income || g_target_group != g_last_target) g_progress_round = ROUND;
        g_last_income = income;
        g_last_target = g_target_group;
        if (g_target_group >= 0 && ROUND - g_progress_round > 90) {
            g_groups[g_target_group].ban_until = ROUND + 100;
            g_progress_round = ROUND;
            g_target_group = choose_group();
            g_last_target = g_target_group;
        }
    }
    if (g_target_group < 0) {
        // Nothing worth farming: ambush at the altar the enemy uses most recently, else wait near spawn side.
        int best = -1, bestr = -1000;
        for (std::size_t i = 0; i < g_altars.size(); ++i)
            if (g_altar_enemy_seen[i] > bestr) bestr = g_altar_enemy_seen[i], best = static_cast<int>(i);
        if (best >= 0 && ROUND - bestr < 200) {
            go_to_tile(g_altar_post[best], true);
            place_ward(g_altars[best]);
        }
        return;
    }
    Group& G = g_groups[g_target_group];
    ensure_group_field(G);
    for (auto& O : g_groups)
        if (O.index != G.index) O.capture_turns = 0;
    // Advertise our target so teammates converge on it.
    const int team_group = cambc::shared_memory[kTeamGroup];
    if (team_group != G.index && dist(pos, G.center) <= 6) cambc::shared_memory[kTeamGroup] = static_cast<std::uint8_t>(G.index);

    const bool visible_group = !(G.spawns & V::visible_tiles()).is_empty();
    const auto alive = live_camps() & G.spawns;
    if (visible_group && alive.is_empty()) {
        const auto st = G.g->state();
        if (std::holds_alternative<M::camp_group_reward_available>(st)) {
            // Capture: stand on a free spawn tile.
            if (G.spawns.is_set(pos)) {
                // Capture takes 5 rounds; much longer means an enemy contests the region out of sight.
                if (++G.capture_turns > 9) {
                    G.capture_turns = 0;
                    G.ban_until = ROUND + 40;
                }
                place_ward(G.center);
                return;
            }
            bitboard goals = G.spawns & ~V::champion_tiles();
            if (!goals.is_empty()) step_dynamic(goals, 20, true);
            return;
        }
        if (std::holds_alternative<M::camp_group_respawning>(st)) {
            // Wait nearby in a bush if possible, never on spawn tiles (that would delay the respawn).
            const int timer = std::get<M::camp_group_respawning>(st).timer;
            if (G.spawns.is_set(pos) || timer > 12) {
                // Leave; choose_group will redirect next turn.
                if (G.spawns.is_set(pos)) step_dynamic(G.near, 6, false);
                return;
            }
            if (!G.near.is_set(pos) || !g_bushes.is_set(pos)) {
                bitboard goals = G.near & g_bushes;
                if (goals.is_empty()) goals = G.near;
                if (!goals.is_set(pos)) step_dynamic(goals, 10, false);
            }
            place_ward(G.center);
            return;
        }
    }
    // Alive (or unknown): approach and attack.
    if (attack_camp_in_range(&G)) {
        g_still = 0;
        // Melee may step toward a better camp after attacking; ranged must stay.
        place_ward(G.center);
        return;
    }
    if (!alive.is_empty()) {
        // Move into attack range of the nearest alive camp of the group.
        bitboard goals;
        for (auto p : alive.positions()) square(goals, p, me.attack_range());
        goals &= ~g_static_walls;
        const bool near_tgt = G.field.get(pos) >= 0 && G.field.get(pos) <= 14;
        if (!(near_tgt && step_dynamic(goals, 30, true)) && G.field.get(pos) > 0) go_to(&G.field, G.spawns, G.center, true);
        attack_camp_in_range(&G);
        return;
    }
    if (g_still >= 3 && dist(pos, G.center) > 3) {
        // Stuck on the way: a camp of another group blocks the path; clear it, else give up for a while.
        if (attack_camp_in_range(nullptr)) return;
        const auto blockers = live_camps();
        for (auto& O : g_groups) {
            if (O.index == G.index) continue;
            bool close = false;
            for (auto p : (blockers & O.spawns).positions())
                if (dist(p, pos) <= 2) close = true;
            if (close) {
                g_target_group = O.index;
                bitboard goals;
                for (auto p : (blockers & O.spawns).positions()) square(goals, p, me.attack_range());
                goals &= ~g_static_walls;
                step_dynamic(goals, 10, false);
                return;
            }
        }
        if (cambc::fuel_remaining() > 40000 && step_dynamic(G.spawns, 40, false)) return;
        G.ban_until = ROUND + 40;
        return;
    }
    go_to(&G.field, G.spawns, G.center, true);
    attack_camp_in_range(&G);
}

// Answer a teammate's call to arms or a weak-enemy sighting nearby.
bool respond_to_call() {
    g_phase = "respond_to_call";
    auto& me = cambc::self();
    const vec2 pos = me.position();
    if (me.health() < me.max_health() * 0.55f || me.gem_count() >= 25) return false;
    const int cx = cambc::shared_memory[kCallX];
    const int cy = cambc::shared_memory[kCallY];
    const int cr = cambc::shared_memory[kCallR];
    if (cx != 0xff && round_age(cr) <= 3) {
        const vec2 c{cx, cy};
        const int d = dist(c, pos);
        if (d > 2 && d <= 14 && dist(c, g_enemy_spawn) > 6) {
            bitboard goals;
            square(goals, c, 2);
            goals &= ~g_static_walls;
            return step_dynamic(goals, 24, false) || go_to_tile(c, false);
        }
    }
    const int sx = cambc::shared_memory[kSightX];
    const int sy = cambc::shared_memory[kSightY];
    const int sr = cambc::shared_memory[kSightR];
    const int sf = cambc::shared_memory[kSightF];
    // An enemy tributing at an altar is channelling with a full bag: the best ambush there is.
    if (sx != 0xff && round_age(sr) <= 2 && (sf & 16) && (sf & 7) <= 2) {
        const vec2 s{sx, sy};
        const int d = dist(s, pos);
        if (d > 2 && d <= 16 && !g_enemy_spawn_zone.is_set(s)) {
            bitboard goals;
            square(goals, s, 2);
            goals &= ~g_static_walls;
            if (step_dynamic(goals, 24, false)) return true;
        }
    }
    if (sx != 0xff && round_age(sr) <= 2 && (sf & 8) && (sf & 7) <= 1) {
        const vec2 s{sx, sy};
        const int d = dist(s, pos);
        if (d > 2 && d <= 10 && dist(s, g_enemy_spawn) > 8) {
            bitboard goals;
            square(goals, s, 2);
            goals &= ~g_static_walls;
            return step_dynamic(goals, 18, false);
        }
    }
    return false;
}

void init_shared_memory_once() {
    // Round 1: mark beacons empty. Any champion may do it; later writes overwrite harmlessly.
    if (ROUND <= 1) {
        cambc::shared_memory[kSightX] = 0xff;
        cambc::shared_memory[kCallX] = 0xff;
        cambc::shared_memory[kTeamGroup] = 0xff;
    }
}

}  // namespace

struct TurnTrace {
    ~TurnTrace() {
        if (ROUND >= 1200 && ROUND <= 1212 && cambc::self().is_alive())
            std::cout << "trace r" << ROUND << " " << static_cast<int>(cambc::self().champion_name()) << " pos "
                      << cambc::self().position().x << "," << cambc::self().position().y << " phase " << g_phase
                      << " tgt " << g_target_group << " enemies " << g_enemies.size() << "\n";
    }
};

void Bot::play_turn() {
    TurnTrace trace_guard;
    g_phase = "play_turn";
    ROUND = static_cast<int>(cambc::round_number());
    if (!g_init) init_static();
    init_shared_memory_once();
    g_moved = g_stop_moving = g_attacked = g_cast = false;
    g_spell_hit = {-1, -1};
    if (g_bad_steps.size() > 16)
        g_bad_steps.erase(std::remove_if(g_bad_steps.begin(), g_bad_steps.end(),
            [](const auto& b) { return b.second < ROUND; }), g_bad_steps.end());

    auto& me = cambc::self();
    if (!me.is_alive()) {
        buy_items();
        return;
    }
    const bool at_spawn = M::spawn_region(team::ally).is_set(me.position());
    if (at_spawn) buy_items();
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        // Cancel a recall if an enemy is about to interrupt it anyway and we can fight back.
        return;
    }
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) return;

    scan_units();
    update_memory();
    const vec2 pos = me.position();
    if (pos == g_last_pos) ++g_still;
    else g_still = 0;
    g_last_pos = pos;
    const float hp = me.health(), maxhp = std::max(1.f, me.max_health());

    // Heal up on spawn before leaving.
    if (at_spawn && hp < maxhp * 0.9f && g_enemies.empty()) {
        cast_support();
        return;
    }

    // Low health: retreat from enemies, recall when nobody is near.
    if (!at_spawn && hp < maxhp * 0.35f) {
        int closest = 99;
        for (const auto& e : g_enemies) closest = std::min(closest, dist(e.p, pos));
        bool camp_threat = false;
        for (auto p : V::awake_camp_tiles().positions())
            if (dist(p, pos) <= 4) camp_threat = true;
        g_phase = "low_hp";
        if (closest > 5 && !camp_threat) {
            me.recall();
            return;
        }
        // Gems in the bag next to an altar: bank them before they are stolen.
        if (me.gem_count() > 0) {
            for (auto a : g_altars)
                if (dist(pos, a) <= cambc::constants::submit_gems_range && altar_usable(a)) {
                    me.submit_gems(a);
                    return;
                }
        }
        cast_support();
        if (!retreat()) {
            if (camp_threat) step_greedy(g_my_spawn);
        }
        return;
    }

    g_phase = "pre_combat";
    if (combat()) return;
    cast_support();

    // Shopping trip: plenty of gold, nothing to lose, nobody around.
    if (!at_spawn && g_enemies.empty() && me.gem_count() < 8) {
        bool camp_threat = false;
        for (auto p : V::awake_camp_tiles().positions())
            if (dist(p, pos) <= 5) camp_threat = true;
        const int gold = static_cast<int>(me.gold_count());
        if (!camp_threat && (gold >= 400 || (gold >= 150 && hp < maxhp * 0.6f) || hp < maxhp * kHealRecall)) {
            me.recall();
            return;
        }
    }
    g_phase = "post_combat";

    // Bank gems.
    const int gems = static_cast<int>(me.gem_count());
    if (gems > 0) {
        int altar_d = 1 << 20;
        for (auto a : g_altars) altar_d = std::min(altar_d, dist(a, pos));
        const bool late = ROUND >= 1880;
        const bool threatened = !g_enemies.empty();
        if (gems >= 35 || (gems >= 12 && altar_d <= 6) || late || (threatened && gems >= 10)) {
            bank_gems(threatened);
            return;
        }
    }

    if (respond_to_call()) return;
    farm();
    if (ROUND % 100 == 0)
        std::cout << "dbg r" << ROUND << " pos " << me.position().x << "," << me.position().y << " hp " << me.health()
                  << " gems " << me.gem_count() << " gold " << me.gold_count() << " tgt " << g_target_group
                  << " fuel " << cambc::fuel_remaining() << " phase " << g_phase << " still " << g_still
                  << " gc " << (g_target_group >= 0 ? g_groups[g_target_group].center.x : -1) << ","
                  << (g_target_group >= 0 ? g_groups[g_target_group].center.y : -1) << " fd "
                  << (g_target_group >= 0 ? g_groups[g_target_group].field.get(me.position()) : -9) << "\n";
}
