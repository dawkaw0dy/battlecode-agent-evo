// Champion decisions. main.cpp dispatches here after the draft.
// Altar control: one warden holds the altar the enemy banks at and locks it when an
// enemy starts a tribute there; everyone else farms, banks and locks opportunistically.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;

namespace {

// ---- tunables ----
constexpr int kWardenMinTeam = 3;     // warden only with at least this many champions
constexpr float kRecallHp = 0.45f;
constexpr float kHealTo = 0.9f;
constexpr unsigned kBankGems = 40;
constexpr unsigned kRecallGold = 180;
constexpr int kChaseRadius = 3;
constexpr bool kLockAll = true;      // lock every free altar we pass
constexpr float kGemWeight = 5.0f;   // gems vs gold in camp value
constexpr float kDangerExp = 1.5f;   // penalty for camps that would out-damage us

int distance(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }

struct GroupInfo {
    cambc::bitboard spawns;
    vec2 rep{0, 0};   // one spawn tile, to test visibility cheaply
    vec2 center{0, 0};
    float gems = 0, gold = 0, hp = 0, str = 0;
    bool monolith = false;
    bool reachable = true;
};

struct World {
    int w = 0, h = 0;
    std::vector<vec2> altars;
    std::vector<int> ally_dist, enemy_dist;   // path distance from each spawn to each altar
    std::vector<GroupInfo> groups;
    std::vector<cambc::map::camp_group> handles;
    std::vector<int> respawn_round;           // last known round the group is back
    std::vector<int> skip_until;              // capture failed (contested region): avoid until then
    cambc::bitboard terrain;                  // walls | holes | altars
    cambc::bitboard ally_spawn, enemy_spawn;
    int team_size = 0;
} world;

// Per-turn scratch.
const char* stage = "";
long long prof[8] = {0};
struct Prof {
    int slot;
    long long start;
    Prof(int s) : slot(s), start(cambc::fuel_remaining()) {}
    ~Prof() { prof[slot] += start - (long long)cambc::fuel_remaining(); }
};
bool moved = false;
bool move_blocked = false;

cambc::bitboard empty_board() {
    cambc::bitboard b;
    b.clear();
    return b;
}

cambc::bitboard single(vec2 p) {
    auto b = empty_board();
    b.set(p, true);
    return b;
}

// Tiles within radius, with our own bounds checks (draw_chebyshev can throw at the edge).
cambc::bitboard ring(vec2 c, int r, int min_r = 0) {
    auto b = empty_board();
    if (min_r == 0 && c.x - r >= 0 && c.y - r >= 0 && c.x + r < world.w && c.y + r < world.h) {
        b.draw_chebyshev(c, r);
        return b;
    }
    for (int dx = -r; dx <= r; ++dx)
        for (int dy = -r; dy <= r; ++dy) {
            vec2 p{c.x + dx, c.y + dy};
            if (p.x < 0 || p.y < 0 || p.x >= world.w || p.y >= world.h) continue;
            if (std::max(std::abs(dx), std::abs(dy)) < min_r) continue;
            b.set(p, true);
        }
    return b;
}

cambc::bitboard blocked_cache;
int blocked_round = -1;
vec2 blocked_pos{-1, -1};
cambc::bitboard blocked_tiles() {
    Prof pf(4);
    const vec2 me_pos = cambc::self().position();
    if (blocked_round == cambc::round_number() && blocked_pos == me_pos) return blocked_cache;
    auto blocked = world.terrain | world.enemy_spawn | cambc::vision::champion_tiles()
        | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    // Allies hidden in bushes are still known to the engine.
    try {
        for (const auto& a : cambc::champions(team::ally)) {
            try {
                if (a.is_alive()) blocked.set(a.position(), true);
            } catch (...) {
            }
        }
    } catch (...) {
    }
    blocked.set(me_pos, false);
    blocked_cache = blocked;
    blocked_round = cambc::round_number();
    blocked_pos = me_pos;
    return blocked;
}

// One step towards the nearest goal tile, only onto free tiles the field reached.
bool step_towards(cambc::bitboard goals) {
    Prof pf(1);
    auto& me = cambc::self();
    if (move_blocked || moved || !me.can_move()) return false;
    const vec2 pos = me.position();
    if (goals.is_set(pos)) return false;
    const auto blocked = blocked_tiles();
    goals &= ~blocked | cambc::vision::camp_tiles() | cambc::vision::champion_tiles();
    goals.set(pos, false);
    if (goals.is_empty()) return false;

    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute_to(blocked & ~goals, goals, single(pos), visited);
    if (!visited.is_set(pos)) {
        // Units block every path: plan on terrain only and take any free improving step.
        auto walls = world.terrain | world.enemy_spawn;
        walls.set(pos, false);
        field.compute_to(walls & ~goals, goals, single(pos), visited);
        if (!visited.is_set(pos)) return false;
    }
    // Recompute: the field computation can clobber the earlier mask.
    const auto blocked_now = blocked_tiles();
    int best_d = field.get(pos);
    vec2 best = pos;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            if (!dx && !dy) continue;
            vec2 n{pos.x + dx, pos.y + dy};
            if (n.x < 0 || n.y < 0 || n.x >= world.w || n.y >= world.h) continue;
            if (blocked_now.is_set(n) || !visited.is_set(n)) continue;
            const int d = field.get(n);
            if (d < best_d) { best_d = d; best = n; }
        }
    if (best == pos) return false;
    try {
        if (me.move_absolute(best)) {
            moved = true;
            return true;
        }
    } catch (const std::exception& e) {
        std::cout << "dbg move " << e.what() << " r" << cambc::round_number() << " " << pos.x << "," << pos.y
                  << " -> " << best.x << "," << best.y << " champ=" << cambc::vision::has_champion(best)
                  << " camp=" << cambc::vision::has_camp(best) << " bush=" << cambc::map::bushes().is_set(best)
                  << " wall=" << cambc::map::walls().is_set(best) << " aspawn=" << world.ally_spawn.is_set(best) << " blk=" << blocked.is_set(best) << " ct=" << cambc::vision::champion_tiles().is_set(best) << " vis=" << cambc::vision::visible_tiles().is_set(best)
                  << '\n';
    }
    move_blocked = true;
    return false;
}

// Terrain-only fields towards fixed goals, cached by key so long trips cost one field.
struct CachedField {
    int key = -1;
    cambc::distance_field field;
    cambc::bitboard visited;
};
std::array<CachedField, 10> field_cache;
int field_cache_next = 0;

CachedField& cached_field(int key, const cambc::bitboard& goals) {
    Prof pf(0);
    for (auto& c : field_cache)
        if (c.key == key) return c;
    auto& c = field_cache[field_cache_next];
    field_cache_next = (field_cache_next + 1) % int(field_cache.size());
    c.key = key;
    auto walls = world.terrain | world.enemy_spawn;
    walls &= ~goals;
    c.field.compute(walls, goals, c.visited);
    return c;
}

// Step along a cached terrain field; near the goal (or when units block) plan around units.
bool step_cached(int key, const cambc::bitboard& goals, int near = 6) {
    auto& me = cambc::self();
    if (move_blocked || moved || !me.can_move()) return false;
    const vec2 pos = me.position();
    if (goals.is_set(pos)) return false;
    auto& c = cached_field(key, goals);
    if (!c.visited.is_set(pos)) return step_towards(goals);
    const int here = c.field.get(pos);
    if (here <= near) return step_towards(goals);
    const auto blocked = blocked_tiles();
    int best_d = here;
    vec2 best = pos;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            if (!dx && !dy) continue;
            vec2 n{pos.x + dx, pos.y + dy};
            if (n.x < 0 || n.y < 0 || n.x >= world.w || n.y >= world.h) continue;
            if (blocked.is_set(n) || !c.visited.is_set(n)) continue;
            const int d = c.field.get(n);
            if (d < best_d) { best_d = d; best = n; }
        }
    if (best == pos) return step_towards(goals);
    try {
        if (me.move_absolute(best)) {
            moved = true;
            return true;
        }
    } catch (...) {
    }
    move_blocked = true;
    return false;
}

// Path distance field from one tile over terrain.
void field_from(vec2 pos, cambc::distance_field& field, cambc::bitboard& visited, std::uint16_t max_d = 0xFFFF) {
    auto walls = world.terrain | world.enemy_spawn;
    walls.set(pos, false);
    field.compute(walls, single(pos), visited, max_d);
}

int altar_dist(const cambc::distance_field& field, const cambc::bitboard& visited, vec2 altar) {
    int best = 1 << 20;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            vec2 n{altar.x + dx, altar.y + dy};
            if (n.x < 0 || n.y < 0 || n.x >= world.w || n.y >= world.h) continue;
            if (visited.is_set(n)) best = std::min(best, int(field.get(n)));
        }
    return best;
}

int group_dist(const cambc::distance_field& field, const cambc::bitboard& visited, const GroupInfo& g) {
    int best = 1 << 20;
    const auto all = g.spawns.positions();
    std::vector<vec2> sample;
    if (all.size() <= 4) sample = all;
    else sample = {all.front(), all[all.size() / 3], all[2 * all.size() / 3], all.back()};
    for (auto t : sample) {
        if (visited.is_set(t)) best = std::min(best, int(field.get(t)));
        else
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    vec2 n{t.x + dx, t.y + dy};
                    if (n.x < 0 || n.y < 0 || n.x >= world.w || n.y >= world.h) continue;
                    if (visited.is_set(n)) best = std::min(best, int(field.get(n)) + 1);
                }
    }
    return best;
}

const cambc::ability_properties* props(const cambc::ability_info& info) {
    return std::visit([](const auto& a) -> const cambc::ability_properties* { return &a; }, info);
}

struct Enemy {
    vec2 pos;
    float hp, max_hp;
    unsigned gems;
    bool submitting;
    bool at_altar;
};

std::vector<Enemy> visible_enemies() {
    Prof pf(2);
    std::vector<Enemy> out;
    for (auto p : cambc::vision::champion_tiles(team::enemy).positions()) {
        try {
            auto c = cambc::vision::champion_at(p);
            if (!c.is_visible() || !c.is_alive()) continue;
            Enemy e{p, c.health(), c.max_health(), 0, false, false};
            try { e.gems = c.gem_count(); } catch (...) {}
            try { e.submitting = c.is_submitting_gems(); } catch (...) {}
            for (auto a : world.altars)
                if (distance(a, p) <= 1) e.at_altar = true;
            out.push_back(e);
        } catch (...) {
        }
    }
    return out;
}

bool has_lock_item() {
    try {
        return cambc::self().consumables()[cambc::consumable_type::altar_lock].count() > 0;
    } catch (...) {
        return false;
    }
}

bool place_lock(vec2 altar) {
    auto& me = cambc::self();
    if (!has_lock_item() || distance(me.position(), altar) > 2) return false;
    if (cambc::vision::has_altar_lock(altar)) return false;
    if (me.restrictions().contains(cambc::buff_restrictions::consumable)) return false;
    try {
        return me.consumables()[cambc::consumable_type::altar_lock].use(altar);
    } catch (...) {
        return false;
    }
}

bool enemy_lock_at(vec2 altar) {
    if (!cambc::vision::has_altar_lock(altar)) return false;
    try {
        return cambc::vision::altar_lock_at(altar).is_enemy();
    } catch (...) {
        return false;
    }
}

bool at_ally_spawn() { return world.ally_spawn.is_set(cambc::self().position()); }

// Basic attack an enemy champion in range; prefer the ones tributing or carrying gems.
bool attack_enemy(const std::vector<Enemy>& enemies) {
    auto& me = cambc::self();
    if (!me.can_attack() || enemies.empty()) return false;
    const auto tiles = me.attack_tiles();
    const Enemy* best = nullptr;
    float best_score = 1e9f;
    for (const auto& e : enemies) {
        if (!tiles.is_set(e.pos)) continue;
        float s = e.hp - 4.0f * e.gems - (e.submitting ? 400.0f : 0.0f) - (e.at_altar ? 150.0f : 0.0f);
        if (s < best_score) { best_score = s; best = &e; }
    }
    if (!best) return false;
    try {
        auto c = cambc::vision::champion_at(best->pos);
        if (!c.is_visible()) return false;
        me.attack(c);
        return true;
    } catch (...) {
        return false;
    }
}

// Attack-reset right after our own basic attack, then attack again.
void try_reset(const std::vector<Enemy>* enemies) {
    auto& me = cambc::self();
    if (me.can_attack() || !me.can_cast()) return;
    for (auto& ab : me.abilities()) {
        try {
            const auto& info = ab.info();
            if (!info || !std::holds_alternative<cambc::ability_attack_reset>(*info)) continue;
            if (!ab.is_ready()) continue;
            if (!ab.use()) continue;
            if (enemies) attack_enemy(*enemies);
            return;
        } catch (...) {
            return;
        }
    }
}

// Cast one useful spell at an enemy champion (or a camp when no champion target is given).
bool cast_at(const std::vector<Enemy>& enemies, const cambc::camp* camp_target) {
    auto& me = cambc::self();
    if (!me.can_cast()) return false;
    const vec2 pos = me.position();
    for (auto& ab : me.abilities()) {
        try {
            const auto& info = ab.info();
            if (!info || !ab.is_ready()) continue;
            const auto* pr = props(*info);
            if (pr->exclusive_with_movement && moved) continue;
            if (auto* d = std::get_if<cambc::ability_damage>(&*info)) {
                const int range = int(d->range);
                for (const auto& e : enemies)
                    if (distance(pos, e.pos) <= range) {
                        auto c = cambc::vision::champion_at(e.pos);
                        if (c.is_visible() && ab.use(c)) return true;
                    }
                if (camp_target && distance(pos, camp_target->position()) <= range && ab.use(*camp_target))
                    return true;
            } else if (auto* cc = std::get_if<cambc::ability_cc>(&*info)) {
                const int range = int(cc->range);
                // Stun a tributing enemy first (cancels the channel), else the weakest in range.
                const Enemy* best = nullptr;
                for (const auto& e : enemies)
                    if (distance(pos, e.pos) <= range && (!best || (e.submitting && !best->submitting)
                            || (e.submitting == best->submitting && e.hp < best->hp)))
                        best = &e;
                if (best) {
                    auto c = cambc::vision::champion_at(best->pos);
                    if (c.is_visible() && ab.use(c)) return true;
                } else if (camp_target && cc->modifier > 0.0f && distance(pos, camp_target->position()) <= range
                           && ab.use(*camp_target))
                    return true;
            } else if (auto* aoe = std::get_if<cambc::ability_aoe_damage>(&*info)) {
                const int range = int(aoe->range);
                for (const auto& e : enemies)
                    if (distance(pos, e.pos) <= range && ab.use(e.pos)) return true;
                if (camp_target && distance(pos, camp_target->position()) <= range
                    && ab.use(camp_target->position()))
                    return true;
            } else if (std::holds_alternative<cambc::ability_shield>(*info)) {
                if (me.health() < me.max_health() * 0.8f && (!enemies.empty() || camp_target) && ab.use())
                    return true;
            } else if (auto* heal = std::get_if<cambc::ability_heal>(&*info)) {
                const int range = int(heal->range);
                for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
                    if (p == pos || distance(p, pos) > range) continue;
                    auto c = cambc::vision::champion_at(p);
                    if (c.is_visible() && c.health() < c.max_health() * 0.6f && ab.use(c)) return true;
                }
            }
        } catch (...) {
        }
    }
    return false;
}

void read_claims(std::array<std::uint8_t, 8>& claims) {
    try {
        cambc::shared_memory.read(0, std::span{claims.data(), claims.size()});
    } catch (...) {
        claims.fill(0);
    }
}

// Step away from awake camps that can hit us; true if we are (still) in their reach.
bool retreat_from_camps() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    std::vector<std::pair<vec2, int>> threats;
    for (auto p : (cambc::vision::camp_tiles() & ring(pos, 7)).positions()) {
        try {
            auto c = cambc::vision::camp_at(p);
            if (c.is_visible() && c.is_alive() && c.is_awake()) threats.push_back({p, c.attack_range()});
        } catch (...) {
        }
    }
    auto margin = [&](vec2 t) {
        int m = 99;
        for (auto& [cp, r] : threats) m = std::min(m, distance(t, cp) - r);
        return m;
    };
    if (threats.empty() || margin(pos) > 0) return false;
    if (moved || move_blocked || !me.can_move()) return true;
    const auto blocked = blocked_tiles();
    vec2 best = pos;
    int best_m = margin(pos);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            vec2 n{pos.x + dx, pos.y + dy};
            if ((!dx && !dy) || n.x < 0 || n.y < 0 || n.x >= world.w || n.y >= world.h || blocked.is_set(n)) continue;
            const int m = margin(n);
            if (m > best_m) { best_m = m; best = n; }
        }
    if (best == pos) return false;   // cornered: recall anyway
    try {
        if (me.move_absolute(best)) moved = true;
        else move_blocked = true;
    } catch (...) {
        move_blocked = true;
    }
    return true;
}

}  // namespace

void Bot::init() {
    initialised = true;
    auto& me = cambc::self();
    world.w = cambc::map::width();
    world.h = cambc::map::height();
    world.terrain = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    world.ally_spawn = cambc::map::spawn_region(team::ally);
    world.enemy_spawn = cambc::map::spawn_region(team::enemy);
    world.altars = cambc::map::altars().positions();

    {
        cambc::distance_field f;
        cambc::bitboard v;
        f.compute(world.terrain | world.enemy_spawn, world.ally_spawn, v);
        for (auto a : world.altars) world.ally_dist.push_back(altar_dist(f, v, a));
        cambc::distance_field g;
        cambc::bitboard u;
        g.compute(world.terrain | world.ally_spawn, world.enemy_spawn, u);
        for (auto a : world.altars) world.enemy_dist.push_back(altar_dist(g, u, a));
    }

    for (const auto& group : cambc::map::camp_groups()) {
        GroupInfo gi;
        gi.spawns = group.spawn_tiles();
        gi.gems = float(group.gem_value());
        gi.gold = float(group.gold_value());
        int n = 0, sx = 0, sy = 0;
        for (const auto& c : group.camps()) {
            try {
                const auto st = c.base_stats();
                gi.hp += st.base_health;
                gi.str += st.base_strength;
                if (c.is_monolith() || st.base_health >= 10000) gi.monolith = true;
                const auto sp = c.spawn_position();
                sx += sp.x;
                sy += sp.y;
                ++n;
            } catch (...) {
            }
        }
        if (n) gi.center = vec2{sx / n, sy / n};
        const auto tiles = gi.spawns.positions();
        if (!tiles.empty()) gi.rep = tiles[tiles.size() / 2];
        world.groups.push_back(gi);
        world.handles.push_back(group);
    }
    world.respawn_round.assign(world.groups.size(), 0);
    world.skip_until.assign(world.groups.size(), 0);

    const auto allies = cambc::champions(team::ally);
    world.team_size = int(allies.size());
    int vendetta = -1;
    for (int i = 0; i < int(allies.size()); ++i) {
        if (allies[i] == me) my_index = i;
        try {
            if (allies[i].champion_name() == cambc::champion_type::vendetta) vendetta = i;
        } catch (...) {
        }
    }
    const int warden_index = vendetta >= 0 ? vendetta : world.team_size - 1;
    warden = world.team_size >= kWardenMinTeam && my_index == warden_index;
    last_health = me.health();
    {
        cambc::distance_field f;
        cambc::bitboard v;
        f.compute(world.terrain | world.enemy_spawn, world.ally_spawn, v);
        int n = 0;
        for (auto& g : world.groups) n += (g.reachable = group_dist(f, v, g) < (1 << 20));
        (void)n;
    }
}

void Bot::buy_items() {
    auto& me = cambc::self();
    try {
        if (!has_lock_item() && me.gold_count() >= 10 && me.can_buy(cambc::consumable_type::altar_lock))
            me.buy(cambc::consumable_type::altar_lock);
    } catch (...) {
    }
    const bool ryth = me.champion_name() == cambc::champion_type::ryth;
    const auto item = ryth ? cambc::item_type::power_shard_of_health : cambc::item_type::power_shard_of_strength;
    try {
        unsigned count = me.gold_count() / item.cost();
        while (count && !me.can_buy(item, count)) --count;
        if (count) me.buy(item, count);
    } catch (...) {
    }
}

// Engage enemy champions that are close; true when the turn is spent fighting.
bool Bot::fight() {
    auto& me = cambc::self();
    const auto enemies = visible_enemies();
    if (enemies.empty()) return false;
    const vec2 pos = me.position();
    int nearest = 99;
    for (const auto& e : enemies) nearest = std::min(nearest, distance(pos, e.pos));
    if (nearest > kChaseRadius + 1) return false;

    // Opportunistic lock: an enemy tributing at an altar within our reach.
    for (const auto& e : enemies)
        if (e.submitting)
            for (auto a : world.altars)
                if (distance(a, e.pos) <= 1 && distance(pos, a) <= 2 && place_lock(a)) break;

    cast_at(enemies, nullptr);
    if (attack_enemy(enemies)) {
        try_reset(&enemies);
        return true;
    }
    // Close in if healthy, otherwise let the caller continue.
    if (me.health() < me.max_health() * 0.5f) return false;
    const bool melee = me.attack_range() <= 1;
    auto goals = empty_board();
    for (const auto& e : enemies)
        if (distance(pos, e.pos) <= kChaseRadius + (melee ? 0 : 1)) goals |= ring(e.pos, melee ? 1 : 2);
    if (goals.is_empty()) return false;
    if (step_towards(goals)) {
        if (attack_enemy(enemies)) try_reset(&enemies);
        return true;
    }
    return false;
}

// Walk to an altar and tribute; break enemy locks in the way.
bool Bot::bank_gems() {
    auto& me = cambc::self();
    const unsigned gems = me.gem_count();
    if (!gems) return false;
    const vec2 pos = me.position();
    int best = -1, best_d = 1 << 20, best_cost = 1 << 20;
    bool any_free = false;
    for (int i = 0; i < int(world.altars.size()); ++i)
        if (!enemy_lock_at(world.altars[i])) any_free = true;
    for (int i = 0; i < int(world.altars.size()); ++i) {
        const vec2 a = world.altars[i];
        if (world.ally_dist[i] >= (1 << 20)) continue;   // not reachable from our spawn
        int d = 0;
        if (distance(pos, a) > 1) {
            auto& c = cached_field(1000 + i, ring(a, 1) & ~world.terrain);
            d = c.visited.is_set(pos) ? int(c.field.get(pos)) : 1 << 20;
        }
        if (d >= (1 << 20)) continue;
        int cost = d;
        if (enemy_lock_at(a)) cost += any_free ? 1000 : 6;
        if (world.enemy_dist[i] + 4 < world.ally_dist[i]) cost += 6;   // deep in their half
        if (cost < best_cost) { best_cost = cost; best = i; best_d = d; }
    }
    if (best < 0) return false;
    const bool want = gems >= kBankGems || best_d <= 6 || cambc::round_number() > 1850
        || (gems >= 15 && me.health() < me.max_health() * 0.5f);
    if (!want) return false;

    const vec2 altar = world.altars[best];
    if (distance(pos, altar) <= cambc::constants::submit_gems_range) {
        if (enemy_lock_at(altar)) {
            try {
                if (me.can_attack()) {
                    auto lock = cambc::vision::altar_lock_at(altar);
                    if (me.attack_tiles().is_set(altar)) {
                        me.attack(lock);
                        try_reset(nullptr);
                        if (me.can_attack() && cambc::vision::has_altar_lock(altar)) {
                            auto again = cambc::vision::altar_lock_at(altar);
                            me.attack(again);
                        }
                    }
                }
            } catch (...) {
            }
            return true;
        }
        // Guard our channel: our own lock stops theirs from cancelling it.
        if (!cambc::vision::has_altar_lock(altar) && (kLockAll || !visible_enemies().empty())) place_lock(altar);
        try {
            me.submit_gems(altar);
        } catch (...) {
        }
        return true;
    }
    step_cached(1000 + best, ring(altar, 1) & ~world.terrain);
    return true;
}

int Bot::choose_group() {
    Prof pf(3);
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const int now = cambc::round_number();
    cambc::distance_field f;
    cambc::bitboard v;
    constexpr int kScan = 36;
    field_from(pos, f, v, kScan);
    std::array<std::uint8_t, 8> claims{};
    read_claims(claims);
    const float dps = std::max(20.0f, me.strength());
    int best = -1;
    float best_score = 0;
    for (int g = 0; g < int(world.groups.size()); ++g) {
        const auto& gi = world.groups[g];
        if (gi.monolith || gi.spawns.is_empty() || !gi.reachable) continue;
        const int cheb = distance(pos, gi.rep);
        int d = cheb <= kScan ? group_dist(f, v, gi) : 1 << 20;
        if (d >= (1 << 20)) d = std::max(kScan, cheb * 3 / 2);   // beyond the scan: estimate
        int helpers = 0;
        for (int i = 0; i < world.team_size && i < 8; ++i)
            if (i != my_index && claims[i] == g + 1) ++helpers;
        const float clear = gi.hp / (dps * (1 + helpers));
        // Camps hit the nearest champion: expect to tank most of it ourselves.
        const float dmg = gi.str * clear / (1 + helpers);
        const int wait = world.respawn_round[g] - now;
        float score = (kGemWeight * gi.gems + gi.gold) / (std::max(d, wait) + clear + 6.0f);
        const float safe = me.max_health() * 0.8f;
        if (dmg > safe) score *= std::pow(safe / dmg, kDangerExp);
        if (helpers && gi.hp < 3000) score *= 0.5f;
        if (world.skip_until[g] > now) score *= 0.01f;
        if (g == target_group) score *= 1.3f;
        if (score > best_score) { best_score = score; best = g; }
    }
    return best;
}

void Bot::farm() {
    Prof pf(5);
    auto& me = cambc::self();
    const int now = cambc::round_number();
    const auto visible = cambc::vision::visible_tiles();

    // Track respawn timers of visible groups.
    if (now % 3 == 0) try {
        for (int g = 0; g < int(world.handles.size()); ++g) {
            if (!visible.is_set(world.groups[g].rep)) continue;
            const auto st = world.handles[g].state();
            if (auto* r = std::get_if<cambc::map::camp_group_respawning>(&st))
                // A timer at zero means an occupied spawn tile blocks the respawn: look elsewhere.
                world.respawn_round[g] = now + (r->timer > 0 ? r->timer : 60);
            else
                world.respawn_round[g] = 0;
        }
    } catch (...) {
    }

    // No gold, gems or damage dealt for a long time: the target is not working out.
    const unsigned wealth = me.gold_count() * 1000 + me.gem_count();
    if (wealth != last_wealth) { last_wealth = wealth; progress_round = now; }
    if (target_group >= 0 && now - progress_round > 30) {
        world.skip_until[target_group] = now + 150;
        progress_round = now;
        target_group = -1;
    }
    if (target_group < 0 || now - retarget_round >= 8) {
        target_group = choose_group();
        retarget_round = now;
        try {
            std::uint8_t claim = std::uint8_t(target_group + 1);
            if (my_index < 8) cambc::shared_memory.write(my_index, std::span{&claim, 1});
        } catch (...) {
        }
    }

    // Attack camps in range: the target group first, lowest health first.
    auto attack_camp = [&]() -> bool {
        if (!me.can_attack() && !me.can_cast()) return false;
        const auto tiles = me.attack_tiles();
        vec2 best{-1, -1};
        float best_hp = 1e9f;
        const vec2 pos = me.position();
        for (auto p : (cambc::vision::camp_tiles() & ring(pos, 4)).positions()) {
            try {
                auto c = cambc::vision::camp_at(p);
                if (!c.is_visible() || !c.is_alive()) continue;
                const bool ours = target_group >= 0 && world.groups[target_group].spawns.is_set(p);
                if (!ours && !c.is_awake()) continue;
                if (!tiles.is_set(p) && distance(pos, p) > 4) continue;
                float hp = c.health() - (ours ? 0 : 0) + (tiles.is_set(p) ? 0 : 5000);
                if (hp < best_hp) { best_hp = hp; best = p; }
            } catch (...) {
            }
        }
        if (best.x < 0) return false;
        bool acted = false;
        try {
            auto c = cambc::vision::camp_at(best);
            if (visible_enemies().empty()) acted = cast_at({}, &c);
            if (cambc::vision::has_camp(best)) {
                auto c2 = cambc::vision::camp_at(best);
                if (c2.is_visible() && c2.is_alive() && me.can_attack() && me.attack_tiles().is_set(best)) {
                    me.attack(c2);
                    acted = true;
                    progress_round = now;
                    try_reset(nullptr);
                    if (me.can_attack() && cambc::vision::has_camp(best)) {
                        auto c3 = cambc::vision::camp_at(best);
                        if (c3.is_visible() && c3.is_alive()) me.attack(c3);
                    }
                }
            }
        } catch (...) {
        }
        return acted;
    };

    const bool ranged = me.attack_range() > 1;
    if (attack_camp() && ranged) return;
    if (target_group < 0) return;
    const auto& gi = world.groups[target_group];
    const bool respawning = world.respawn_round[target_group] > now;


    if (respawning) {
        // Never stand on spawn tiles of a respawning group: that delays the respawn.
        cambc::bitboard around = empty_board();
        for (auto t : gi.spawns.positions()) around |= ring(t, 2);
        around &= ~gi.spawns & ~world.terrain & ~world.enemy_spawn;
        progress_round = now;   // waiting for a respawn is not being stuck
        if (around.is_set(me.position())) return;
        step_cached(2000 + target_group, around);
        return;
    }
    const cambc::bitboard live = cambc::vision::camp_tiles() & gi.spawns;
    if (gi.spawns.is_set(me.position()) && live.is_empty()) {
        // Capturing; a capture that never completes is contested elsewhere in the region.
        if (++capture_wait <= 12) return;
        world.skip_until[target_group] = now + 150;
        capture_wait = 0;
        target_group = choose_group();
        retarget_round = now;
        return;
    }
    capture_wait = 0;
    const bool moved_now = live.is_empty() ? step_cached(target_group, gi.spawns) : step_towards(live);
    if (moved_now) attack_camp();
}

// Warden: keep a lock, wait beside the enemy's altar, lock it when they tribute.
bool Bot::warden_turn() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    if (world.altars.empty()) return false;

    // Altar the enemy reaches first.
    int post = 0;
    for (int i = 1; i < int(world.altars.size()); ++i)
        if (world.enemy_dist[i] - world.ally_dist[i] < world.enemy_dist[post] - world.ally_dist[post]) post = i;
    const vec2 altar = world.altars[post];

    const auto enemies = visible_enemies();
    // Lock any altar in reach where an enemy is tributing or about to.
    for (const auto& e : enemies)
        for (auto a : world.altars)
            if (distance(a, e.pos) <= 1 && (e.submitting || e.gems >= 5) && distance(pos, a) <= 2 && place_lock(a))
                break;

    if (!has_lock_item()) {
        if (me.gold_count() >= 10 && !at_ally_spawn() && enemies.empty() && me.gem_count() == 0) {
            try {
                me.recall();
                return true;
            } catch (...) {
            }
        }
        return false;
    }
    // Fight near the post (punish lock breakers), then hold the post.
    if (fight()) return true;
    if (me.gem_count() >= 10 || cambc::round_number() > 1850) {
        if (bank_gems()) return true;
    }
    auto goals = ring(altar, 2, 2) & ~world.terrain & ~world.enemy_spawn;
    auto bushes = goals & cambc::map::bushes();
    if (!bushes.is_empty()) goals = bushes;
    if (goals.is_set(pos)) {
        // Farm a camp right beside the post, otherwise wait.
        try {
            if (me.ward_cooldown() == 0) {
                auto wt = me.ward_placement_tiles();
                if (wt.is_set(altar) == false) {
                    auto near = ring(altar, 1) & wt & ~world.terrain;
                    if (!near.is_empty()) me.place_ward(near.positions()[0]);
                }
            }
        } catch (...) {
        }
        return true;
    }
    step_cached(3000 + post, goals);
    return true;
}

void Bot::play_turn() {
    try {
        turn();
    } catch (const std::exception& e) {
        std::cout << "dbg exc " << stage << " r" << cambc::round_number() << " " << e.what() << '\n';
    }
    if (cambc::round_number() % 25 == 3) std::cout << "dbg endfuel " << cambc::fuel_remaining() << '\n';
    if (cambc::round_number() == 1000) {
        std::cout << "dbg prof";
        for (auto v : prof) std::cout << ' ' << v;
        std::cout << '\n';
    }
}

void Bot::turn() {
    Prof pf(6);
    moved = false;
    move_blocked = false;
    auto& me = cambc::self();
    stage = "init";
    if (!initialised) init();
    stage = "pre";

    if (!me.is_alive()) {
        buy_items();
        return;
    }
    const float hp = me.health();
    if (hp < last_health) last_hit_round = cambc::round_number();
    last_health = hp;

    if (me.is_recalling() || me.is_teleporting()) return;
    if (me.is_submitting_gems()) {
        // Hit anything in range while the channel runs? Actions are blocked during channels.
        return;
    }
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) return;

    const bool spawn = at_ally_spawn();
    if (spawn) buy_items();

    const auto enemies = visible_enemies();
    if (spawn && hp < me.max_health() * kHealTo) {
        if (!enemies.empty()) {
            cast_at(enemies, nullptr);
            if (attack_enemy(enemies)) try_reset(&enemies);
        }
        return;
    }
    if (!spawn && hp < me.max_health() * kRecallHp) {
        if (!enemies.empty()) {
            cast_at(enemies, nullptr);
            if (attack_enemy(enemies)) try_reset(&enemies);
        }
        // Damage cancels a recall: first walk out of reach of awake camps.
        if (retreat_from_camps()) return;
        try {
            me.recall();
        } catch (...) {
        }
        return;
    }
    if (!spawn && me.gem_count() == 0 && me.gold_count() >= kRecallGold && enemies.empty()) {
        try {
            me.recall();
            return;
        } catch (...) {
        }
    }

    if (cambc::round_number() % 50 == 2)
        std::cout << "dbg fuel " << cambc::fuel_remaining() << " r" << cambc::round_number() << " i" << my_index << " w" << warden << " pos " << me.position().x
                  << "," << me.position().y << " tg " << target_group << " gems " << me.gem_count() << " gold "
                  << me.gold_count() << '\n';
    // Lock any free altar in reach: our team still tributes there, theirs must break it.
    if (kLockAll && has_lock_item()) {
        int best = -1;
        for (int i = 0; i < int(world.altars.size()); ++i) {
            const vec2 a = world.altars[i];
            if (distance(me.position(), a) > 2 || cambc::vision::has_altar_lock(a)) continue;
            if (best < 0 || world.enemy_dist[i] < world.enemy_dist[best]) best = i;
        }
        if (best >= 0) place_lock(world.altars[best]);
    }
    stage = "warden";
    if (warden && warden_turn()) return;
    stage = "fight";
    if (fight()) return;
    stage = "bank";
    if (bank_gems()) return;
    stage = "farm";
    farm();
}
