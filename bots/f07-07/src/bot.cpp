// Early aggression: the whole team moves as one pack, farms camps together on the way to the
// enemy's side, and strikes lone or hurt enemy champions it meets. One champion leads; the
// others follow its position, which it broadcasts through the 8 bytes of shared memory.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>
#include <variant>

using cambc::team;
using cambc::vec2;

namespace {

constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
                                    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

// Champion kits by cambc::champion_type: attack resets, spell damage as a multiple of focus per
// eight-turn cycle, and whether one slot stuns.
constexpr std::array<int, 13> kResets{1, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0};
constexpr std::array<float, 13> kSpellMult{1.0f, 0, 0, 2.0f, 0, 3.4f, 2.5f, 2.0f, 0, 0.75f, 0, 1.4f, 1.5f};
constexpr std::array<int, 13> kStuns{1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 1};

constexpr float kEngageRatio = 0.85f;  // fight when our square-law strength is at least this share of theirs
constexpr float kRecallHp = 0.35f;
constexpr int kBankGems = 30;
constexpr int kChaseTurns = 6;

int cheb(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }

bool in_map(vec2 p, int w, int h) { return p.x >= 0 && p.y >= 0 && p.x < w && p.y < h; }

int warn_count = 0;
void warn(const char* where, const std::exception& e) {
    if (warn_count++ < 20) std::printf("warn: %s r%u %s\n", where, cambc::round_number(), e.what());
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------

void Bot::init() {
    auto& me = cambc::self();
    width = cambc::map::width();
    height = cambc::map::height();
    ally_spawn = cambc::map::spawn_region(team::ally);
    enemy_spawn = cambc::map::spawn_region(team::enemy);
    static_block = cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | enemy_spawn;

    auto centre = [](const std::vector<vec2>& tiles) {
        long sx = 0, sy = 0;
        for (auto t : tiles) sx += t.x, sy += t.y;
        const long n = std::max<long>(1, static_cast<long>(tiles.size()));
        return vec2{static_cast<int>(sx / n), static_cast<int>(sy / n)};
    };
    home_center = centre(ally_spawn.positions());
    enemy_center = centre(enemy_spawn.positions());

    cambc::bitboard visited;
    const auto terrain = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    home_field.compute(terrain, ally_spawn, visited);
    enemy_field.compute(terrain, enemy_spawn, visited);

    altars = cambc::map::altars().positions();

    group_at.assign(static_cast<std::size_t>(width * height), -1);
    for (const auto& g : cambc::map::camp_groups()) {
        Group G;
        G.handle = static_cast<unsigned>(static_cast<cambc::handle_camp_group_t>(g));
        G.gems = static_cast<int>(g.gem_value());
        G.gold = static_cast<int>(g.gold_value());
        G.respawn = g.max_respawn_timer();
        for (const auto& c : g.camps()) {
            const vec2 p = c.spawn_position();
            G.spawns.push_back(p);
            float chp = 0, cstr = 0;
            int crange = 3;
            switch (c.kind()) {
                case cambc::camp_type::wolf: chp = 1350; cstr = 20; break;
                case cambc::camp_type::elk: chp = 2400; cstr = 60; crange = 4; break;
                case cambc::camp_type::gem_cluster: chp = 120; break;
                case cambc::camp_type::gold_cluster: chp = 120; break;
                case cambc::camp_type::monolith: chp = 20000; cstr = 200; crange = 5; G.boss = true; break;
                case cambc::camp_type::golem: chp = 5000; cstr = 120; crange = 4; G.boss = true; break;
            }
            G.max_hp += chp;
            G.strength += cstr;
            G.range = std::max(G.range, crange);
            group_at[static_cast<std::size_t>(p.y * width + p.x)] = static_cast<std::int16_t>(groups.size());
        }
        G.center = centre(G.spawns);
        groups.push_back(std::move(G));
    }

    const auto mates = cambc::champions(team::ally);
    team_count = static_cast<int>(mates.size());
    for (int i = 0; i < team_count; ++i) {
        const auto& m = mates[static_cast<std::size_t>(i)];
        if (m == me) my_index = i;
        Body b;
        b.id = static_cast<unsigned>(static_cast<cambc::handle_champion_t>(m));
        b.type = static_cast<int>(m.champion_name());
        const auto stats = cambc::champion_designs[static_cast<std::size_t>(std::clamp(b.type, 0, 12))].base_stats();
        b.hp = b.max_hp = stats.base_health;
        b.strength = stats.base_strength;
        b.focus = stats.base_focus;
        b.range = stats.base_attack_range;
        b.melee = b.range <= 1;
        const auto t = static_cast<std::size_t>(std::clamp(b.type, 0, 12));
        b.dps = b.strength * (1.0f + kResets[t] / 8.0f) + b.focus * kSpellMult[t] / 8.0f;
        ally_design.push_back(b);
        ally_ids.push_back(b.id);
    }

    const auto slots = me.abilities();
    for (int s = 0; s < 2; ++s) {
        const auto& info = slots[static_cast<std::size_t>(s)].info();
        if (!info) continue;
        Spell sp;
        std::visit([&](const auto& a) {
            using T = std::decay_t<decltype(a)>;
            sp.kind = static_cast<int>(T::kind);
            sp.exclusive = a.exclusive_with_movement;
            if constexpr (requires { a.range; }) sp.range = a.range;
            if constexpr (requires { a.modifier; }) sp.modifier = a.modifier;
            if constexpr (requires { a.radius; }) sp.radius = a.radius;
            if constexpr (requires { a.enhanced_attack; }) sp.enhanced = a.enhanced_attack;
        }, *info);
        spells[static_cast<std::size_t>(s)] = sp;
    }
    type = static_cast<int>(me.champion_name());
    ready = true;
}

void Bot::read_body(const cambc::champion& c, Body& b, bool enemy) {
    b.id = static_cast<unsigned>(static_cast<cambc::handle_champion_t>(c));
    b.hp = c.health();
    b.max_hp = std::max(1.0f, c.max_health());
    b.strength = c.strength();
    b.focus = c.focus();
    b.range = c.attack_range();
    b.type = static_cast<int>(c.champion_name());
    b.melee = b.range <= 1;
    const auto st = c.state();
    b.channelling = st.contains(cambc::champion_state::recalling) || st.contains(cambc::champion_state::submitting_gems)
        || st.contains(cambc::champion_state::teleporting);
    b.stunned = c.buffs().has_stun();
    const auto t = static_cast<std::size_t>(std::clamp(b.type, 0, 12));
    b.dps = b.strength * (1.0f + kResets[t] / 8.0f) + b.focus * kSpellMult[t] / 8.0f;
    b.burst = b.strength * (1.0f + kResets[t]) + b.focus * kSpellMult[t] * 0.6f;
    if (b.stunned || b.channelling) b.dps *= 0.5f;
    b.on_spawn = (enemy ? enemy_spawn : ally_spawn).is_set(b.pos);
}

void Bot::snapshot() {
    auto& me = cambc::self();
    pos = me.position();
    hp = me.health();
    max_hp = std::max(1.0f, me.max_health());
    strength = me.strength();
    focus = me.focus();
    range = me.attack_range();
    melee = range <= 1;
    moved = attacked = cast = cast_exclusive = move_blocked = false;
    leashed = false;

    visible = cambc::vision::visible_tiles();
    const auto enemy_tiles = cambc::vision::champion_tiles(team::enemy);
    const auto ally_tiles = cambc::vision::champion_tiles(team::ally);
    const auto camp_tiles = cambc::vision::camp_tiles();
    occupied = enemy_tiles | ally_tiles | camp_tiles;

    enemies.clear();
    for (const auto p : enemy_tiles.positions()) {
        try {
            Body b;
            b.pos = p;
            read_body(cambc::vision::champion_at(p), b, true);
            if (b.hp > 0) enemies.push_back(b);
        } catch (const std::exception& e) { warn("enemy", e); }
    }
    allies.clear();
    for (const auto p : ally_tiles.positions()) {
        if (p == pos) continue;
        try {
            Body b;
            b.pos = p;
            read_body(cambc::vision::champion_at(p), b, false);
            if (b.hp > 0) allies.push_back(b);
        } catch (const std::exception& e) { warn("ally", e); }
    }
    camps.clear();
    for (const auto p : camp_tiles.positions()) {
        try {
            const auto c = cambc::vision::camp_at(p);
            CampSeen s;
            s.pos = p;
            s.hp = c.health();
            if (s.hp <= 0) continue;
            s.max_hp = std::max(1.0f, c.max_health());
            s.strength = c.strength();
            s.awake = c.is_awake();
            const vec2 sp = c.spawn_position();
            if (in_map(sp, width, height)) s.group = group_at[static_cast<std::size_t>(sp.y * width + sp.x)];
            switch (c.kind()) {
                case cambc::camp_type::elk: case cambc::camp_type::golem: s.range = 4; break;
                case cambc::camp_type::monolith: s.range = 5; break;
                case cambc::camp_type::gem_cluster: case cambc::camp_type::gold_cluster: s.range = 0; break;
                default: s.range = 3; break;
            }
            camps.push_back(s);
        } catch (const std::exception& e) { warn("camp", e); }
    }

    alive_allies = 0;
    for (const auto& c : cambc::champions(team::ally))
        if (c.is_alive()) ++alive_allies;
    alive_enemies = 0;
    for (const auto& c : cambc::champions(team::enemy))
        if (c.is_alive()) ++alive_enemies;
    if (!enemies.empty()) last_enemy_round = static_cast<int>(round);
    for (const auto& e : enemies) remember(enemy_mem, e);
    for (const auto& a : allies) remember(ally_mem, a);
}

// Refresh camp-group memory for groups we can see.
void Bot::update_groups() {
    for (int gi = 0; gi < static_cast<int>(groups.size()); ++gi) {
        auto& g = groups[static_cast<std::size_t>(gi)];
        if (cheb(g.center, pos) > 11) continue;
        bool seen = false;
        for (auto s : g.spawns)
            if (cheb(s, pos) <= 9 && visible.is_set(s)) { seen = true; break; }
        if (!seen) continue;
        std::uint32_t state = 0;
        if (cambc::detail::sdk_camp_group_state(g.handle, &state) != 0 || state == 0) continue;
        g.state = static_cast<int>(state);
        g.seen = static_cast<int>(round);
        if (state == 3) {
            std::int32_t timer = 0;
            if (cambc::detail::sdk_camp_group_cooldown(g.handle, &timer) == 0)
                g.dead_until = static_cast<int>(round) + timer;
            g.hp_left = -1;
        } else if (state == 1) {
            g.dead_until = 0;
            float left = 0;
            bool all_seen = true;
            for (auto s : g.spawns)
                if (!visible.is_set(s)) { all_seen = false; break; }
            for (const auto& c : camps)
                if (c.group == gi) left += c.hp;
            if (all_seen) g.hp_left = left;
        } else {
            g.dead_until = 0;
            g.hp_left = 0;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Shared memory: [0,1] leader x,y  [2] leader round  [3] unavailable mask
//                [4] mode (low 2 bits) | sighting round (high 6 bits)  [5] mode argument
//                [6,7] last enemy sighting x,y
// ---------------------------------------------------------------------------------------------

void Bot::read_shared() {
    const int lr = mem[2] & 3;
    leader_fresh = mem[0] != 255 && ((static_cast<int>(round) - lr) & 3) <= 2 && round > 1;
    pack_mask = mem[2] >> 2;
    leader_pos = vec2{mem[0], mem[1]};
    mode = mem[4] & 3;
    mode_arg = mem[5];
    const int sr = mem[4] >> 2;
    sighting_age = (static_cast<int>(round & 63) - sr) & 63;
    sighting = vec2{mem[6], mem[7]};
    if (round < 2) sighting_age = 1000;
    call_age = sighting_age;
    call_fresh = call_age <= 1;
    call_engage = (mem[3] & 0x80) != 0;
    call_target = sighting;
}

void Bot::write_shared() {
    cambc::shared_memory.write(0, std::span<const std::uint8_t>{mem});
}

void Bot::elect_leader() {
    const auto mates = cambc::champions(team::ally);
    leader = -1;
    int fallback = -1;
    for (int i = 0; i < static_cast<int>(mates.size()) && i < 8; ++i) {
        if (!mates[static_cast<std::size_t>(i)].is_alive()) continue;
        if (fallback < 0) fallback = i;
        if (mem[3] & (1u << i) & 0x3F) continue;
        leader = i;
        break;
    }
    if (leader < 0) leader = fallback;
    is_leader = leader == my_index;
}

int Bot::nearest_altar(vec2 from) const {
    int best = -1, bd = 1 << 30;
    for (int i = 0; i < static_cast<int>(altars.size()); ++i) {
        const int d = cheb(from, altars[static_cast<std::size_t>(i)]);
        if (d < bd) bd = d, best = i;
    }
    return best;
}

// Leader: choose what the pack does next.
void Bot::plan() {
    auto& me = cambc::self();
    const int r = static_cast<int>(round);
    const int gems = static_cast<int>(me.gem_count());

    // Bank when the pack carries enough, or near the end.
    if (gems >= kBankGems || (gems > 0 && r > 1900)) {
        if (mode != 2) {
            mode = 2;
            mode_arg = nearest_altar(pos);
        }
        return;
    }
    if (mode == 2 && gems > 0) return;

    // Re-score camp groups every few turns or when the target is gone.
    bool need = target < 0 || r - last_plan >= 6;
    if (target >= 0) {
        const auto& g = groups[static_cast<std::size_t>(target)];
        if (g.dead_until > r + 10) need = true;
    }
    if (!need) {
        mode = 1;
        mode_arg = target;
        return;
    }
    last_plan = r;
    if (r - leader_field_round >= 6) {
        cambc::bitboard roots, visited;
        roots.set(pos);
        leader_field.compute(cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | enemy_spawn, roots, visited);
        leader_field_round = r;
    }
    const float team_dps = std::max(1, alive_allies) * 70.0f;
    const float team_hp = std::max(1, alive_allies) * 1000.0f;
    float best = 0;
    int best_i = -1;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        const auto& g = groups[static_cast<std::size_t>(i)];
        if (g.gems + g.gold <= 0) continue;
        int d = 1 << 20;
        for (auto s : g.spawns) {
            for (auto dir : kDirs) {
                const vec2 n = s + dir;
                if (!in_map(n, width, height)) continue;
                const int v = leader_field.get(n);
                if (v != cambc::distance_field::unreachable && v < d) d = v;
            }
        }
        if (d >= (1 << 20)) continue;
        const float hp_left = g.hp_left >= 0 ? g.hp_left : g.max_hp;
        const float clear = hp_left / team_dps;
        // Damage the pack takes while clearing; bosses need a strong, healthy pack.
        const float taken = g.strength * clear;
        if (taken > team_hp * 0.6f) continue;
        const int wait = std::max(0, g.dead_until - r - d);
        const float value = g.gems * 1.0f + g.gold * 0.4f;
        float score = value / (static_cast<float>(d + wait) + clear + 8.0f);
        // Prefer groups on the enemy's half: that is where their farmers are.
        const int dh = home_field.get(g.center), de = enemy_field.get(g.center);
        if (dh != cambc::distance_field::unreachable && de != cambc::distance_field::unreachable && de < dh) score *= 1.25f;
        if (i == target) score *= 1.3f;
        if (score > best) best = score, best_i = i;
    }
    if (best_i != target) target_since = r;
    target = best_i;
    mode = target >= 0 ? 1 : 0;
    mode_arg = target >= 0 ? target : 255;
}

// ---------------------------------------------------------------------------------------------
// Movement
// ---------------------------------------------------------------------------------------------

bool Bot::free_tile(vec2 p) const {
    return in_map(p, width, height) && !static_block.is_set(p) && !occupied.is_set(p);
}

// Step once along a shortest path to any free goal tile.
bool Bot::step_to(const cambc::bitboard& goals_in, bool slow) {
    auto& me = cambc::self();
    if (moved || move_blocked || cast_exclusive) return false;
    if (!me.can_move()) return false;
    if (goals_in.is_set(pos)) return false;
    auto blocked = static_block | occupied;
    blocked.set(pos, false);
    const auto goals = goals_in & ~blocked;
    if (goals.is_empty()) return false;
    if (slow) return false;
    cambc::bitboard start, visited;
    start.set(pos);
    cambc::distance_field field;
    field.compute_to(blocked, goals, start, visited);
    if (!visited.is_set(pos)) return false;
    const int here = field.get(pos);
    vec2 best = pos;
    int bd = here;
    float bt = threat_at(pos);
    for (auto d : kDirs) {
        const vec2 n = pos + d;
        if (!in_map(n, width, height) || blocked.is_set(n) || !visited.is_set(n) || !leash_ok(n)) continue;
        const int v = field.get(n);
        if (v >= here) continue;
        const float t = threat_at(n);
        if (v < bd || (v == bd && t < bt)) bd = v, best = n, bt = t;
    }
    if (best == pos) return false;
    if (!me.move_absolute(best)) {
        move_blocked = true;
        return false;
    }
    moved = true;
    pos = best;
    return true;
}

bool Bot::step_toward(vec2 goal, int within) {
    cambc::bitboard goals;
    goals.draw_chebyshev(goal, static_cast<float>(within));
    return step_to(goals);
}

// Damage enemies could deal to a tile next turn.
float Bot::threat_at(vec2 p) const {
    float t = 0;
    for (const auto& e : enemies) {
        if (e.channelling || e.on_spawn) continue;
        const int reach = e.melee ? 2 : e.range;
        if (cheb(p, e.pos) <= reach) t += e.dps;
    }
    for (const auto& c : camps) {
        if (!c.awake || c.range == 0) continue;
        if (cheb(p, c.pos) <= c.range) t += c.strength;
    }
    return t;
}

// ---------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------

bool Bot::attack_body(const Body& b) {
    auto& me = cambc::self();
    if (attacked || cheb(pos, b.pos) > range) return false;
    if (!me.can_attack()) return false;
    try {
        if (!cambc::vision::has_champion(b.pos)) return false;
        const auto c = cambc::vision::champion_at(b.pos);
        if (!c.is_visible() || c.health() <= 0) return false;
        me.attack(c);
        attacked = true;
        return true;
    } catch (const std::exception& e) { warn("attack", e); }
    return false;
}

bool Bot::attack_camp(const CampSeen& s) {
    auto& me = cambc::self();
    if (attacked || cheb(pos, s.pos) > range) return false;
    if (!me.can_attack()) return false;
    try {
        if (!cambc::vision::has_camp(s.pos)) return false;
        const auto c = cambc::vision::camp_at(s.pos);
        if (!c.is_visible() || c.health() <= 0) return false;
        me.attack(c);
        attacked = true;
        return true;
    } catch (const std::exception& e) { warn("attack camp", e); }
    return false;
}

// After our own basic attack this turn, reset it if a target is still in reach.
bool Bot::try_reset_attack(bool camp_target) {
    auto& me = cambc::self();
    if (cast) return false;
    for (int s = 0; s < 2; ++s) {
        const auto& sp = spells[static_cast<std::size_t>(s)];
        if (sp.kind != static_cast<int>(cambc::ability_kind::attack_reset)) continue;
        const auto ab = me.abilities()[static_cast<std::size_t>(s)];
        if (!ab.is_ready()) continue;
        // Ranged champions that walked get their attack back; others need a hit first.
        if (!attacked && !(moved && !melee)) continue;
        try {
            if (!ab.use()) continue;
            cast = true;
            attacked = false;
            return true;
        } catch (const std::exception& e) { warn("reset", e); }
        (void)camp_target;
    }
    return false;
}

// Offensive spell on an enemy champion.
bool Bot::cast_on_enemy(const Body& t, bool allow_exclusive) {
    auto& me = cambc::self();
    if (cast) return false;
    const int d = cheb(pos, t.pos);
    // Choose the best ready slot: stun/damage first.
    for (int pass = 0; pass < 2; ++pass) {
        for (int s = 0; s < 2; ++s) {
            const auto& sp = spells[static_cast<std::size_t>(s)];
            const bool cc = sp.kind == static_cast<int>(cambc::ability_kind::cc);
            const bool dmg = sp.kind == static_cast<int>(cambc::ability_kind::damage);
            const bool aoe = sp.kind == static_cast<int>(cambc::ability_kind::aoe_damage);
            if (pass == 0 ? !cc : !(dmg || aoe)) continue;
            if (sp.exclusive && (moved || !allow_exclusive)) continue;
            if (d > static_cast<int>(sp.range)) continue;
            if (cc && t.stunned) continue;
            const auto ab = me.abilities()[static_cast<std::size_t>(s)];
            if (!ab.is_ready()) continue;
            try {
                bool ok = false;
                if (aoe) {
                    ok = ab.use(t.pos);
                } else {
                    if (!cambc::vision::has_champion(t.pos)) continue;
                    const auto c = cambc::vision::champion_at(t.pos);
                    ok = ab.use(c);
                }
                if (ok) {
                    cast = true;
                    if (sp.exclusive) cast_exclusive = true;
                    return true;
                }
            } catch (const std::exception& e) { warn("spell", e); }
        }
    }
    return false;
}

// Damage spells speed up camp clears when no enemy champion is around.
bool Bot::cast_on_camp(const CampSeen& c) {
    auto& me = cambc::self();
    if (cast) return false;
    const int d = cheb(pos, c.pos);
    for (int s = 0; s < 2; ++s) {
        const auto& sp = spells[static_cast<std::size_t>(s)];
        const bool dmg = sp.kind == static_cast<int>(cambc::ability_kind::damage)
            || sp.kind == static_cast<int>(cambc::ability_kind::cc)
            || sp.kind == static_cast<int>(cambc::ability_kind::aoe_damage);
        if (!dmg || sp.modifier <= 0) continue;
        if (sp.exclusive && moved) continue;
        if (d > static_cast<int>(sp.range)) continue;
        const auto ab = me.abilities()[static_cast<std::size_t>(s)];
        if (!ab.is_ready()) continue;
        // Exclusive casts stop a melee champion from walking into range; only cast them in reach.
        if (sp.exclusive && d > range) continue;
        try {
            bool ok = false;
            if (sp.kind == static_cast<int>(cambc::ability_kind::aoe_damage)) {
                ok = ab.use(c.pos);
            } else {
                if (!cambc::vision::has_camp(c.pos)) continue;
                ok = ab.use(cambc::vision::camp_at(c.pos));
            }
            if (ok) {
                cast = true;
                if (sp.exclusive) cast_exclusive = true;
                return true;
            }
        } catch (const std::exception& e) { warn("camp spell", e); }
    }
    return false;
}

// Heals and shields.
bool Bot::cast_support() {
    auto& me = cambc::self();
    if (cast) return false;
    for (int s = 0; s < 2; ++s) {
        const auto& sp = spells[static_cast<std::size_t>(s)];
        if (sp.exclusive && moved) continue;
        if (sp.kind == static_cast<int>(cambc::ability_kind::heal)) {
            const Body* best = nullptr;
            float need = 150;
            for (const auto& a : allies) {
                if (cheb(a.pos, pos) > static_cast<int>(sp.range)) continue;
                const float miss = a.max_hp - a.hp;
                if (miss > need) need = miss, best = &a;
            }
            if (!best) continue;
            const auto ab = me.abilities()[static_cast<std::size_t>(s)];
            if (!ab.is_ready()) continue;
            try {
                if (!cambc::vision::has_champion(best->pos)) continue;
                if (ab.use(cambc::vision::champion_at(best->pos))) { cast = true; return true; }
            } catch (const std::exception& e) { warn("heal", e); }
        } else if (sp.kind == static_cast<int>(cambc::ability_kind::shield)) {
            if (threat_at(pos) <= 0 || hp > max_hp * 0.9f) continue;
            const auto ab = me.abilities()[static_cast<std::size_t>(s)];
            if (!ab.is_ready()) continue;
            try {
                if (ab.use()) { cast = true; return true; }
            } catch (const std::exception& e) { warn("shield", e); }
        }
    }
    return false;
}

// Dash towards (or away from) a tile.
bool Bot::try_dash(vec2 toward, bool away) {
    auto& me = cambc::self();
    if (cast) return false;
    for (int s = 0; s < 2; ++s) {
        const auto& sp = spells[static_cast<std::size_t>(s)];
        if (sp.kind != static_cast<int>(cambc::ability_kind::dash)) continue;
        if (sp.exclusive && moved) continue;
        const auto ab = me.abilities()[static_cast<std::size_t>(s)];
        if (!ab.is_ready()) continue;
        const int r = std::max(1, static_cast<int>(sp.range));
        vec2 best = pos;
        float bscore = away ? static_cast<float>(cheb(pos, toward)) - threat_at(pos) * 0.01f
                            : -static_cast<float>(cheb(pos, toward));
        for (int dx = -r; dx <= r; ++dx) {
            for (int dy = -r; dy <= r; ++dy) {
                const vec2 n{pos.x + dx, pos.y + dy};
                if (n == pos || !free_tile(n) || (!away && !leash_ok(n))) continue;
                // A legal path needs a non-wall middle tile.
                if (cheb(n, pos) == 2) {
                    bool ok = false;
                    for (auto d : kDirs) {
                        const vec2 m = pos + d;
                        if (cheb(m, n) <= 1 && in_map(m, width, height) && !cambc::map::walls().is_set(m)
                            && !occupied.is_set(m)) { ok = true; break; }
                    }
                    if (!ok) continue;
                }
                float sc;
                if (away) {
                    sc = static_cast<float>(cheb(n, toward)) - threat_at(n) * 0.01f;
                } else {
                    const int dd = cheb(n, toward);
                    if (dd < 1) continue;
                    sc = -static_cast<float>(dd);
                }
                if (sc > bscore + 0.5f) bscore = sc, best = n;
            }
        }
        if (best == pos) return false;
        try {
            if (ab.use(best)) {
                cast = true;
                if (sp.exclusive) cast_exclusive = true;
                pos = best;
                return true;
            }
            move_blocked = true;
        } catch (const std::exception& e) { warn("dash", e); }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Combat
// ---------------------------------------------------------------------------------------------

void Bot::remember(std::vector<Memory>& list, const Body& b) {
    for (auto& m : list) {
        if (m.body.id == b.id) {
            m.body = b;
            m.round = static_cast<int>(round);
            return;
        }
    }
    list.push_back(Memory{b, static_cast<int>(round)});
}

// Fight or flee against enemy champions. Returns true when the turn was used.
bool Bot::combat() {
    const int r = static_cast<int>(round);
    // Enemies that matter: seen near us this round or very recently (bushes hide them).
    std::vector<const Body*> foes;
    for (const auto& e : enemies)
        if (cheb(e.pos, pos) <= 9) foes.push_back(&e);
    float their_hp = 0, their_dps = 0;
    for (const auto* e : foes) {
        their_hp += e->hp;
        their_dps += e->dps;
    }
    for (const auto& m : enemy_mem) {
        if (r - m.round < 1 || r - m.round > 2 || cheb(m.body.pos, pos) > 8) continue;
        bool shown = false;
        for (const auto* e : foes) shown |= e->id == m.body.id;
        if (shown) continue;
        their_hp += m.body.hp;
        their_dps += m.body.dps;
    }
    const bool fresh_call = call_fresh && call_age == 0;
    if (foes.empty()) {
        // Nothing in sight: join a fight a teammate called nearby.
        if (call_fresh && call_engage && call_target.x < width && call_target.y < height
            && cheb(call_target, pos) <= 12 && hp > max_hp * 0.4f) {
            step_toward(call_target, range);
            return true;
        }
        return false;
    }

    float our_hp = hp, our_dps = 0;
    {
        const auto t = static_cast<std::size_t>(std::clamp(type, 0, 12));
        our_dps = strength * (1.0f + kResets[t] / 8.0f) + focus * kSpellMult[t] / 8.0f;
    }
    unsigned counted = 0;
    for (const auto& m : ally_mem) {
        if (r - m.round > 2 || cheb(m.body.pos, pos) > 7) continue;
        our_hp += m.body.hp;
        our_dps += m.body.dps;
        for (int i = 0; i < static_cast<int>(ally_ids.size()); ++i)
            if (ally_ids[static_cast<std::size_t>(i)] == m.body.id) counted |= 1u << i;
    }
    // Pack members out of sight (bushes) still fight with us.
    int pack_near = 0;
    if (in_pack || (leader_fresh && cheb(pos, leader_pos) <= 5)) {
        for (int i = 0; i < static_cast<int>(ally_design.size()); ++i) {
            if (i == my_index || !(pack_mask & (1 << i))) continue;
            ++pack_near;
            if (counted & (1u << i)) continue;
            const Body* b = &ally_design[static_cast<std::size_t>(i)];
            for (const auto& m : ally_mem)
                if (m.body.id == b->id && r - m.round <= 30) b = &m.body;
            our_hp += b->hp;
            our_dps += b->dps;
        }
    }
    // Leash: pack members do not chase away from the pack; a leader without a pack holds.
    leashed = true;
    if (is_leader) {
        leash_center = pos;
        leash_radius = pack_near >= std::min(2, std::max(0, alive_allies - 1)) ? 2 : 0;
    } else if (in_pack && leader_fresh) {
        leash_center = leader_pos;
        leash_radius = 5;
    } else {
        leashed = false;
    }
    const float ours = our_hp * our_dps;
    const float theirs = std::max(1.0f, their_hp * their_dps);
    const float ratio = ours / theirs;

    // Best target: the called one when visible, else the one we can kill soonest.
    const Body* target = nullptr;
    float best = 1e9f;
    for (const auto* e : foes) {
        if (e->on_spawn) continue;
        if (r < no_chase_until && e->id == chase_id) continue;
        const int d = cheb(e->pos, pos);
        float score = e->hp;
        if (e->stunned || e->channelling) score *= 0.6f;
        score += 120.0f * std::max(0, d - range);
        if (fresh_call && call_engage && e->pos == call_target) score -= 400.0f;
        if (score < best) best = score, target = e;
    }

    const bool dying = hp < max_hp * 0.25f && threat_at(pos) >= hp * 0.5f;
    bool fight;
    if (fresh_call) {
        // Someone already called it this round: follow, unless our own view is clearly different.
        fight = call_engage;
        if (call_engage && ratio < 0.5f) fight = false;
        if (!call_engage && ratio > 2.0f) fight = true;
    } else {
        fight = ratio >= kEngageRatio;
    }
    if (fight != call_engage || !fresh_call) {
        // Publish the decision for the teammates acting after us this round.
        {
            mem[3] = static_cast<std::uint8_t>((mem[3] & 0x7F) | (fight ? 0x80 : 0));
            const vec2 tp = target ? target->pos : foes[0]->pos;
            mem[6] = static_cast<std::uint8_t>(tp.x);
            mem[7] = static_cast<std::uint8_t>(tp.y);
            mem[4] = static_cast<std::uint8_t>((mem[4] & 3) | ((round & 63) << 2));
        }
    }
    if (dying) fight = false;
    // A target we can finish right now is worth a swing even in a bad fight.
    if (!fight && target && cheb(target->pos, pos) <= range && target->hp <= strength * 1.5f && hp > threat_at(pos))
        fight = true;

    debug_block({
        if (round <= 300)
            std::printf("c r%u i%d t%d pos(%d,%d) hp%.0f/%.0f al%zu fo%zu ratio%.2f call%d/%d fight%d tgt(%d,%d hp%.0f) thr%.0f\n", round, my_index, type, pos.x, pos.y, hp, max_hp,
                allies.size(), foes.size(), ratio, (int)fresh_call, (int)call_engage, (int)fight, target ? target->pos.x : -1, target ? target->pos.y : -1, target ? target->hp : 0.f, threat_at(pos));
    });
    debug_block({
        if (round <= 40)
            for (const auto* e : foes)
                std::printf("  e id%u t%d pos(%d,%d) hp%.0f str%.0f foc%.0f rng%d dps%.0f st%d ch%d sp%d\n", e->id, e->type, e->pos.x, e->pos.y, e->hp, e->strength, e->focus, e->range, e->dps, (int)e->stunned, (int)e->channelling, (int)e->on_spawn);
    });
    if (fight && !target) fight = false;
    if (fight) {
        if (target->id != chase_id) {
            chase_id = target->id;
            chase_turns = 0;
            chase_best = cheb(target->pos, pos);
        }
        const int d = cheb(target->pos, pos);
        if (d > range) {
            if (d < chase_best) chase_best = d, chase_turns = 0;
            else if (++chase_turns > kChaseTurns) {
                no_chase_until = r + 15;
                return false;
            }
        } else {
            chase_turns = 0;
            chase_best = d;
        }
        engage(*target);
        return true;
    }
    retreat();
    return true;
}

void Bot::engage(const Body& t) {
    auto& me = cambc::self();
    const int d = cheb(pos, t.pos);
    // Walk-excluding spells first, while we have not walked. Melee only casts them when the
    // target is already in reach or out of walking reach anyway.
    const bool allow_excl = !melee ? true : (d <= 1 || d > 2);
    if (!melee && d > range) {
        // A ranged champion out of range prefers a spell from where it stands over walking.
        cast_on_enemy(t, true);
    } else {
        cast_on_enemy(t, allow_excl);
    }
    attack_body(t);
    if (!attacked && !cast_exclusive) {
        if (melee) {
            if (cheb(pos, t.pos) > 1) {
                if (cheb(pos, t.pos) >= 3) try_dash(t.pos, false);
                if (cheb(pos, t.pos) > 1) step_toward(t.pos, 1);
            }
            attack_body(t);
        } else if (cheb(pos, t.pos) > range) {
            if (!try_dash(t.pos, false)) step_toward(t.pos, range);
            // A walk spends a ranged champion's attack; a reset gives it back.
            if (cheb(pos, t.pos) <= range && moved) {
                if (try_reset_attack(false)) attack_body(t);
            }
        }
    }
    if (attacked && t.hp > 0) {
        if (try_reset_attack(false)) attack_body(t);
    }
    if (!cast) cast_on_enemy(t, false);
    if (!cast) cast_support();
    if (!moved && !cast_exclusive && melee && cheb(pos, t.pos) > 1) step_toward(t.pos, 1);
    (void)me;
}

void Bot::retreat() {
    auto& me = cambc::self();
    leashed = false;
    // Stun the closest threat if it does not cost our step.
    const Body* closest = nullptr;
    for (const auto& e : enemies)
        if (!closest || cheb(e.pos, pos) < cheb(closest->pos, pos)) closest = &e;
    if (closest) cast_on_enemy(*closest, false);
    // Free shot for ranged champions standing safe.
    if (!melee && closest && threat_at(pos) <= 0) {
        if (attack_body(*closest)) return;
    }
    if (closest && cheb(closest->pos, pos) <= 3) try_dash(closest->pos, true);
    // Step to the neighbour with the least threat, preferring the way home.
    if (!moved && !cast_exclusive && me.can_move()) {
        vec2 best = pos;
        float bs = threat_at(pos) * 10.0f + static_cast<float>(home_field.get(pos));
        for (auto d : kDirs) {
            const vec2 n = pos + d;
            if (!free_tile(n)) continue;
            const int hd = home_field.get(n);
            if (hd == cambc::distance_field::unreachable) continue;
            const float s = threat_at(n) * 10.0f + static_cast<float>(hd);
            if (s < bs) bs = s, best = n;
        }
        if (best != pos) {
            try {
                if (me.move_absolute(best)) { moved = true; pos = best; }
                else move_blocked = true;
            } catch (const std::exception& e) { warn("retreat", e); }
        }
    }
    if (!cast) cast_support();
    // Ranged: shoot if still possible (did not move).
    if (!melee && !moved && closest) attack_body(*closest);
    if (melee && closest) attack_body(*closest);
}

// ---------------------------------------------------------------------------------------------
// Economy
// ---------------------------------------------------------------------------------------------

void Bot::shop() {
    auto& me = cambc::self();
    const auto gold = me.gold_count();
    if (gold < 30) return;
    // Damage dealers mostly buy strength, some health; casters buy focus.
    auto pick = [&](int n) {
        const bool caster = type == static_cast<int>(cambc::champion_type::cassi);
        const bool healer = type == static_cast<int>(cambc::champion_type::willow);
        if (caster) return (n % 3 == 2) ? cambc::item_type::power_shard_of_health : cambc::item_type::power_shard_of_focus;
        if (healer) return (n % 2 == 1) ? cambc::item_type::power_shard_of_health : cambc::item_type::power_shard_of_focus;
        return (n % 3 == 2) ? cambc::item_type::power_shard_of_health : cambc::item_type::power_shard_of_strength;
    };
    for (int guard = 0; guard < 20; ++guard) {
        const auto item = pick(shards_bought);
        try {
            if (me.gold_count() < item.cost() || !me.can_buy(item, 1)) return;
            me.buy(item, 1);
            ++shards_bought;
        } catch (const std::exception& e) {
            warn("shop", e);
            return;
        }
    }
}

// Tribute when standing by an altar; returns true when the turn was used.
bool Bot::bank() {
    auto& me = cambc::self();
    if (me.gem_count() == 0) return false;
    for (auto a : altars) {
        if (cheb(a, pos) > cambc::constants::submit_gems_range) continue;
        try {
            me.submit_gems(a);
            return true;
        } catch (const std::exception& e) { warn("tribute", e); }
    }
    return false;
}

void Bot::follow(vec2 goal, int slack) {
    if (cheb(goal, pos) <= slack) return;
    step_toward(goal, slack);
}

// Farm the pack's target group, or close in on the leader.
void Bot::farm() {
    auto& me = cambc::self();
    const int tgt = (mode == 1 && mode_arg < static_cast<int>(groups.size())) ? mode_arg : -1;

    // Stray too far from the leader and we walk back first.
    if (!is_leader && leader_fresh && cheb(leader_pos, pos) > 6) {
        bool near_target = false;
        if (tgt >= 0)
            for (const auto& c : camps)
                if (c.group == tgt && cheb(c.pos, pos) <= 5) near_target = true;
        if (!near_target) {
            follow(leader_pos, 2);
            return;
        }
    }

    if (tgt >= 0) {
        auto& g = groups[static_cast<std::size_t>(tgt)];
        // Living camps of the group in sight: hit the weakest in range, else close in.
        const CampSeen* hit = nullptr;
        const CampSeen* near = nullptr;
        for (const auto& c : camps) {
            if (c.group != tgt) continue;
            const int d = cheb(c.pos, pos);
            if (d <= range && (!hit || c.hp < hit->hp)) hit = &c;
            if (!near || d < cheb(near->pos, pos)) near = &c;
        }
        if (hit || near) {
            const CampSeen& c = hit ? *hit : *near;
            if (cheb(c.pos, pos) <= 4) cast_on_camp(c);
            if (hit) attack_camp(*hit);
            if (!attacked) {
                cambc::bitboard goals;
                for (const auto& cc : camps)
                    if (cc.group == tgt) goals.draw_chebyshev(cc.pos, static_cast<float>(range));
                step_to(goals);
                for (const auto& cc : camps)
                    if (cc.group == tgt && cheb(cc.pos, pos) <= range) { attack_camp(cc); break; }
            } else if (melee) {
                // Melee can still walk; stay put next to the target.
            }
            if (attacked) try_reset_attack(true);
            if (attacked && hit) attack_camp(*hit);
            if (!cast) cast_support();
            return;
        }
        // No monsters left in sight: stand on a spawn tile to capture.
        bool group_visible = false;
        for (auto s : g.spawns)
            if (visible.is_set(s)) { group_visible = true; break; }
        if (group_visible && g.state == 2) {
            for (auto s : g.spawns)
                if (s == pos) return;  // capturing
            cambc::bitboard goals;
            for (auto s : g.spawns) goals.set(s);
            if (!step_to(goals)) {
                // All spawn tiles taken by allies: wait beside them.
            }
            return;
        }
        if (group_visible && g.state == 3) {
            // Gone; the leader will re-plan.
            if (!is_leader && leader_fresh) follow(leader_pos, 2);
            return;
        }
        // Walk to the group (leader) or the leader (followers).
        if (is_leader) {
            int near_allies = 0;
            for (const auto& m : ally_mem)
                if (static_cast<int>(round) - m.round <= 2 && cheb(m.body.pos, pos) <= 4) ++near_allies;
            const int expected = std::max(0, alive_allies - 1 - __builtin_popcount(mem[3] & 0x3Fu));
            bool wait = round % 3 == 0;
            near_allies = std::max(near_allies, __builtin_popcount(static_cast<unsigned>(pack_mask)) - 1);
            if (near_allies < std::min(expected, 3)) {
                if (wait_turns < 8) wait = true;
                ++wait_turns;
                if (wait_turns > 12) wait_turns = 0;
            } else {
                wait_turns = 0;
            }
            if (!wait) {
                cambc::bitboard goals;
                for (auto s : g.spawns) goals.draw_chebyshev(s, static_cast<float>(std::max(range, 1)));
                step_to(goals);
            }
        } else if (leader_fresh) {
            follow(leader_pos, 2);
        } else {
            cambc::bitboard goals;
            for (auto s : g.spawns) goals.draw_chebyshev(s, static_cast<float>(std::max(range, 1)));
            step_to(goals);
        }
        return;
    }
    if (!is_leader && leader_fresh) follow(leader_pos, 2);
    (void)me;
}

// ---------------------------------------------------------------------------------------------
// Turn
// ---------------------------------------------------------------------------------------------

void Bot::play_turn() {
    auto& me = cambc::self();
    round = cambc::round_number();
    if (!ready) init();

    cambc::shared_memory.read(0, std::span<std::uint8_t>{mem});
    if (round <= 1 && my_index == 0) {
        mem.fill(0);
        mem[0] = mem[6] = 255;
    }
    if (!me.is_alive()) {
        unavailable = true;
        mem[3] |= static_cast<std::uint8_t>(1u << my_index);
        mem[2] &= static_cast<std::uint8_t>(~(1u << (my_index + 2)));
        write_shared();
        shop();
        return;
    }
    const auto st = me.state();
    if (st.contains(cambc::champion_state::recalling) || st.contains(cambc::champion_state::submitting_gems)
        || st.contains(cambc::champion_state::teleporting)) {
        if (st.contains(cambc::champion_state::recalling)) {
            unavailable = true;
            mem[3] |= static_cast<std::uint8_t>(1u << my_index);
            mem[2] &= static_cast<std::uint8_t>(~(1u << (my_index + 2)));
            write_shared();
        }
        return;
    }

    snapshot();
    const bool at_spawn = ally_spawn.is_set(pos);
    if (at_spawn) shop();
    if (unavailable && hp >= max_hp * 0.9f) unavailable = false;
    // Spawn heals at turn end: stay until healed.
    const bool healing = at_spawn && hp < max_hp * 0.95f;
    if (unavailable) mem[3] |= static_cast<std::uint8_t>(1u << my_index);
    else mem[3] &= static_cast<std::uint8_t>(~(1u << my_index));

    read_shared();
    elect_leader();
    in_pack = !unavailable && (is_leader || (leader_fresh && cheb(pos, leader_pos) <= 3));
    if (in_pack) mem[2] |= static_cast<std::uint8_t>(1u << (my_index + 2));
    else mem[2] &= static_cast<std::uint8_t>(~(1u << (my_index + 2)));
    pack_mask = mem[2] >> 2;
    update_groups();


    if (is_leader) {
        plan();
        mem[4] = static_cast<std::uint8_t>((mem[4] & ~3) | (mode & 3));
        mem[5] = static_cast<std::uint8_t>(mode_arg);
    }

    bool done = healing;
    try {
        if (!done && (!enemies.empty() || (call_fresh && call_engage))) done = combat();
    } catch (const std::exception& e) { warn("combat", e); }
    if (!done) leashed = false;

    if (!done) {
        try {
            if (hp < max_hp * kRecallHp && !at_spawn) {
                me.recall();
                unavailable = true;
                mem[3] |= static_cast<std::uint8_t>(1u << my_index);
                done = true;
            }
        } catch (const std::exception& e) { warn("recall", e); }
    }
    if (!done) {
        if (bank()) done = true;
    }
    if (!done) {
        try {
            if (mode == 2 && me.gem_count() > 0) {
                cambc::bitboard goals;
                const int a = (mode_arg < static_cast<int>(altars.size())) ? mode_arg : nearest_altar(pos);
                if (a >= 0) goals.draw_chebyshev(altars[static_cast<std::size_t>(a)], 1.0f);
                if (!step_to(goals)) {
                    // Altar crowded: try any altar.
                    cambc::bitboard any;
                    for (auto al : altars) any.draw_chebyshev(al, 1.0f);
                    step_to(any);
                }
                if (bank()) done = true;
                done = true;
            } else if (mode == 2 && !is_leader && leader_fresh) {
                follow(leader_pos, 2);
                done = true;
            } else {
                farm();
                done = true;
            }
        } catch (const std::exception& e) { warn("farm", e); }
    }

    debug_block({
        if (round % 25 == 0)
            std::printf("t r%u i%d L%d mode%d arg%d pos(%d,%d) hp%.0f gems%u en%zu cp%zu tgt%d\n", round, my_index, leader, mode, mode_arg,
                pos.x, pos.y, hp, me.gem_count(), enemies.size(), camps.size(), target);
    });
    if (is_leader) {
        mem[0] = static_cast<std::uint8_t>(pos.x);
        mem[1] = static_cast<std::uint8_t>(pos.y);
        mem[2] = static_cast<std::uint8_t>((mem[2] & 0xFC) | (round & 3));
    }
    write_shared();
}
