// Champion decisions. main.cpp dispatches here after the draft.
//
// Scaling economy: every champion farms the camp group with the best reward per turn (gold
// weighted early, gems late), banks its bag at the nearest altar, and turns gold into shards
// on spawn (passive emblems first, then power shards). Fights are taken only when the local
// Lanchester estimate favours us; otherwise champions step out of reach and recall.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

using cambc::bitboard;
using cambc::distance_field;
using cambc::team;
using cambc::vec2;

namespace {

constexpr bool kDebug = false;
constexpr std::uint16_t kUnreached = distance_field::unreachable;
constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

int cheb(vec2 a, vec2 b) { return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)); }

int W = 0, H = 0;
bool in_bounds(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < W && p.y < H; }

// Packed local copy of a bitboard: reads cost a few instructions instead of a host call.
struct Bits {
    std::vector<std::uint8_t> b;
    void load(const bitboard& board) {
        b.assign(bitboard::linear_buffer_size(), 0);
        board.export_linear_buffer(b);
    }
    bool get(vec2 p) const {
        const int i = p.y * W + p.x;
        return (b[i >> 3] >> (i & 7)) & 1;
    }
};

struct CampSpec { float hp, str; };
CampSpec camp_spec(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return {1350, 20};
    case cambc::camp_type::elk: return {2400, 60};
    case cambc::camp_type::gem_cluster: return {120, 0};
    case cambc::camp_type::gold_cluster: return {120, 0};
    case cambc::camp_type::monolith: return {20000, 200};
    case cambc::camp_type::golem: return {5000, 120};
    }
    return {1000, 50};
}

struct Member {
    vec2 pos;
    cambc::camp_type type;
    float hp, max_hp, str;
};

struct Group {
    std::vector<Member> m;
    vec2 center;
    int gold = 0, gems = 0, respawn = 0;
    int state = 0; // 0 unknown (assumed alive), 1 alive, 2 reward available, 3 respawning
    int ready = 0; // round the group respawns when state == 3
    int seen = -1000;
    int enemy_seen = -1000;
    int bad_until = -1;
    bool big = false; // golem or monolith inside
    bitboard spawn;
    std::unique_ptr<distance_field> field;
    bool has_field = false;
};

// Static world model, filled on the first turn.
Bits g_block;           // walls, holes, altars, enemy spawn
bitboard g_block_board;
bitboard g_camp_board;  // every camp spawn tile: static routes go around camps
Bits g_spawn;           // ally spawn
std::vector<vec2> g_altars;
std::vector<int> g_altar_enemy_lock; // last round an enemy lock was seen on each altar
distance_field g_spawn_field, g_altar_field;
std::vector<Group> g_groups;
std::vector<cambc::map::camp_group> g_engine_groups;
std::vector<int> g_field_order; // groups by distance from our spawn, for lazy field computation
std::size_t g_field_next = 0;
int g_index = 0; // our slot in shared memory

// Our kit.
cambc::champion_type g_type;
bool g_ranged = false;
int g_reset = -1, g_dash = -1, g_nuke = -1, g_shield = -1;
float g_nuke_range = 0;
bool g_nuke_exclusive = false, g_nuke_camps = false;

// Per-turn snapshot.
int g_round = 0;
vec2 g_pos;
float g_hp = 0, g_max_hp = 1, g_str = 0, g_focus = 0;
unsigned g_gems = 0, g_gold = 0;
int g_range = 1;
Bits g_occ;
bool g_moved = false, g_attacked = false, g_cast = false;
int g_target = -1;
int g_approach = 3;
int g_target_since = 0;
int g_last_enemy_round = -1000;
std::array<std::uint8_t, 8> g_shared{};
std::string g_trace;
void trace(const char* what) {
    if (kDebug) g_trace += std::string(what) + ":" + std::to_string(cambc::fuel_remaining() / 1000) + " ";
}

struct Unit {
    vec2 pos;
    float hp, max_hp, str, focus;
    int range;
    cambc::champion_type type;
    unsigned gems;
    float dps;
};
std::vector<Unit> g_enemies, g_allies;

bool has_reset(cambc::champion_type t) {
    using T = cambc::champion_type;
    return t == T::ryth || t == T::vesper_vale || t == T::vendetta || t == T::issac || t == T::marcella
        || t == T::willow || t == T::varg || t == T::tavi;
}

// Rough damage per turn of a champion from its live stats (spells averaged over their cooldown).
float unit_dps(cambc::champion_type t, float str, float focus) {
    using T = cambc::champion_type;
    float spell = 0;
    switch (t) {
    case T::cassi: spell = 2.0f; break;
    case T::stellar: spell = 2.0f; break;
    case T::prism: spell = 2.5f; break;
    case T::marcella: spell = 2.0f; break;
    case T::tavi: spell = 1.4f; break;
    case T::ryth: spell = 1.0f; break;
    case T::ren: spell = 1.1f; break;
    case T::heart_of_ruin: spell = 1.5f; break;
    default: break;
    }
    return str * (has_reset(t) ? 1.12f : 1.0f) + focus * spell / 9.0f;
}

bool blocked_now(vec2 p) { return !in_bounds(p) || g_block.get(p) || g_occ.get(p); }

float my_dps() { return unit_dps(g_type, g_str, g_focus); }

// ---------------------------------------------------------------- initialisation

void init_world() {
    auto& me = cambc::self();
    W = cambc::map::width();
    H = cambc::map::height();
    g_block_board = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    g_block.load(g_block_board);
    g_spawn.load(cambc::map::spawn_region(team::ally));
    g_altars = cambc::map::altars().positions();
    g_altar_enemy_lock.assign(g_altars.size(), -1000);

    // Our slot: index among ally handles.
    const auto allies = cambc::champions(team::ally);
    for (std::size_t i = 0; i < allies.size(); ++i)
        if (static_cast<cambc::handle_champion_t>(allies[i]) == static_cast<cambc::handle_champion_t>(me))
            g_index = static_cast<int>(i);
    if (g_index > 7) g_index = 7;

    // Kit from ability metadata.
    g_type = me.champion_name();
    g_range = me.attack_range();
    g_ranged = g_range > 1;
    const auto abilities = me.abilities();
    for (int s = 0; s < 2; ++s) {
        const auto& ab = abilities[s];
        if (ab.is_attack_reset()) g_reset = s;
        else if (ab.is_dash()) g_dash = s;
        else if (ab.is_shield()) g_shield = s;
        else if (ab.is_damage()) {
            const auto d = ab.get<cambc::ability_damage>();
            g_nuke = s;
            g_nuke_range = d.range;
            g_nuke_exclusive = d.exclusive_with_movement;
            g_nuke_camps = d.valid_targets.contains(cambc::ability_target_mask::camps);
        } else if (ab.is_cc()) {
            const auto d = ab.get<cambc::ability_cc>();
            if (d.modifier > 0) {
                g_nuke = s;
                g_nuke_range = d.range;
                g_nuke_exclusive = d.exclusive_with_movement;
                g_nuke_camps = d.valid_targets.contains(cambc::ability_target_mask::camps);
            }
        }
    }

    // Camp groups (the batched load costs ~25k fuel, so only once).
    g_engine_groups = cambc::map::camp_groups();
    g_groups.resize(g_engine_groups.size());
    for (std::size_t i = 0; i < g_engine_groups.size(); ++i) {
        auto& g = g_groups[i];
        const auto& eg = g_engine_groups[i];
        g.gold = static_cast<int>(eg.gold_value());
        g.gems = static_cast<int>(eg.gem_value());
        g.respawn = eg.max_respawn_timer();
        long sx = 0, sy = 0;
        for (const auto& c : eg.camps()) {
            const auto spec = camp_spec(c.kind());
            g.m.push_back({c.spawn_position(), c.kind(), spec.hp, spec.hp, spec.str});
            g.spawn.set(c.spawn_position());
            if (c.kind() == cambc::camp_type::golem || c.kind() == cambc::camp_type::monolith) g.big = true;
            sx += c.spawn_position().x;
            sy += c.spawn_position().y;
        }
        std::sort(g.m.begin(), g.m.end(), [](const Member& a, const Member& b) {
            return a.str * b.max_hp > b.str * a.max_hp;
        });
        const long n = std::max<long>(1, static_cast<long>(g.m.size()));
        g.center = vec2{static_cast<int>(sx / n), static_cast<int>(sy / n)};
    }
    for (const auto& g : g_groups) g_camp_board |= g.spawn;
    const bitboard static_walls = g_block_board | g_camp_board;
    // Static fields towards spawn and altars.
    {
        bitboard visited;
        g_spawn_field.compute(static_walls, cambc::map::spawn_region(team::ally), visited);
    }
    {
        bitboard roots;
        for (auto a : g_altars)
            for (auto d : kDirs) {
                const vec2 p = a + d;
                if (in_bounds(p) && !g_block.get(p)) roots.set(p);
            }
        bitboard visited;
        g_altar_field.compute(static_walls & ~roots, roots, visited);
        if (!visited.is_set(me.position())) g_altar_field.compute(g_block_board, roots, visited);
    }
    const vec2 home = cambc::map::spawn_region(team::ally).positions()[0];
    g_field_order.resize(g_groups.size());
    for (std::size_t i = 0; i < g_groups.size(); ++i) g_field_order[i] = static_cast<int>(i);
    std::sort(g_field_order.begin(), g_field_order.end(), [&](int a, int b) {
        return cheb(g_groups[a].center, home) < cheb(g_groups[b].center, home);
    });
}

void compute_group_field(Group& g) {
    g.field = std::make_unique<distance_field>();
    bitboard visited;
    g.field->compute((g_block_board | g_camp_board) & ~g.spawn, g.spawn, visited);
    // A group walled in by other camps: route through them (we clear the way on arrival).
    if (!visited.is_set(cambc::map::spawn_region(team::ally).positions()[0]))
        g.field->compute(g_block_board & ~g.spawn, g.spawn, visited);
    g.has_field = true;
}

void lazy_fields(std::uint64_t reserve) {
    while (g_field_next < g_field_order.size() && cambc::fuel_remaining() > reserve) {
        auto& g = g_groups[g_field_order[g_field_next++]];
        if (!g.has_field) compute_group_field(g);
    }
}

int field_value(const distance_field& f, vec2 p) {
    const auto v = f.get(p);
    return v == kUnreached ? 100000 : static_cast<int>(v);
}

// Path distance from p to the group's spawn tiles, or a Chebyshev estimate before the field exists.
int group_dist(const Group& g, vec2 p) {
    if (g.has_field) return field_value(*g.field, p);
    int best = 1000;
    for (const auto& m : g.m) best = std::min(best, cheb(m.pos, p));
    return best + best / 4;
}

// ---------------------------------------------------------------- per-turn snapshot

void scan_units() {
    g_enemies.clear();
    g_allies.clear();
    for (const auto side : {team::enemy, team::ally}) {
        for (const auto p : cambc::vision::champion_tiles(side).positions()) {
            if (p == g_pos) continue;
            const auto c = cambc::vision::champion_at(p);
            Unit u;
            u.pos = p;
            u.hp = c.health();
            u.max_hp = c.max_health();
            u.str = c.strength();
            u.focus = c.focus();
            u.range = c.attack_range();
            u.type = c.champion_name();
            u.gems = side == team::enemy ? c.gem_count() : 0;
            u.dps = unit_dps(u.type, u.str, u.focus);
            (side == team::enemy ? g_enemies : g_allies).push_back(u);
        }
    }
    if (!g_enemies.empty()) g_last_enemy_round = g_round;
}

void load_occupancy() {
    const bitboard occ = cambc::vision::champion_tiles() | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    g_occ.load(occ);
}

void update_groups() {
    Bits vis;
    vis.load(cambc::vision::visible_tiles());
    for (std::size_t gi = 0; gi < g_groups.size(); ++gi) {
        auto& g = g_groups[gi];
        bool near = false;
        for (const auto& m : g.m)
            if (cheb(m.pos, g_pos) <= 9 && vis.get(m.pos)) { near = true; break; }
        if (!near) continue;
        g.seen = g_round;
        for (const auto& e : g_enemies)
            if (cheb(e.pos, g.center) <= 6) g.enemy_seen = g_round;
        bool any_alive = false;
        for (auto& m : g.m) {
            if (!vis.get(m.pos)) continue;
            if (cambc::vision::has_camp(m.pos)) {
                m.hp = cambc::vision::camp_at(m.pos).health();
                any_alive = true;
            } else {
                m.hp = 0;
            }
        }
        if (any_alive) {
            g.state = 1;
            continue;
        }
        const auto st = g_engine_groups[gi].state();
        if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
            g.state = 2;
        } else if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&st)) {
            g.state = 3;
            g.ready = g_round + r->timer;
            for (auto& m : g.m) m.hp = m.max_hp;
        } else {
            g.state = 1;
            for (auto& m : g.m) m.hp = m.max_hp;
        }
    }
}

void update_altar_locks() {
    for (std::size_t i = 0; i < g_altars.size(); ++i) {
        const vec2 a = g_altars[i];
        if (cheb(a, g_pos) > 8 || !cambc::vision::in_vision(a)) continue;
        bool enemy_lock = false;
        for (const auto& lock : cambc::vision::altar_locks(team::enemy))
            if (lock.position() == a) enemy_lock = true;
        g_altar_enemy_lock[i] = enemy_lock ? g_round : -1000;
    }
}

// ---------------------------------------------------------------- shopping

// Item enum layout: family * 6 + stat * 2 (+1 for the emblem); stats are strength, focus, health.
enum Family { kMonster = 0, kPower = 1, kDurability = 2, kTreasure = 3, kJungle = 4, kHome = 5 };

void shop() {
    auto& me = cambc::self();
    unsigned gold = me.gold_count();
    if (gold < 30) return;
    const int stat = 0; // strength: every champion we draft scales its basic attack
    std::vector<int> plan;
    if (g_ranged) plan = {kTreasure, kMonster};
    else plan = {kJungle, kTreasure, kMonster};
    for (int guard = 0; guard < 8 && gold >= 30; ++guard) {
        const auto items = me.item_count();
        int family = kPower;
        for (int f : plan) {
            const cambc::item_type emblem{static_cast<std::uint32_t>(f * 6 + stat * 2 + 1)};
            if (items[emblem] == 0) { family = f; break; }
        }
        const cambc::item_type shard{static_cast<std::uint32_t>(family * 6 + stat * 2)};
        const unsigned have = items[shard];
        unsigned want = std::min<unsigned>(gold / 30, have < 5 ? 5 - have : 1);
        if (want == 0) break;
        if (!me.can_buy(shard, want)) break;
        me.buy(shard, want);
        gold = me.gold_count();
    }
}

// ---------------------------------------------------------------- movement

// Greedy descent of a static field, stepping only onto free tiles with a smaller distance.
bool step_static(const distance_field& f) {
    auto& me = cambc::self();
    trace("ss");
    if (g_moved || !me.can_move()) return false;
    const int cur = field_value(f, g_pos);
    vec2 best = g_pos;
    int best_v = cur;
    for (auto d : kDirs) {
        const vec2 n = g_pos + d;
        if (blocked_now(n)) continue;
        const int v = field_value(f, n);
        if (v < best_v) { best_v = v; best = n; }
    }
    if (best == g_pos) return false;
    g_moved = true;
    if (kDebug) g_trace += "->" + std::to_string(best.x) + "," + std::to_string(best.y) + " ";
    if (!me.move_absolute(best)) { trace("FAIL"); return false; }
    g_pos = best;
    return true;
}

// Exact route around visible units: a field from the free goal tiles up to our tile.
bool step_dynamic(bitboard goals) {
    auto& me = cambc::self();
    trace("sd");
    if (g_moved || !me.can_move()) return false;
    if (goals.is_set(g_pos)) return false;
    bitboard blocked = g_block_board | cambc::vision::champion_tiles() | cambc::vision::camp_tiles()
        | cambc::vision::teleport_tiles();
    blocked.set(g_pos, false);
    goals &= ~blocked;
    if (goals.is_empty()) return false;
    bitboard start;
    start.set(g_pos);
    distance_field field;
    bitboard visited;
    field.compute_to(blocked, goals, start, visited);
    if (!visited.is_set(g_pos)) return false;
    const int cur = field.get(g_pos);
    vec2 best = g_pos;
    int best_v = cur;
    for (auto d : kDirs) {
        const vec2 n = g_pos + d;
        if (blocked_now(n) || !visited.is_set(n)) continue;
        const int v = field.get(n);
        if (v < best_v) { best_v = v; best = n; }
    }
    if (best == g_pos) return false;
    g_moved = true;
    if (!me.move_absolute(best)) return false;
    g_pos = best;
    return true;
}

// Use a ready dash to skip two tiles along a static field during travel.
bool dash_along(const distance_field& f) {
    auto& me = cambc::self();
    if (g_dash < 0 || g_cast || !me.can_cast()) return false;
    const auto ab = me.abilities()[g_dash];
    if (ab.cooldown() > 0) return false;
    const int cur = field_value(f, g_pos);
    if (cur < 6) return false;
    vec2 best = g_pos;
    int best_v = cur - 1;
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = -2; dy <= 2; ++dy) {
            if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
            const vec2 n = g_pos + vec2{dx, dy};
            if (blocked_now(n)) continue;
            const int v = field_value(f, n);
            if (v >= best_v) continue;
            // A free middle tile adjacent to both ends keeps the dash path legal.
            bool mid_ok = false;
            for (auto d : kDirs) {
                const vec2 m = g_pos + d;
                if (cheb(m, n) == 1 && !blocked_now(m)) { mid_ok = true; break; }
            }
            if (!mid_ok) continue;
            best_v = v;
            best = n;
        }
    if (best == g_pos) return false;
    g_cast = true;
    if (!ab.use(best)) { g_moved = true; return false; }
    g_pos = me.position();
    return true;
}

bool travel(Group& g) {
    if (!g.has_field && cambc::fuel_remaining() > 40000) compute_group_field(g);
    if (!g.has_field) return false;
    if (g_enemies.empty()) dash_along(*g.field);
    if (step_static(*g.field)) return true;
    if (g_moved) return false;
    return step_dynamic(g.spawn);
}

// ---------------------------------------------------------------- combat

int find_enemy(vec2 p) {
    for (std::size_t i = 0; i < g_enemies.size(); ++i)
        if (g_enemies[i].pos == p) return static_cast<int>(i);
    return -1;
}

// Attack a visible champion on tile p if possible, then use the attack reset for a second hit.
bool attack_champion_at(vec2 p) {
    auto& me = cambc::self();
    if (g_attacked || !me.can_attack() || cheb(p, g_pos) > g_range) return false;
    if (g_ranged && g_moved) return false;
    if (!cambc::vision::has_champion(p)) return false;
    const auto c = cambc::vision::champion_at(p);
    if (c.team() != team::enemy) return false;
    me.attack(c);
    g_attacked = true;
    if (g_reset >= 0 && !g_cast && me.can_cast()) {
        const auto ab = me.abilities()[g_reset];
        if (ab.cooldown() == 0 && cambc::vision::has_champion(p)) {
            g_cast = true;
            if (ab.use() && me.can_attack() && cambc::vision::has_champion(p))
                me.attack(cambc::vision::champion_at(p));
        }
    }
    return true;
}

bool nuke_champion_at(vec2 p) {
    auto& me = cambc::self();
    if (g_nuke < 0 || g_cast || !me.can_cast()) return false;
    if (g_nuke_exclusive && g_moved) return false;
    if (cheb(p, g_pos) > static_cast<int>(g_nuke_range)) return false;
    const auto ab = me.abilities()[g_nuke];
    if (ab.cooldown() > 0 || !cambc::vision::has_champion(p)) return false;
    const auto c = cambc::vision::champion_at(p);
    if (c.team() != team::enemy) return false;
    g_cast = true;
    if (ab.use(c)) {
        if (g_nuke_exclusive) g_moved = true;
        return true;
    }
    return false;
}

struct Fight {
    float ours = 0, theirs = 0;
    int nearest = 1000;
    bool threatened = false;
};

Fight assess() {
    Fight f;
    float our_dps = my_dps(), our_hp = g_hp, their_dps = 0, their_hp = 0;
    for (const auto& a : g_allies)
        if (cheb(a.pos, g_pos) <= 6) { our_dps += a.dps; our_hp += a.hp; }
    for (const auto& e : g_enemies) {
        const int d = cheb(e.pos, g_pos);
        if (d > 8) continue;
        their_dps += e.dps;
        their_hp += e.hp;
        f.nearest = std::min(f.nearest, d);
        if (d <= e.range + 3) f.threatened = true;
    }
    f.ours = our_dps * our_hp;
    f.theirs = their_dps * their_hp;
    return f;
}

// Pick the enemy we kill fastest, preferring those already in range.
int fight_target() {
    int best = -1;
    float best_s = 1e30f;
    for (std::size_t i = 0; i < g_enemies.size(); ++i) {
        const auto& e = g_enemies[i];
        const int d = cheb(e.pos, g_pos);
        float s = e.hp / std::max(1.0f, e.dps) + std::max(0, d - g_range) * 40.0f - e.gems * 0.5f;
        if (s < best_s) { best_s = s; best = static_cast<int>(i); }
    }
    return best;
}

bool do_fight() {
    const int t = fight_target();
    if (t < 0) return false;
    const auto e = g_enemies[t];
    const int d = cheb(e.pos, g_pos);
    if (d > 6) return false;
    auto& me = cambc::self();
    if (d <= g_range) {
        nuke_champion_at(e.pos);
        attack_champion_at(e.pos);
        return true;
    }
    // Close the distance: melee can still hit after stepping, ranged cannot.
    if (d <= static_cast<int>(g_nuke_range) && !g_ranged) {
        // Melee nukes exclude walking; walking into reach is worth more.
    } else if (d <= static_cast<int>(g_nuke_range)) {
        if (nuke_champion_at(e.pos)) return true;
    }
    bitboard goal;
    goal.draw_chebyshev(e.pos, static_cast<float>(g_range));
    step_dynamic(goal);
    if (!g_ranged && me.can_attack()) attack_champion_at(e.pos);
    return true;
}

// Step to the reachable tile that keeps us furthest outside enemy reach, drifting home.
bool flee() {
    auto& me = cambc::self();
    auto score = [&](vec2 p) {
        int margin = 100;
        for (const auto& e : g_enemies) margin = std::min(margin, cheb(p, e.pos) - e.range);
        return margin * 10.0f - field_value(g_spawn_field, p) * 0.5f;
    };
    float best_s = score(g_pos);
    vec2 best = g_pos;
    for (auto d : kDirs) {
        const vec2 n = g_pos + d;
        if (blocked_now(n)) continue;
        const float s = score(n);
        if (s > best_s) { best_s = s; best = n; }
    }
    // A ready dash buys two tiles when someone is close.
    if (g_dash >= 0 && !g_cast && me.can_cast()) {
        const auto ab = me.abilities()[g_dash];
        if (ab.cooldown() == 0) {
            vec2 dbest = best;
            float dbest_s = best_s + 5;
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy) {
                    if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
                    const vec2 n = g_pos + vec2{dx, dy};
                    if (blocked_now(n)) continue;
                    bool mid_ok = false;
                    for (auto d : kDirs) {
                        const vec2 m = g_pos + d;
                        if (cheb(m, n) == 1 && !blocked_now(m)) { mid_ok = true; break; }
                    }
                    if (!mid_ok) continue;
                    const float s = score(n);
                    if (s > dbest_s) { dbest_s = s; dbest = n; }
                }
            if (dbest != best && cheb(dbest, g_pos) == 2) {
                g_cast = true;
                if (ab.use(dbest)) {
                    g_pos = me.position();
                    return true;
                }
                g_moved = true;
                return false;
            }
        }
    }
    if (best == g_pos || g_moved || !me.can_move()) return false;
    g_moved = true;
    if (!me.move_absolute(best)) return false;
    g_pos = best;
    return true;
}

// ---------------------------------------------------------------- camps

int alive_count(const Group& g) {
    int n = 0;
    for (const auto& m : g.m) n += m.hp > 0;
    return n;
}

int allies_on(int gi) {
    int n = 0;
    for (int i = 0; i < 8; ++i)
        if (i != g_index && g_shared[i] == gi + 1) ++n;
    return n;
}

// Estimated damage the tank takes while the team clears the group (strongest hitters first;
// members are pre-sorted by strength per health at initialisation).
float clear_damage(const Group& g, float dps) {
    float str_left = 0;
    for (const auto& m : g.m)
        if (m.hp > 0) str_left += m.str;
    if (str_left <= 0) return 0;
    float dmg = 0;
    for (const auto& m : g.m) {
        if (m.hp <= 0) continue;
        dmg += str_left * (m.hp / dps);
        str_left -= m.str;
        if (str_left <= 0) break;
    }
    return dmg;
}

float gold_mult() {
    float mult = g_type == cambc::champion_type::vesper_vale ? 1.33f : 1.0f;
    return mult;
}

float group_score(int gi, int dist) {
    auto& g = g_groups[gi];
    if (g.bad_until > g_round) return -1;
    int wait = 0;
    if (g.state == 3) {
        wait = std::max(0, g.ready - g_round - dist);
        if (wait > 40) return -1;
    }
    const int helpers = allies_on(gi);
    const int n = helpers + 1;
    float hp = 0;
    if (g.state != 2)
        for (const auto& m : g.m) hp += g.state == 3 ? m.max_hp : m.hp;
    const float dps = my_dps() * n * (g_type == cambc::champion_type::varg && g.m.size() > 1 ? 1.4f : 1.0f);
    const float kill = hp / std::max(1.0f, dps);
    if (g.state != 2) {
        const float dmg = clear_damage(g, dps) / n;
        if (dmg > g_hp * 0.75f) return -1;
    }
    const float gem_w = g_round < 500 ? 2.5f : g_round < 1300 ? 3.5f : 6.0f;
    float value = (g.gems * gem_w + g.gold * gold_mult()) / n;
    if (g_round - g.enemy_seen < 25) value *= 0.5f;
    return value / (dist + kill + 6 + wait);
}

void choose_target() {
    float best_s = 0;
    int best = -1;
    float cur_s = -1;
    for (std::size_t i = 0; i < g_groups.size(); ++i) {
        const int dist = group_dist(g_groups[i], g_pos);
        if (dist >= 100000) continue;
        const float s = group_score(static_cast<int>(i), dist);
        if (static_cast<int>(i) == g_target) cur_s = s;
        if (s > best_s) { best_s = s; best = static_cast<int>(i); }
    }
    if (g_target >= 0 && cur_s > 0 && best_s < cur_s * 1.3f) return;
    if (best != g_target) {
        g_target = best;
        if (best >= 0 && !g_groups[best].has_field && cambc::fuel_remaining() > 45000)
            compute_group_field(g_groups[best]);
        g_approach = g_range;
        g_target_since = g_round;
    }
}

// Attack order: hitters with the most strength per health first, clusters last.
bool attack_camps(Group& g) {
    auto& me = cambc::self();
    if (g_attacked || !me.can_attack()) return false;
    if (g_ranged && g_moved) return false;
    const Member* best = nullptr;
    float best_s = -1;
    for (const auto& m : g.m) {
        if (m.hp <= 0 || cheb(m.pos, g_pos) > g_range) continue;
        if (!cambc::vision::has_camp(m.pos)) continue;
        float s = (m.str + 1) / std::max(1.0f, m.hp);
        if (g_type == cambc::champion_type::varg)
            for (const auto& o : g.m)
                if (&o != &m && o.hp > 0 && cheb(o.pos, m.pos) <= 1) s += 0.01f;
        if (s > best_s) { best_s = s; best = &m; }
    }
    // Any other camp in reach is fair game too (an awake neighbour, a cluster in passing).
    if (!best) return false;
    const vec2 p = best->pos;
    me.attack(cambc::vision::camp_at(p));
    g_attacked = true;
    if (g_reset >= 0 && !g_cast && me.can_cast()) {
        const auto ab = me.abilities()[g_reset];
        if (ab.cooldown() == 0) {
            g_cast = true;
            if (ab.use() && me.can_attack()) {
                if (cambc::vision::has_camp(p)) {
                    me.attack(cambc::vision::camp_at(p));
                } else {
                    for (const auto& m : g.m)
                        if (m.pos != p && m.hp > 0 && cheb(m.pos, g_pos) <= g_range && cambc::vision::has_camp(m.pos)) {
                            me.attack(cambc::vision::camp_at(m.pos));
                            break;
                        }
                }
            }
        }
    }
    return true;
}

void nuke_camps(Group& g) {
    auto& me = cambc::self();
    if (g_nuke < 0 || !g_nuke_camps || g_cast || !me.can_cast()) return;
    if (g_nuke_exclusive && g_moved) return;
    const auto ab = me.abilities()[g_nuke];
    if (ab.cooldown() > 0) return;
    for (const auto& m : g.m) {
        if (m.hp <= 0 || cheb(m.pos, g_pos) > static_cast<int>(g_nuke_range)) continue;
        if (!cambc::vision::has_camp(m.pos)) continue;
        g_cast = true;
        if (ab.use(cambc::vision::camp_at(m.pos)) && g_nuke_exclusive) g_moved = true;
        return;
    }
}

void shield_if_hit() {
    auto& me = cambc::self();
    if (g_shield < 0 || g_cast || !me.can_cast()) return;
    if (g_hp > g_max_hp * 0.9f) return;
    const auto ab = me.abilities()[g_shield];
    if (ab.cooldown() > 0) return;
    g_cast = true;
    ab.use();
}

bool any_awake_camp_near(int radius) {
    for (const auto p : cambc::vision::awake_camp_tiles().positions())
        if (cheb(p, g_pos) <= radius) return true;
    return false;
}

void work_target() {
    auto& g = g_groups[g_target];
    const int dist = group_dist(g, g_pos);
    if (g.state == 2 || (g.state == 1 && alive_count(g) == 0)) {
        // Capture: stand on a spawn tile until the reward lands.
        if (g.spawn.is_set(g_pos)) return;
        if (dist > 6) { travel(g); return; }
        step_dynamic(g.spawn);
        return;
    }
    if (g.state == 3) {
        // Wait beside the region, never on a spawn tile (that delays the respawn).
        if (dist > 3) travel(g);
        else if (g.spawn.is_set(g_pos)) {
            bitboard goal;
            for (const auto& m : g.m) goal.draw_chebyshev(m.pos, 2.0f);
            goal &= ~g.spawn;
            step_dynamic(goal);
        }
        return;
    }
    // Alive: hit what is in reach, otherwise approach.
    nuke_camps(g);
    if (attack_camps(g)) { shield_if_hit(); return; }
    if (dist > g_range + 6) { travel(g); return; }
    bitboard goal;
    bool any = false;
    for (const auto& m : g.m)
        if (m.hp > 0) { goal.draw_chebyshev(m.pos, static_cast<float>(g_approach)); any = true; }
    if (!any) { step_dynamic(g.spawn); return; }
    // In range but no sight of the target: come closer next time.
    bool in_range = false;
    for (const auto& m : g.m)
        if (m.hp > 0 && cheb(m.pos, g_pos) <= g_range) in_range = true;
    if (in_range && g_approach > 1) {
        --g_approach;
        goal.clear();
        for (const auto& m : g.m)
            if (m.hp > 0) goal.draw_chebyshev(m.pos, static_cast<float>(g_approach));
    }
    if (!step_dynamic(goal) && !g_moved) travel(g);
    if (!g_ranged) attack_camps(g);
    shield_if_hit();
}

// ---------------------------------------------------------------- banking and recall

bool can_tribute_at(std::size_t i) { return g_round - g_altar_enemy_lock[i] > 60; }

bool bank() {
    auto& me = cambc::self();
    for (std::size_t i = 0; i < g_altars.size(); ++i) {
        const vec2 a = g_altars[i];
        if (cheb(a, g_pos) > cambc::constants::submit_gems_range) continue;
        if (g_round - g_altar_enemy_lock[i] <= 0) {
            // Enemy lock beside us: knock it off with basic attacks.
            if (!g_attacked && me.can_attack())
                for (const auto& lock : cambc::vision::altar_locks(team::enemy))
                    if (lock.position() == a && cheb(a, g_pos) <= g_range) {
                        me.attack(lock);
                        g_attacked = true;
                        return true;
                    }
            continue;
        }
        if (cambc::vision::has_altar_lock(a)) continue;
        me.submit_gems(a);
        return true;
    }
    bitboard goal;
    bool any = false;
    for (std::size_t i = 0; i < g_altars.size(); ++i) {
        if (!can_tribute_at(i)) continue;
        goal.draw_chebyshev(g_altars[i], static_cast<float>(cambc::constants::submit_gems_range));
        any = true;
    }
    if (!any)
        for (auto a : g_altars) goal.draw_chebyshev(a, 1.0f);
    if (field_value(g_altar_field, g_pos) > 5 && step_static(g_altar_field)) return true;
    if (!g_moved) step_dynamic(goal);
    return true;
}

bool want_bank() {
    if (g_gems == 0) return false;
    const int d = field_value(g_altar_field, g_pos);
    const unsigned ours = cambc::global_submitted_gems(team::ally);
    if (ours + g_gems >= cambc::constants::winning_gems) return true;
    if (g_round + d + 20 >= static_cast<int>(cambc::constants::max_rounds)) return true;
    if (g_gems >= 40) return true;
    if (g_gems >= 20 && g_round - g_last_enemy_round < 30) return true;
    if (d <= 4) return true;
    if (g_gems >= 12 && d <= 10) return true;
    return false;
}

bool want_recall() {
    if (g_spawn.get(g_pos)) return false;
    if (!g_enemies.empty()) return false;
    if (any_awake_camp_near(6)) return false;
    const int d = field_value(g_spawn_field, g_pos);
    if (d <= 6) return false;
    if (g_hp < g_max_hp * 0.4f) return true;
    if (g_gold >= 150 && g_hp < g_max_hp * 0.7f) return true;
    if (g_gold >= 300) return true;
    return false;
}

bool capturing() {
    if (g_target < 0) return false;
    const auto& g = g_groups[g_target];
    return g.state == 2 && g.spawn.is_set(g_pos);
}

} // namespace

void Bot::play_turn() {
    auto& me = cambc::self();
    static bool inited = false;
    if (!inited) {
        init_world();
        inited = true;
        if (kDebug && g_index == 0) {
            Bits walls, holes, bush, ally, enemy;
            walls.load(cambc::map::walls()); holes.load(cambc::map::holes()); bush.load(cambc::map::bushes());
            enemy.load(cambc::map::spawn_region(team::enemy));
            std::vector<std::string> rows(H, std::string(W, '.'));
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const vec2 p{x, y};
                    char ch = '.';
                    if (walls.get(p)) ch = '#'; else if (holes.get(p)) ch = 'o'; else if (bush.get(p)) ch = '"';
                    else if (g_spawn.get(p)) ch = 'S'; else if (enemy.get(p)) ch = 'E';
                    rows[y][x] = ch;
                }
            for (auto a : g_altars) rows[a.y][a.x] = 'A';
            for (std::size_t i = 0; i < g_groups.size(); ++i)
                for (const auto& m : g_groups[i].m) rows[m.pos.y][m.pos.x] = static_cast<char>('a' + (i % 26));
            for (int y = H - 1; y >= 0; --y) std::cout << "map " << (y < 10 ? " " : "") << y << ' ' << rows[y] << '\n';
            for (std::size_t i = 0; i < g_groups.size(); ++i) {
                std::cout << "grp " << i << " (" << static_cast<char>('a' + (i % 26)) << ") gold " << g_groups[i].gold << " gems " << g_groups[i].gems << " :";
                for (const auto& m : g_groups[i].m) std::cout << ' ' << static_cast<int>(m.type);
                std::cout << '\n';
            }
        }
    }
    g_round = static_cast<int>(cambc::round_number());
    g_moved = g_attacked = g_cast = false;

    if (!me.is_alive()) {
        shop();
        g_target = -1;
        return;
    }
    g_pos = me.position();
    g_hp = me.health();
    g_max_hp = me.max_health();
    g_str = me.strength();
    g_focus = me.focus();
    g_gems = me.gem_count();
    g_gold = me.gold_count();
    {
        std::array<std::uint8_t, 8> bytes{};
        cambc::shared_memory.read(0, std::span{bytes});
        g_shared = bytes;
    }

    const auto state = me.state();
    if (g_spawn.get(g_pos)) {
        shop();
        g_gold = me.gold_count();
    }
    trace("start");
    if (g_target >= 0 && !g_groups[g_target].has_field) compute_group_field(g_groups[g_target]);
    scan_units();
    load_occupancy();
    trace("scan");
    update_groups();
    update_altar_locks();
    trace("groups");

    const bool busy = state.contains(cambc::champion_state::recalling)
        || state.contains(cambc::champion_state::submitting_gems)
        || state.contains(cambc::champion_state::teleporting);
    if (!busy) {
        bool done = false;
        // Stay on spawn for a turn end to heal after a recall.
        if (g_spawn.get(g_pos) && g_hp < g_max_hp * 0.95f && g_enemies.empty()) done = true;
        if (!g_enemies.empty()) {
            const auto f = assess();
            const float caution = 1.25f + g_gems / 100.0f;
            if (f.ours > f.theirs * caution) {
                trace("fight");
                done = do_fight();
            } else if (f.threatened) {
                trace("flee");
                flee();
                done = true;
            }
        }
        if (!done && !capturing()) {
            if (want_bank()) {
                trace("bank");
                bank();
                done = true;
            } else if (want_recall()) {
                trace("recall");
                me.recall();
                done = true;
            }
        }
        if (!done) {
            choose_target();
            trace("choose");
            if (g_target >= 0) work_target();
            else if (!g_spawn.get(g_pos)) step_static(g_spawn_field);
            trace("work");
        }
    }

    if (kDebug && (g_round % 25 == 0 || g_round < 4 || (g_round > 100 && g_round < 130)))
        std::cout << "dbg r" << g_round << " me" << g_index << " pos " << g_pos.x << "," << g_pos.y << " hp " << g_hp
                  << " gold " << g_gold << " gems " << g_gems << " tgt " << g_target
                  << (g_target >= 0 ? " st " + std::to_string(g_groups[g_target].state) + " d " + std::to_string(group_dist(g_groups[g_target], g_pos)) : std::string())
                  << " en " << g_enemies.size() << " fuel " << cambc::fuel_remaining() << " | " << g_trace << '\n';
    g_trace.clear();
    // Publish our target for the team.
    cambc::shared_memory[static_cast<std::size_t>(g_index)] = static_cast<std::uint8_t>(g_target + 1);
    lazy_fields(55000);
}
