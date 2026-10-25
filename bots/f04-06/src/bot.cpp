// Champion decisions. main.cpp dispatches here after the draft.
// Early aggression: the whole team moves as one pack, farms the camp the pack agrees on through shared
// memory, and jumps on enemy champions it can outnumber. Kill gold is turned into shards at spawn.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <span>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;
using cambc::bitboard;

namespace {

// ---- tuning ---------------------------------------------------------------------------------------
constexpr float kEngageRatio = 1.1f;     // fight when our square-law power >= this × theirs
constexpr float kCarrierRatio = 1.6f;    // ... when carrying many gems
constexpr float kRecallHp = 0.50f;       // recall below this share of health with nobody close
constexpr float kFleeHp = 0.22f;         // always disengage below this share
constexpr int kBankGems = 20;            // carry at most this many gems before banking
constexpr int kNearAltar = 7;            // bank any gems when an altar is this close
constexpr int kHuntRange = 14;           // answer a pack sighting this far away
constexpr int kShopGold = 270;           // recall to shop with this much gold
constexpr int kPackSize = 2;             // champions per farming pack
constexpr int kGemWeight = 5;            // a gem is worth this much gold when choosing camps
constexpr int kKeepAway = 5;             // back off from stronger foes this close
constexpr int kWalkHome = 10;            // walk instead of recalling this close to spawn
constexpr int kShadowRounds = -1;        // walk home rather than recall this soon after seeing a foe
constexpr int kEnemySideSlack = 99;      // skip camps this much closer to the enemy spawn than ours
constexpr int kTraceFrom = -1, kTraceTo = -1;
constexpr bool kHunt = false;            // answer pack sightings
constexpr int kHuntAge = 6;              // rounds a sighting stays valid

struct Group {
    bitboard tiles;
    bitboard near1, near3;  // tiles within 1 and 3 of the group's spawn tiles
    vec2 center;
    int gems = 0, gold = 0;
    float hp = 0, str = 0;
    bool boss = false;
    int ready = 0;        // round from which we expect it alive (0 = alive or unknown)
    bool reward = false;  // seen dead with the reward waiting
};

struct Foe {
    vec2 p;
    float hp, max_hp, str, focus;
    int range;
    bool channel;
};

bool g_init = false;
int W = 0, H = 0;
bitboard g_static;            // walls, holes, altars, enemy spawn
bitboard g_ally_spawn;
bitboard g_enemy_spawn_near;  // enemy spawn and two tiles around it: they heal there, we cannot follow
std::vector<Group> g_groups;
std::vector<cambc::map::camp_group> g_cg;
std::vector<vec2> g_altars;
std::vector<std::uint16_t> g_home;  // walking distance to our spawn, 65535 = unreached
vec2 g_enemy_home{0, 0};
int g_round = 0;
vec2 g_pos;
bool g_move_failed = false;
bool g_attacked = false;
float g_last_hp = 0;
int g_shards = 0;
const char* g_mode = "";
char g_last[160] = "";
bool g_was_alive = true;
int g_foe_seen = -100;            // last round this champion saw an enemy
int g_pack = 0;                   // champions farm in packs: {0,2}, {1,3}, {4} by draft slot
constexpr std::array<int, 5> kPackByte{0, 1, 5, 6, 7};

// Run one engine action; a refused action is logged and reported as false.
template<class F> bool safe(const char* what, F&& f) {
    try {
        return f();
    } catch (const cambc::error& e) {
        std::cout << "dbg " << what << " r" << g_round << " " << e.what() << "\n";
        return false;
    }
}

int dist(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }

int home_dist(vec2 p) {
    if (p.x < 0 || p.y < 0 || p.x >= W || p.y >= H) return 65535;
    return g_home[static_cast<size_t>(p.y * W + p.x)];
}

bitboard ring(vec2 c, int r) {
    bitboard b;
    b.clear();
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            const vec2 q{c.x + dx, c.y + dy};
            if (q.x >= 0 && q.y >= 0 && q.x < W && q.y < H) b.set(q);
        }
    return b;
}

void camp_stats(cambc::camp_type t, float& hp, float& str) {
    using ct = cambc::camp_type;
    switch (t) {
    case ct::wolf: hp = 1350; str = 20; break;
    case ct::elk: hp = 2400; str = 60; break;
    case ct::gem_cluster: hp = 120; str = 0; break;
    case ct::gold_cluster: hp = 120; str = 0; break;
    case ct::monolith: hp = 20000; str = 200; break;
    case ct::golem: hp = 5000; str = 120; break;
    default: hp = 2000; str = 60; break;
    }
}

void init_map() {
    g_init = true;
    W = cambc::map::width();
    H = cambc::map::height();
    g_static = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    g_ally_spawn = cambc::map::spawn_region(team::ally);
    g_enemy_spawn_near.clear();
    for (auto p : cambc::map::spawn_region(team::enemy).positions()) g_enemy_spawn_near |= ring(p, 2);
    for (auto a : cambc::map::altars().positions()) g_altars.push_back(a);
    g_cg = cambc::map::camp_groups();
    for (const auto& grp : g_cg) {
        Group g;
        g.tiles = grp.spawn_tiles();
        g.near1.clear();
        g.near3.clear();
        for (auto p : g.tiles.positions()) {
            g.near1 |= ring(p, 1);
            g.near3 |= ring(p, 3);
        }
        g.gems = static_cast<int>(grp.gem_value());
        g.gold = static_cast<int>(grp.gold_value());
        int sx = 0, sy = 0, n = 0;
        for (const auto& c : grp.camps()) {
            float hp, str;
            camp_stats(c.kind(), hp, str);
            g.hp += hp;
            g.str += str;
            if (c.is_monolith() || c.is_golem()) g.boss = true;
            sx += c.spawn_position().x;
            sy += c.spawn_position().y;
            ++n;
        }
        g.center = n ? vec2{sx / n, sy / n} : vec2{0, 0};
        g_groups.push_back(g);
    }
    {
        int sx = 0, sy = 0, n = 0;
        for (auto p : cambc::map::spawn_region(team::enemy).positions()) { sx += p.x; sy += p.y; ++n; }
        if (n) g_enemy_home = vec2{sx / n, sy / n};
    }
    {
        using ct = cambc::champion_type;
        static constexpr std::array order{ct::vendetta, ct::ryth, ct::vesper_vale, ct::marcella, ct::varg};
        static constexpr std::array pack_of{0, 0, 0, 1, 1};
        g_pack = 4;
        const auto mine = cambc::self().champion_name();
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == mine) g_pack = pack_of[i];
    }
    cambc::distance_field f;
    bitboard visited;
    bitboard walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    f.compute(walls, g_ally_spawn, visited);
    g_home.assign(static_cast<size_t>(W * H), 65535);
    std::vector<std::uint16_t> buf(static_cast<size_t>(W * H));
    f.export_linear_buffer(std::span<std::uint16_t>{buf.data(), buf.size()});
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (visited.is_set(vec2{x, y})) g_home[static_cast<size_t>(y * W + x)] = buf[static_cast<size_t>(y * W + x)];
}

// ---- shared memory -------------------------------------------------------------------------------
std::array<std::uint8_t, 8> g_mem{};
void mem_load() { cambc::shared_memory.read(0, std::span<std::uint8_t>{g_mem.data(), 8}); }
void mem_store() { cambc::shared_memory.write(0, std::span<const std::uint8_t>{g_mem.data(), 8}); }
int age(std::uint8_t stamp) { return (g_round - stamp) & 255; }

// ---- movement ------------------------------------------------------------------------------------
bitboard blocked_now() {
    bitboard b = g_static | cambc::vision::champion_tiles(team::ally) | cambc::vision::champion_tiles(team::enemy)
        | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    b.set(g_pos, false);
    return b;
}

bool try_move(vec2 t) {
    auto& me = cambc::self();
    if (g_move_failed || !me.can_move()) return false;
    try {
        if (me.move_absolute(t)) { g_pos = t; return true; }
    } catch (const cambc::error& e) {
        std::cout << "dbg move " << g_round << " " << g_pos.x << "," << g_pos.y << " -> " << t.x << "," << t.y << " " << e.what() << "\n";
    }
    g_move_failed = true;
    return false;
}

// One step along a shortest path to any of the goal tiles.
bool step_to(bitboard goals) {
    auto& me = cambc::self();
    if (g_move_failed || !me.can_move()) return false;
    const bitboard blocked = blocked_now();
    goals &= ~blocked;
    if (goals.is_empty() || goals.is_set(g_pos)) return false;
    cambc::distance_field f;
    bitboard visited;
    bitboard dest;
    dest.clear();
    dest.set(g_pos);
    f.compute_to(blocked, goals, dest, visited);
    if (!visited.is_set(g_pos)) return false;
    const auto d = f.next_move_from(g_pos);
    if (!d) return false;
    const vec2 t{g_pos.x + d->x, g_pos.y + d->y};
    if (!cambc::map::is_in_bounds(t) || blocked.is_set(t)) return false;
    return try_move(t);
}

// Step away from the foes, preferring tiles closer to our spawn.
bool step_away(const std::vector<Foe>& foes) {
    auto& me = cambc::self();
    if (g_move_failed || !me.can_move()) return false;
    const bitboard blocked = blocked_now();
    auto score = [&](vec2 p) {
        int m = 99;
        for (const auto& f : foes) m = std::min(m, dist(p, f.p) - f.range);
        const int hd = home_dist(p);
        return std::min(m, 8) * 10 - (hd == 65535 ? 999 : hd);
    };
    vec2 best = g_pos;
    int bs = score(g_pos);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            const vec2 q{g_pos.x + dx, g_pos.y + dy};
            if (!cambc::map::is_in_bounds(q) || blocked.is_set(q)) continue;
            const int s = score(q);
            if (s > bs) { bs = s; best = q; }
        }
    return best != g_pos && try_move(best);
}

// ---- abilities -----------------------------------------------------------------------------------
template<class T> std::optional<T> payload(const cambc::ability& a) {
    const auto& info = a.info();
    if (!info) return std::nullopt;
    if (auto p = std::get_if<T>(&*info)) return *p;
    return std::nullopt;
}

bool excludes_walk(const cambc::ability& a) {
    const auto& info = a.info();
    if (!info) return false;
    return std::visit([](const auto& p) { return p.exclusive_with_movement; }, *info);
}

float spell_range(const cambc::ability& a) {
    if (auto p = payload<cambc::ability_damage>(a)) return p->range;
    if (auto p = payload<cambc::ability_cc>(a)) return p->range;
    if (auto p = payload<cambc::ability_aoe_damage>(a)) return p->range;
    if (auto p = payload<cambc::ability_heal>(a)) return p->range;
    return 0;
}

bool moved_this_turn(vec2 start) { return g_pos != start; }

// Cast an offensive spell (damage, stun, area) on a champion or camp tile.
bool cast_offensive(vec2 target, bool is_champion, bool allow_cc, vec2 start) {
    auto& me = cambc::self();
    if (!me.can_cast()) return false;
    for (const auto& a : me.abilities()) {
        if (!a.is_ready()) continue;
        const bool dmg = a.is_damage(), cc = a.is_cc(), aoe = a.is_aoe_damage();
        if (!(dmg || aoe || (cc && allow_cc))) continue;
        if (excludes_walk(a) && moved_this_turn(start)) continue;
        if (dist(g_pos, target) > static_cast<int>(spell_range(a))) continue;
        return safe("cast", [&] {
            if (aoe) return a.use(target);
            if (is_champion) {
                if (!cambc::vision::champion_tiles(team::enemy).is_set(target)) return false;
                return a.use(cambc::vision::champion_at(target));
            }
            if (!cambc::vision::camp_tiles().is_set(target)) return false;
            return a.use(cambc::vision::camp_at(target));
        });
    }
    return false;
}

bool use_shield() {
    auto& me = cambc::self();
    if (!me.can_cast()) return false;
    for (const auto& a : me.abilities())
        if (a.is_shield() && a.is_ready()) return safe("shield", [&] { return a.use(); });
    return false;
}

// Basic attack on the tile, then an attack reset and a second hit when available.
bool attack_tile(vec2 target, bool champion) {
    auto& me = cambc::self();
    return safe("attack", [&] {
        if (champion) {
            if (!cambc::vision::champion_tiles(team::enemy).is_set(target)) return false;
            me.attack(cambc::vision::champion_at(target));
        } else {
            if (!cambc::vision::camp_tiles().is_set(target)) return false;
            me.attack(cambc::vision::camp_at(target));
        }
        return true;
    });
}

bool hit(vec2 target, bool champion) {
    auto& me = cambc::self();
    if (!me.can_attack()) return false;
    if (dist(g_pos, target) > me.attack_range()) return false;
    if (!attack_tile(target, champion)) return false;
    g_attacked = true;
    // Reset only right after our own attack, and only while the target is still there.
    if (me.can_cast() && !me.can_attack()) {
        for (const auto& a : me.abilities()) {
            if (!a.is_attack_reset() || !a.is_ready()) continue;
            const bool there = champion ? cambc::vision::champion_tiles(team::enemy).is_set(target)
                                        : cambc::vision::camp_tiles().is_set(target);
            if (!there) break;
            if (safe("reset", [&] { return a.use(); }) && me.can_attack()) attack_tile(target, champion);
            break;
        }
    }
    return true;
}

// Dash to a free tile within `want` of the target; returns true if we dashed.
bool dash_towards(vec2 target, int want) {
    auto& me = cambc::self();
    if (!me.can_cast() || g_move_failed) return false;
    for (const auto& a : me.abilities()) {
        if (!a.is_dash() || !a.is_ready()) continue;
        const bitboard blocked = blocked_now();
        bitboard tiles = a.valid_tiles() & ~blocked;
        vec2 best = g_pos;
        int bd = dist(g_pos, target);
        for (auto t : tiles.positions()) {
            const int d = dist(t, target);
            if (d < bd && d >= 1) { bd = d; best = t; }
        }
        if (best == g_pos || bd > want) return false;
        try {
            if (a.use(best)) { g_pos = best; return true; }
        } catch (const cambc::error& e) {
            std::cout << "dbg dash " << g_round << " " << g_pos.x << "," << g_pos.y << " -> " << best.x << "," << best.y << " " << e.what() << "\n";
        }
        g_move_failed = true;
        return false;
    }
    return false;
}

bool dash_away(const std::vector<Foe>& foes) {
    auto& me = cambc::self();
    if (!me.can_cast() || g_move_failed) return false;
    for (const auto& a : me.abilities()) {
        if (!a.is_dash() || !a.is_ready()) continue;
        if (auto p = payload<cambc::ability_dash>(a); p && p->modifier > 0) return false;  // keep the engage dash
        const bitboard blocked = blocked_now();
        bitboard tiles = a.valid_tiles() & ~blocked;
        vec2 best = g_pos;
        int bs = -1000000;
        for (auto t : tiles.positions()) {
            int m = 99;
            for (const auto& f : foes) m = std::min(m, dist(t, f.p));
            const int hd = home_dist(t);
            if (hd == 65535) continue;
            const int s = std::min(m, 6) * 10 - hd;
            if (s > bs) { bs = s; best = t; }
        }
        if (best == g_pos) return false;
        if (safe("dash_away", [&] { return a.use(best); })) { g_pos = best; return true; }
        g_move_failed = true;
        return false;
    }
    return false;
}

// ---- shopping ------------------------------------------------------------------------------------
void shop() {
    auto& me = cambc::self();
    if (me.consumables()[cambc::consumable_type::altar_lock].count() == 0 && me.gold_count() >= 10
        && me.can_buy(cambc::consumable_type::altar_lock))
        me.buy(cambc::consumable_type::altar_lock);
    // Five strength shards, then five health shards, repeating (five combine into a power emblem).
    while (me.gold_count() >= 30) {
        const auto counts = me.item_count();
        const int s = static_cast<int>(counts[cambc::item_type::power_shard_of_strength])
            + 5 * static_cast<int>(counts[cambc::item_type::power_emblem_of_strength]);
        const int h = static_cast<int>(counts[cambc::item_type::power_shard_of_health])
            + 5 * static_cast<int>(counts[cambc::item_type::power_emblem_of_health]);
        const auto item = s / 5 <= h / 5 ? cambc::item_type::power_shard_of_strength
                                         : cambc::item_type::power_shard_of_health;
        if (!me.can_buy(item, 1)) break;
        me.buy(item, 1);
        ++g_shards;
    }
}

// ---- knowledge -----------------------------------------------------------------------------------
void update_groups() {
    const bitboard vis = cambc::vision::visible_tiles();
    const auto& groups = g_cg;
    for (size_t i = 0; i < groups.size() && i < g_groups.size(); ++i) {
        auto& g = g_groups[i];
        if ((g.tiles & vis).is_empty()) {
            if (g.ready && g.ready <= g_round) g.ready = 0;
            continue;
        }
        try {
            const auto st = groups[i].state();
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) { g.ready = 0; g.reward = false; }
            else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) { g.ready = 0; g.reward = true; }
            else if (auto r = std::get_if<cambc::map::camp_group_respawning>(&st)) {
                g.ready = g_round + r->timer;
                g.reward = false;
            }
        } catch (...) {}
    }
}

std::vector<Foe> visible_foes(team side) {
    std::vector<Foe> out;
    for (auto p : cambc::vision::champion_tiles(side).positions()) {
        const auto c = cambc::vision::champion_at(p);
        Foe f;
        f.p = p;
        f.hp = c.health();
        f.max_hp = c.max_health();
        f.str = c.strength();
        f.focus = c.focus();
        f.range = c.attack_range();
        f.channel = c.is_recalling() || c.is_submitting_gems() || c.is_teleporting();
        out.push_back(f);
    }
    return out;
}

float power_of(const std::vector<Foe>& v, vec2 around, int radius) {
    float hp = 0, dps = 0;
    for (const auto& f : v) {
        if (dist(f.p, around) > radius) continue;
        hp += f.hp;
        dps += (f.str + 0.3f * f.focus) * (f.channel ? 0.3f : 1.0f);
    }
    return hp * dps;
}

extern bool g_allow_far;
int pick_group(int team_n, const std::array<int, 5>& taken) {
    int best = -1;
    float bs = 0;
    auto& me = cambc::self();
    const float team_dps = (me.strength() + 0.25f * me.focus()) * static_cast<float>(std::max(1, team_n));
    const float team_hp = me.max_health() * static_cast<float>(std::max(1, team_n));
    for (size_t i = 0; i < g_groups.size(); ++i) {
        const auto& g = g_groups[i];
        const int d = dist(g_pos, g.center);
        if (std::find(taken.begin(), taken.end(), static_cast<int>(i)) != taken.end()) continue;
        if (g.ready && g.ready > g_round + d) continue;
        const float turns = g.hp / team_dps;
        if (g.str * turns > 0.7f * team_hp) continue;
        if (g.boss && (g_round < 500 || team_n < 3)) continue;
        // Enemy side camps are where their champions get hurt: a mild bonus for aggression.
        const int de = dist(g.center, g_enemy_home), dh = home_dist(g.center);
        if (dh == 65535) continue;
        float value = static_cast<float>(g.gems * kGemWeight + g.gold) + (g.reward ? 40.0f : 0.0f);
        if (de + kEnemySideSlack < dh && !g_allow_far) continue;
        value *= de < dh ? 0.85f : 1.1f;
        const float s = value / (static_cast<float>(d) + turns + 6.0f);
        if (s > bs) { bs = s; best = static_cast<int>(i); }
    }
    return best;
}

bool g_allow_far = false;
int pick_group_any(int team_n, const std::array<int, 5>& taken) {
    g_allow_far = false;
    int t = pick_group(team_n, taken);
    if (t < 0) {
        g_allow_far = true;
        t = pick_group(team_n, taken);
        g_allow_far = false;
    }
    return t;
}

bool group_valid(int i) {
    if (i < 0 || i >= static_cast<int>(g_groups.size())) return false;
    const auto& g = g_groups[i];
    return !(g.ready && g.ready > g_round + dist(g_pos, g.center));
}

} // namespace

void turn_body_impl() {
    if (!g_init) init_map();
    auto& me = cambc::self();
    g_round = static_cast<int>(cambc::round_number());
    g_move_failed = false;
    g_attacked = false;

    if (!me.is_alive()) {
        if (g_was_alive) std::cout << "dbg died r" << g_round << " last: " << g_last << "\n";
        g_was_alive = false;
        shop();
        return;
    }
    g_was_alive = true;
    if (me.is_submitting_gems() || me.is_teleporting()) return;
    g_pos = me.position();
    const vec2 start = g_pos;
    const float hp = me.health(), max_hp = me.max_health();
    const bool hurt_now = hp < g_last_hp - 1;
    g_last_hp = hp;

    mem_load();
    update_groups();
    if (g_round >= kTraceFrom && g_round <= kTraceTo) std::cout << "dbg T r" << g_round << " c" << static_cast<int>(me.champion_name()) << " @" << g_pos.x << "," << g_pos.y << " last " << g_last << "\n";
    if (g_round % 100 == 1) std::cout << "dbg st r" << g_round << " pos " << g_pos.x << "," << g_pos.y << " hp " << hp << " gems " << me.gem_count() << " gold " << me.gold_count() << " tgt " << int(g_mem[0]) - 1 << "\n";
    const auto foes = visible_foes(team::enemy);
    const auto allies = visible_foes(team::ally);
    const bool at_spawn = g_ally_spawn.is_set(g_pos);
    const int my_range = me.attack_range();
    const bool melee = my_range <= 1;

    if (me.is_recalling()) {
        // Keep recalling unless a foe is about to interrupt it anyway and we could fight back.
        return;
    }
    if (at_spawn) shop();

    // Report enemy sightings to the pack.
    if (!foes.empty()) {
        g_foe_seen = g_round;
        int sx = 0, sy = 0;
        for (const auto& f : foes) { sx += f.p.x; sy += f.p.y; }
        const int n = static_cast<int>(foes.size());
        g_mem[2] = static_cast<std::uint8_t>(sx / n);
        g_mem[3] = static_cast<std::uint8_t>(sy / n);
        g_mem[4] = static_cast<std::uint8_t>(g_round & 255);
        mem_store();
    }

    // Place our altar lock on any unlocked altar within reach (blocks enemy tributes).
    try {
        auto lock = me.consumables()[cambc::consumable_type::altar_lock];
        if (lock.count() > 0)
            for (auto a : g_altars)
                if (dist(g_pos, a) <= 2 && !cambc::vision::has_altar_lock(a)) {
                    lock.use(a);
                    break;
                }
    } catch (const cambc::error&) {}

    // Heal up on spawn before leaving.
    if (at_spawn && hp < max_hp * 0.92f && foes.empty()) return;

    // ---- combat -----------------------------------------------------------------------------------
    int nearest_foe = 99;
    for (const auto& f : foes) nearest_foe = std::min(nearest_foe, dist(g_pos, f.p));
    struct Trace {
        ~Trace() {
            int awake_n = 0;
            try { awake_n = cambc::vision::awake_camp_tiles().count(); } catch (...) {}
            std::snprintf(g_last, sizeof g_last, "r%d %s hp%d foes%d nf%d al%d awake%d pos%d,%d", g_round, g_mode,
                hp, nf_count, nf, al, awake_n, g_pos.x, g_pos.y);
        }
        int hp, nf_count, nf, al;
    } trace{static_cast<int>(hp), static_cast<int>(foes.size()), nearest_foe, static_cast<int>(allies.size())};
    g_mode = "none";

    // Path distance to a tile within `r` of each foe; foes we cannot reach soon are not targets.
    int ti = -1;
    if (!foes.empty() && nearest_foe <= 9) {
        cambc::distance_field rf;
        bitboard rvis;
        bitboard root;
        root.clear();
        root.set(g_pos);
        const bitboard walls = blocked_now();
        rf.compute(walls, root, rvis, 10);
        float tscore = 1e9f;
        for (size_t i = 0; i < foes.size(); ++i) {
            const auto& f = foes[i];
            if (g_enemy_spawn_near.is_set(f.p)) continue;
            int pd = 99;
            const int r = my_range;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    const vec2 q{f.p.x + dx, f.p.y + dy};
                    if (q == g_pos) { pd = 0; continue; }
                    if (!cambc::map::is_in_bounds(q) || !rvis.is_set(q)) continue;
                    pd = std::min<int>(pd, rf.get(q));
                }
            // Do not chase: only foes that are close, busy channelling, or hurt enough to finish.
            const bool weak = f.hp < 0.35f * f.max_hp;
            // Ranged foes shooting us are worth closing on (they stand still to shoot); melee foes
            // two or more tiles away are baiting a chase into their team: let them come to us.
            const bool threat = f.range >= 2 && dist(f.p, g_pos) <= f.range + 1;
            if (pd > (f.channel ? 6 : weak ? 3 : threat ? 4 : 1)) continue;
            float s = f.hp + 80.0f * static_cast<float>(pd);
            if (f.channel) s -= 400.0f;
            if (s < tscore) { tscore = s; ti = static_cast<int>(i); }
        }
    }
    if (ti >= 0) {
        const float ours = power_of(allies, g_pos, 6);
        const float theirs = power_of(foes, g_pos, 9);
        const int gems = static_cast<int>(me.gem_count());
        float need = gems >= 25 ? kCarrierRatio : kEngageRatio;
        if (home_dist(g_pos) <= 8) need *= 0.6f;  // spawn heals us and tempts them
        const auto& t = foes[static_cast<size_t>(ti)];
        const float my_burst = me.strength() * 2.0f + me.focus() * 1.5f;
        const bool killable = t.hp <= my_burst && dist(g_pos, t.p) <= my_range + 1;
        const bool fight = (ours >= theirs * need && hp > max_hp * kFleeHp) || killable;
        if (g_round >= kTraceFrom && g_round <= kTraceTo) {
            std::cout << "dbg F r" << g_round << " c" << static_cast<int>(me.champion_name()) << " @" << g_pos.x << "," << g_pos.y << " hp" << int(hp) << (fight ? " FIGHT " : " flee ") << int(ours / 1000) << "/" << int(theirs / 1000) << " foes:";
            for (const auto& f : foes) std::cout << " " << f.p.x << "," << f.p.y << "h" << int(f.hp) << "r" << f.range;
            std::cout << " allies:";
            for (const auto& f : allies) std::cout << " " << f.p.x << "," << f.p.y << "h" << int(f.hp);
            std::cout << "\n";
        }
        if (false) std::cout << "dbg f r" << g_round << " " << static_cast<int>(me.champion_name()) << " a" << allies.size() << " e" << foes.size() << " d" << nearest_foe << " hp" << int(hp) << " t" << int(t.hp) << " " << (fight ? "FIGHT" : "flee") << " " << int(ours/1000) << "/" << int(theirs/1000) << "\n";
        if (fight) {
            g_mode = "fight";
            if (hurt_now || nearest_foe <= 3) use_shield();
            const int d0 = dist(g_pos, t.p);
            if (melee) {
                cast_offensive(t.p, true, true, start);
                if (d0 > 1) {
                    if (d0 <= 3) dash_towards(t.p, 1);
                    if (dist(g_pos, t.p) > 1) {
                        bitboard goal = ring(t.p, 1);
                        step_to(goal);
                    }
                }
                hit(t.p, true);
                cast_offensive(t.p, true, true, start);
            } else {
                if (d0 <= my_range) {
                    cast_offensive(t.p, true, true, start);
                    hit(t.p, true);
                } else {
                    // Out of range: spells first if they reach, else close in (dash keeps the attack).
                    if (!cast_offensive(t.p, true, true, start)) {
                        if (d0 <= my_range + 2) dash_towards(t.p, my_range);
                        if (dist(g_pos, t.p) > my_range) step_to(ring(t.p, my_range));
                        if (dist(g_pos, t.p) <= my_range && !moved_this_turn(start)) hit(t.p, true);
                    }
                }
            }
            if (false) std::cout << "dbg mv r" << g_round << " " << start.x << "," << start.y << " -> " << g_pos.x << "," << g_pos.y << " t " << t.p.x << "," << t.p.y << " att " << g_attacked << " mf " << g_move_failed << "\n";
            return;
        }
        g_mode = "flee";
        // Losing fight: punish whoever is in reach, stun the closest chaser, then back off.
        int ai = -1;
        for (size_t i = 0; i < foes.size(); ++i)
            if (dist(g_pos, foes[i].p) <= my_range && (ai < 0 || foes[i].hp < foes[static_cast<size_t>(ai)].hp))
                ai = static_cast<int>(i);
        int ci = -1;
        for (size_t i = 0; i < foes.size(); ++i)
            if (ci < 0 || dist(g_pos, foes[i].p) < dist(g_pos, foes[static_cast<size_t>(ci)].p)) ci = static_cast<int>(i);
        if (hurt_now || nearest_foe <= 3) use_shield();
        bool melee_chaser = false;
        for (const auto& f : foes) melee_chaser |= f.range <= 1 && dist(g_pos, f.p) <= 2;
        if (melee) {
            // A melee chaser at our speed hits us every turn anyway: trade back while retreating.
            if (melee_chaser) cast_offensive(foes[static_cast<size_t>(ci)].p, true, true, start);
            if (ai >= 0) hit(foes[static_cast<size_t>(ai)].p, true);
            if (!step_away(foes)) dash_away(foes);
            else if (nearest_foe <= 2) dash_away(foes);
        } else {
            if (nearest_foe <= 2) {
                if (!dash_away(foes)) step_away(foes);
            } else if (ai >= 0) {
                cast_offensive(foes[static_cast<size_t>(ai)].p, true, true, start);
                hit(foes[static_cast<size_t>(ai)].p, true);
            } else {
                step_away(foes);
            }
        }
        if (hp < max_hp * kRecallHp && nearest_foe >= 7 && me.can_move()) safe("recall", [&] { me.recall(); return true; });
        return;
    }

    // Unengaged but outmatched foes close by: keep our distance instead of walking into them.
    if (ti < 0 && nearest_foe <= kKeepAway) {
        const float ours = power_of(allies, g_pos, 6), theirs = power_of(foes, g_pos, 8);
        if (theirs > ours * kEngageRatio) {
            g_mode = "keepaway";
            if (step_away(foes)) return;
        }
    }

    // ---- recover -----------------------------------------------------------------------------------
    if (!at_spawn && hp < max_hp * kRecallHp) {
        // Walk out of camp range first: camp hits would cancel the recall.
        std::vector<Foe> camps;
        for (auto p : cambc::vision::awake_camp_tiles().positions())
            if (dist(p, g_pos) <= 6) camps.push_back(Foe{p, 0, 0, 0, 0, 4, false});
        if (!camps.empty()) {
            g_mode = "campexit";
            if (!step_away(camps)) dash_away(camps);
            return;
        }
    }
    if (!at_spawn && nearest_foe > 6 && (hp < max_hp * kRecallHp
            || (static_cast<int>(me.gold_count()) >= kShopGold && me.gem_count() < 10)
            || (static_cast<int>(me.gold_count()) >= kShopGold / 2 && hp < max_hp * 0.6f))) {
        g_mode = "home";
        // Close to spawn, walking is as fast as a recall and cannot be interrupted.
        // A foe seen lately is likely still following: a recall would only be interrupted.
        const bool shadowed = g_round - g_foe_seen <= kShadowRounds
;
        if ((home_dist(g_pos) <= kWalkHome || shadowed) && step_to(g_ally_spawn)) return;
        g_mode = "recall";
        safe("recall", [&] { me.recall(); return true; });
        return;
    }

    // ---- bank gems ---------------------------------------------------------------------------------
    const int gems = static_cast<int>(me.gem_count());
    if (gems > 0) {
        int nearest_altar = 99;
        for (auto a : g_altars) nearest_altar = std::min(nearest_altar, dist(g_pos, a));
        g_mode = "bank";
        if (gems >= kBankGems || nearest_altar <= kNearAltar || g_round > 1850) {
            for (auto a : g_altars)
                if (dist(g_pos, a) <= cambc::constants::submit_gems_range && nearest_foe > 5) {
                    safe("submit", [&] { me.submit_gems(a); return true; });
                    return;
                }
            bitboard goals;
            goals.clear();
            const bitboard enemy_locks = cambc::vision::altar_lock_tiles(team::enemy);
            for (auto a : g_altars)
                if (!enemy_locks.is_set(a)) goals |= ring(a, cambc::constants::submit_gems_range);
            if (goals.is_empty())
                for (auto a : g_altars) goals |= ring(a, cambc::constants::submit_gems_range);
            if (step_to(goals)) return;
        }
    }

    // ---- hunt a fresh pack sighting ------------------------------------------------------------------
    if (kHunt && (g_mem[4] != 0 || g_mem[2] != 0)) {
        const vec2 seen{g_mem[2], g_mem[3]};
        if (age(g_mem[4]) <= kHuntAge && dist(g_pos, seen) <= kHuntRange && hp > max_hp * 0.6f
            && static_cast<int>(allies.size()) >= 2) {
            if (step_to(ring(seen, 1))) return;
        }
    }

    // ---- farm with the pack --------------------------------------------------------------------------
    const int byte = kPackByte[static_cast<size_t>(g_pack)];
    int target = static_cast<int>(g_mem[static_cast<size_t>(byte)]) - 1;
    if (!group_valid(target)) {
        std::array<int, 5> taken{-1, -1, -1, -1, -1};
        for (int k = 0; k < 5; ++k)
            if (k != g_pack) taken[static_cast<size_t>(k)] = static_cast<int>(g_mem[static_cast<size_t>(kPackByte[static_cast<size_t>(k)])]) - 1;
        target = pick_group_any(kPackSize, taken);
        if (target < 0) target = pick_group_any(kPackSize, std::array<int, 5>{-1, -1, -1, -1, -1});
        g_mem[static_cast<size_t>(byte)] = static_cast<std::uint8_t>(target + 1);
        mem_store();
    }

    // Hit any awake camp in range first (they are hitting us), then target-group camps.
    const auto awake = cambc::vision::awake_camp_tiles();
    vec2 best_camp{-1, -1};
    float best_hp = 1e9f;
    const bitboard camp_tiles = cambc::vision::camp_tiles();
    for (auto p : camp_tiles.positions()) {
        const bool in_target = target >= 0 && g_groups[static_cast<size_t>(target)].tiles.is_set(p)
            ; // camps sit on their spawn tiles
        if (!awake.is_set(p) && !in_target) continue;
        const auto c = cambc::vision::camp_at(p);
        const float chp = c.health();
        const float s = chp + (in_target ? 0.0f : 500.0f);
        if (s < best_hp) { best_hp = s; best_camp = p; }
    }

    if (target >= 0) {
        auto& g = g_groups[static_cast<size_t>(target)];
        bitboard alive_tiles = g.tiles & camp_tiles;
        const bool visible = !(g.tiles & cambc::vision::visible_tiles()).is_empty();
        if (visible && alive_tiles.is_empty() && g.reward) {
            g_mode = "capture";
            // Capture: stand on a spawn tile of the group.
            if (best_camp.x >= 0 && dist(g_pos, best_camp) <= my_range) hit(best_camp, false);
            if (!g.tiles.is_set(g_pos)) step_to(g.tiles);
            return;
        }
        if (best_camp.x >= 0) {
            g_mode = "farm";
            const int d = dist(g_pos, best_camp);
            if (d <= my_range) {
                cast_offensive(best_camp, false, false, start);
                hit(best_camp, false);
                if (melee && !g_attacked) step_to(ring(best_camp, 1));
                return;
            }
            if (!melee) {
                cast_offensive(best_camp, false, false, start);
                if (!me.can_cast() && !moved_this_turn(start)) return;
            }
            step_to(ring(best_camp, my_range));
            if (dist(g_pos, best_camp) <= my_range && (melee || !moved_this_turn(start))) hit(best_camp, false);
            return;
        }
        g_mode = "approach";
        // Approach the group.
        if (!step_to(my_range >= 3 ? g.near3 : g.near1) && !step_to(ring(g.center, 3)) && !g_move_failed
            && me.can_move()) {
            // Unreachable from here (islands, blocked lanes): drop it for a while and pick again.
            g.ready = g_round + 25;
            g_mem[static_cast<size_t>(byte)] = 0;
            mem_store();
        }
        return;
    }

    // Nothing to farm: walk towards the middle of the map.
    step_to(ring(vec2{W / 2, H / 2}, 3));
}

void Bot::play_turn() {
    // A stale handle or a race with a hidden unit must not cost the game: log it and yield the turn.
    try {
        turn_body_impl();
    } catch (const std::exception& e) {
        std::cout << "dbg exc r" << g_round << " " << e.what() << "\n";
    }
}
