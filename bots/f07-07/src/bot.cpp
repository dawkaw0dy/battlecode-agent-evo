// Early aggression on a farming economy. Each champion farms the camp group with the best value
// per turn (claims in shared memory keep allies apart), banks gems at altars and turns gold into
// shards. Whenever enemy champions show up the local fight is weighed with a square law; a winning
// fight is taken and announced so that healthy allies nearby converge on it, a losing one is left.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>
#include <string_view>
#include <variant>

using cambc::team;
using cambc::vec2;

namespace {

constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
                                    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

// Champion kits by cambc::champion_type: attack resets and spell damage as a multiple of focus per
// eight-turn cycle.
constexpr std::array<int, 13> kResets{1, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0};
constexpr std::array<float, 13> kSpellMult{1.0f, 0, 0, 2.0f, 0, 3.4f, 2.5f, 2.0f, 0, 0.75f, 0, 1.4f, 1.5f};

constexpr float kEngageValue = 0.0f;    // fight when the simulated outcome is worth more than this
constexpr float kRecallHp = 0.3f;
constexpr int kBankGems = 30;
constexpr int kChaseTurns = 6;
constexpr int kHelpRange = 14;

int cheb(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }

bool in_map(vec2 p, int w, int h) { return p.x >= 0 && p.y >= 0 && p.x < w && p.y < h; }

float kit_dps(int type, float strength, float focus) {
    const auto t = static_cast<std::size_t>(std::clamp(type, 0, 12));
    return strength * (1.0f + kResets[t] / 8.0f) + focus * kSpellMult[t] / 8.0f;
}

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
    terrain = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    static_block = terrain | enemy_spawn;

    auto centre = [](const std::vector<vec2>& tiles) {
        long sx = 0, sy = 0;
        for (auto t : tiles) sx += t.x, sy += t.y;
        const long n = std::max<long>(1, static_cast<long>(tiles.size()));
        return vec2{static_cast<int>(sx / n), static_cast<int>(sy / n)};
    };
    home_center = centre(ally_spawn.positions());
    enemy_center = centre(enemy_spawn.positions());

    cambc::bitboard visited;
    home_field.compute(terrain, ally_spawn, visited);
    enemy_field.compute(terrain, enemy_spawn, visited);

    altars = cambc::map::altars().positions();
    for (auto a : altars) altar_goals.draw_chebyshev(a, static_cast<float>(cambc::constants::submit_gems_range));
    altar_goals &= ~static_block;
    if (!altar_goals.is_empty()) altar_field.compute(static_block, altar_goals, visited);

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
        G.home_dist = G.enemy_dist = 1000;
        for (auto s : G.spawns) {
            for (auto d : kDirs) {
                const vec2 n = s + d;
                if (!in_map(n, width, height)) continue;
                const int h = home_field.get(n), e = enemy_field.get(n);
                if (h != cambc::distance_field::unreachable) G.home_dist = std::min(G.home_dist, h);
                if (e != cambc::distance_field::unreachable) G.enemy_dist = std::min(G.enemy_dist, e);
            }
        }
        groups.push_back(std::move(G));
    }

    const auto mates = cambc::champions(team::ally);
    team_count = static_cast<int>(mates.size());
    for (int i = 0; i < team_count; ++i) {
        const auto& m = mates[static_cast<std::size_t>(i)];
        if (m == me) my_index = i;
        ally_ids.push_back(static_cast<unsigned>(static_cast<cambc::handle_champion_t>(m)));
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
    b.dps = kit_dps(b.type, b.strength, b.focus);
    if (b.stunned || b.channelling) b.dps *= 0.5f;
    b.on_spawn = (enemy ? enemy_spawn : ally_spawn).is_set(b.pos);
    b.gems = c.gem_count();
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
    gems = me.gem_count();
    gold = me.gold_count();
    my_dps = std::max(10.0f, kit_dps(type, strength, focus));
    damaged = prev_hp >= 0 && hp < prev_hp - 1.0f;
    prev_hp = hp;
    moved = attacked = cast = cast_exclusive = move_blocked = false;

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
    const int r = static_cast<int>(round);
    for (int gi = 0; gi < static_cast<int>(groups.size()); ++gi) {
        auto& g = groups[static_cast<std::size_t>(gi)];
        if (cheb(g.center, pos) > 12) continue;
        bool seen = false;
        for (auto s : g.spawns)
            if (cheb(s, pos) <= 9 && visible.is_set(s)) { seen = true; break; }
        if (!seen) continue;
        std::uint32_t state = 0;
        if (cambc::detail::sdk_camp_group_state(g.handle, &state) != 0 || state == 0) continue;
        g.state = static_cast<int>(state);
        g.seen = r;
        for (const auto& e : enemies)
            if (cheb(e.pos, g.center) <= 6) g.danger_until = r + 30;
        if (state == 3) {
            std::int32_t timer = 0;
            if (cambc::detail::sdk_camp_group_cooldown(g.handle, &timer) == 0) g.dead_until = r + timer;
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

void Bot::read_shared() {
    cambc::shared_memory.read(0, std::span<std::uint8_t>{mem});
    if (round <= 1 && my_index == 0) {
        mem.fill(255);
        mem[7] = 0;
    }
    alert_age = 1000;
    if (mem[7] & 0x80) {
        alert_age = (static_cast<int>(round & 63) - (mem[7] & 63)) & 63;
        alert = vec2{mem[5], mem[6]};
        alert_help = (mem[7] & 0x40) != 0;
    }
}

void Bot::post_alert(vec2 p, bool help) {
    mem[5] = static_cast<std::uint8_t>(p.x);
    mem[6] = static_cast<std::uint8_t>(p.y);
    mem[7] = static_cast<std::uint8_t>(0x80 | (help ? 0x40 : 0) | (round & 63));
}

// ---------------------------------------------------------------------------------------------
// Movement
// ---------------------------------------------------------------------------------------------

bool Bot::free_tile(vec2 p) const {
    return in_map(p, width, height) && !static_block.is_set(p) && !occupied.is_set(p);
}

// Step once along a shortest path to any free goal tile.
bool Bot::step_to(const cambc::bitboard& goals_in) {
    auto& me = cambc::self();
    if (moved || move_blocked || cast_exclusive) return false;
    if (!me.can_move()) return false;
    if (goals_in.is_set(pos)) return false;
    auto blocked = static_block | occupied;
    blocked.set(pos, false);
    const auto goals = goals_in & ~blocked;
    if (goals.is_empty()) return false;
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
        if (!in_map(n, width, height) || blocked.is_set(n) || !visited.is_set(n)) continue;
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

// Damage enemies and awake camps could deal to a tile next turn.
float Bot::threat_at(vec2 p) const {
    float t = 0;
    for (const auto& e : enemies) {
        if (e.channelling || e.on_spawn) continue;
        const int reach = e.melee ? 2 : e.range;
        if (cheb(p, e.pos) <= reach) t += e.dps;
    }
    return t + camp_threat_at(p);
}

float Bot::camp_threat_at(vec2 p) const {
    float t = 0;
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
        if (!c.is_visible() || c.health() <= 0 || !c.is_enemy()) return false;
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

// Reset the basic attack: after our own hit, or for a ranged champion that walked.
bool Bot::try_reset_attack() {
    auto& me = cambc::self();
    if (cast) return false;
    if (!attacked && !(moved && !melee)) return false;
    for (int s = 0; s < 2; ++s) {
        const auto& sp = spells[static_cast<std::size_t>(s)];
        if (sp.kind != static_cast<int>(cambc::ability_kind::attack_reset)) continue;
        const auto ab = me.abilities()[static_cast<std::size_t>(s)];
        if (!ab.is_ready()) continue;
        try {
            if (!ab.use()) continue;
            cast = true;
            attacked = false;
            return true;
        } catch (const std::exception& e) { warn("reset", e); }
    }
    return false;
}

// Offensive spell on an enemy champion: stuns first, then damage.
bool Bot::cast_on_enemy(const Body& t, bool allow_exclusive) {
    auto& me = cambc::self();
    if (cast) return false;
    const int d = cheb(pos, t.pos);
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
                    if (!c.is_enemy()) continue;
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
        // Exclusive casts stop a melee champion from walking into range; only cast them in reach.
        if (sp.exclusive && d > range) continue;
        const auto ab = me.abilities()[static_cast<std::size_t>(s)];
        if (!ab.is_ready()) continue;
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
                if (n == pos || !free_tile(n)) continue;
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

// Fight or flee against enemy champions in sight. Returns true when the turn was used.
bool Bot::combat() {
    const int r = static_cast<int>(round);
    std::vector<const Body*> foes;
    for (const auto& e : enemies)
        if (cheb(e.pos, pos) <= 9) foes.push_back(&e);
    if (foes.empty()) return false;

    // Focus-fire simulation: each side pours its damage into the weakest enemy it can reach.
    struct SimUnit {
        float hp, dps, gems;
        int delay;
        bool me;
    };
    std::vector<SimUnit> ours, theirs;
    auto delay_to = [&](vec2 from, int rng, bool is_melee, bool vs_ours) {
        int d = 99;
        if (vs_ours) {
            for (const auto* e : foes) d = std::min(d, cheb(from, e->pos));
        } else {
            d = cheb(from, pos);
            for (const auto& m : ally_mem)
                if (r - m.round <= 2 && cheb(m.body.pos, pos) <= 8) d = std::min(d, cheb(from, m.body.pos));
        }
        return std::max(0, d - (is_melee ? 2 : rng));
    };
    ours.push_back({std::max(1.0f, hp - camp_threat_at(pos) * 2.0f), my_dps, static_cast<float>(gems),
                    delay_to(pos, range, melee, true), true});
    for (const auto& m : ally_mem) {
        if (r - m.round > 2 || cheb(m.body.pos, pos) > 8) continue;
        ours.push_back({m.body.hp, m.body.dps, static_cast<float>(m.body.gems),
                        delay_to(m.body.pos, m.body.range, m.body.melee, true), false});
    }
    for (const auto* e : foes) {
        // Enemies busy with camps take damage from them too.
        float hurt = 0;
        for (const auto& c : camps)
            if (c.awake && c.range > 0 && cheb(c.pos, e->pos) <= c.range) hurt += c.strength;
        theirs.push_back({std::max(1.0f, e->hp - hurt * 2.0f), e->channelling ? 0.0f : e->dps,
                          static_cast<float>(e->gems), delay_to(e->pos, e->range, e->melee, false), false});
    }
    for (const auto& m : enemy_mem) {
        if (r - m.round < 1 || r - m.round > 3 || cheb(m.body.pos, pos) > 10) continue;
        bool shown = false;
        for (const auto* e : foes) shown |= e->id == m.body.id;
        if (shown) continue;
        theirs.push_back({m.body.hp, m.body.dps, static_cast<float>(m.body.gems),
                          delay_to(m.body.pos, m.body.range, m.body.melee, false) + 1, false});
    }
    auto pour = [](std::vector<SimUnit>& side, float dmg, int t) {
        while (dmg > 0) {
            SimUnit* weakest = nullptr;
            for (auto& u : side)
                if (u.hp > 0 && u.delay <= t + 2 && (!weakest || u.hp < weakest->hp)) weakest = &u;
            if (!weakest) return;
            const float d = std::min(dmg, weakest->hp);
            weakest->hp -= d;
            dmg -= d;
        }
    };
    std::vector<float> our_start, their_start;
    for (const auto& u : ours) our_start.push_back(u.hp);
    for (const auto& u : theirs) their_start.push_back(u.hp);
    bool me_dies = false;
    for (int t = 0; t < 8; ++t) {
        float ours_dmg = 0, theirs_dmg = 0;
        for (const auto& u : ours)
            if (u.hp > 0 && u.delay <= t) ours_dmg += u.dps;
        for (const auto& u : theirs)
            if (u.hp > 0 && u.delay <= t) theirs_dmg += u.dps;
        pour(theirs, ours_dmg, t);
        pour(ours, theirs_dmg, t);
        if (ours[0].hp <= 0) me_dies = true;
    }
    float value = 0;
    int their_dead = 0, our_dead = 0;
    for (std::size_t i = 0; i < theirs.size(); ++i) {
        if (theirs[i].hp <= 0) { value += 300.0f + 2.0f * theirs[i].gems; ++their_dead; }
        value += 0.1f * (their_start[i] - std::max(0.0f, theirs[i].hp));
    }
    for (std::size_t i = 0; i < ours.size(); ++i) {
        if (ours[i].hp <= 0) { value -= 300.0f + 2.0f * ours[i].gems; ++our_dead; }
        value -= 0.1f * (our_start[i] - std::max(0.0f, ours[i].hp));
    }
    const float ratio = value;  // logged as the fight value

    // Best target: the one we can kill soonest, not sheltering on its spawn.
    const Body* target = nullptr;
    float best = 1e9f;
    for (const auto* e : foes) {
        if (e->on_spawn) continue;
        if (r < no_chase_until && e->id == chase_id) continue;
        const int d = cheb(e->pos, pos);
        float score = e->hp;
        if (e->stunned || e->channelling) score *= 0.6f;
        score += 120.0f * std::max(0, d - range);
        score -= 2.0f * static_cast<float>(e->gems);
        if (score < best) best = score, target = e;
    }

    // Fight when the simulation favours us and does not spend our life for nothing.
    bool fight = target != nullptr && value > kEngageValue && !(me_dies && their_dead == 0);
    if (me_dies && value < 300.0f) fight = false;
    const bool dying = hp < max_hp * 0.25f && threat_at(pos) >= hp * 0.5f;
    if (dying) fight = false;
    // A target we can finish right now is worth a swing even in a bad fight.
    if (!fight && target && cheb(target->pos, pos) <= range && target->hp <= strength * 1.5f && hp > threat_at(pos))
        fight = true;

    debug_block({
        if (round <= 400)
            std::printf("c r%u i%d t%d pos(%d,%d) hp%.0f/%.0f al%zu fo%zu ratio%.2f fight%d tgt(%d,%d hp%.0f) thr%.0f\n", round, my_index, type, pos.x, pos.y, hp, max_hp,
                allies.size(), foes.size(), ratio, (int)fight, target ? target->pos.x : -1, target ? target->pos.y : -1, target ? target->hp : 0.f, threat_at(pos));
    });

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
        post_alert(target->pos, true);
        debug_block({
            last_act = "engage";
            std::snprintf(last_info, sizeof last_info, "hp%.0f ratio%.2f foes%zu allies%zu tgt_hp%.0f d%d dmg%d", hp, ratio, foes.size(), allies.size(), target->hp, cheb(target->pos, pos), (int)damaged);
        });
        engage(*target);
        return true;
    }
    // Enemies far away and not coming for us: carry on farming, but warn the team.
    bool close = false;
    for (const auto* e : foes)
        if (!e->on_spawn && cheb(e->pos, pos) <= (e->melee ? 4 : e->range + 2)) close = true;
    if (!close) {
        if (!(alert_age <= 2 && alert_help)) post_alert(foes[0]->pos, false);
        return false;
    }
    if (!(alert_age <= 2 && alert_help)) post_alert(foes[0]->pos, false);
    debug_block({
        last_act = "retreat";
        std::snprintf(last_info, sizeof last_info, "hp%.0f ratio%.2f foes%zu allies%zu near_d%d dmg%d gems%u", hp, ratio, foes.size(), allies.size(), cheb(foes[0]->pos, pos), (int)damaged, gems);
    });
    retreat();
    return true;
}

void Bot::engage(const Body& t) {
    const int d = cheb(pos, t.pos);
    // Walk-excluding spells first, while we have not walked. Melee only casts them when the
    // target is already in reach or out of walking reach anyway.
    const bool allow_excl = !melee ? true : (d <= 1 || d > 2);
    cast_on_enemy(t, allow_excl);
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
                if (try_reset_attack()) attack_body(t);
            }
        }
    }
    if (attacked && t.hp > strength) {
        if (try_reset_attack()) attack_body(t);
    }
    if (!cast) cast_on_enemy(t, false);
    if (!cast) cast_support();
    if (!moved && !cast_exclusive && melee && cheb(pos, t.pos) > 1) step_toward(t.pos, 1);
}

void Bot::retreat() {
    // Stun the closest threat if it does not cost our step.
    const Body* closest = nullptr;
    for (const auto& e : enemies)
        if (!closest || cheb(e.pos, pos) < cheb(closest->pos, pos)) closest = &e;
    if (closest) cast_on_enemy(*closest, false);
    // Free shot for ranged champions standing where nobody can hit them next turn.
    if (!melee && closest && threat_at(pos) <= 0) {
        if (attack_body(*closest)) return;
    }
    if (closest && cheb(closest->pos, pos) <= 3) try_dash(closest->pos, true);
    // Step to the neighbour with the least threat, preferring the way home.
    step_home();
    if (!cast) cast_support();
    if (closest && (melee || !moved)) attack_body(*closest);
}

// One step down the home field, avoiding known threats.
bool Bot::step_home() {
    auto& me = cambc::self();
    if (moved || move_blocked || cast_exclusive || !me.can_move()) return false;
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
    if (best == pos) return false;
    try {
        if (cambc::vision::has_champion(best) || cambc::vision::has_camp(best)) {
            move_blocked = true;
            return false;
        }
        if (me.move_absolute(best)) {
            moved = true;
            pos = best;
            return true;
        }
        move_blocked = true;
    } catch (const std::exception& e) { warn("step home", e); }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Economy
// ---------------------------------------------------------------------------------------------

void Bot::shop() {
    auto& me = cambc::self();
    if (me.gold_count() < 30) return;
    // Damage dealers mostly buy strength, some health; casters buy focus.
    auto pick = [&](int n) {
        const bool caster = type == static_cast<int>(cambc::champion_type::cassi);
        const bool healer = type == static_cast<int>(cambc::champion_type::willow);
        // The first emblem cuts camp damage by 30% (jungle); then raw stats (power).
        if (n < 5) return (caster || healer) ? cambc::item_type::jungle_shard_of_focus : cambc::item_type::jungle_shard_of_strength;
        if (caster) return (n % 3 == 2) ? cambc::item_type::power_shard_of_health : cambc::item_type::power_shard_of_focus;
        if (healer) return (n % 2 == 1) ? cambc::item_type::power_shard_of_health : cambc::item_type::power_shard_of_focus;
        return (n % 3 == 2) ? cambc::item_type::power_shard_of_health : cambc::item_type::power_shard_of_strength;
    };
    for (int guard = 0; guard < 30; ++guard) {
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
    if (gems == 0) return false;
    for (auto a : altars) {
        if (cheb(a, pos) > cambc::constants::submit_gems_range) continue;
        try {
            if (cambc::vision::has_altar_lock(a) && cambc::vision::altar_lock_at(a).is_enemy()) {
                // Break the lock with basic attacks.
                if (!attacked && me.can_attack()) {
                    me.attack(cambc::vision::altar_lock_at(a));
                    attacked = true;
                    return true;
                }
                continue;
            }
            me.submit_gems(a);
            return true;
        } catch (const std::exception& e) { warn("tribute", e); }
    }
    return false;
}

// Walk to the nearest altar with our gems.
bool Bot::go_bank() {
    if (altar_goals.is_empty()) return false;
    if (bank()) return true;
    step_to(altar_goals);
    if (bank()) return true;
    return true;
}

// Pick the camp group with the best value per turn of our time.
void Bot::choose_target() {
    const int r = static_cast<int>(round);
    last_plan = r;
    if (r - my_field_round >= 4 || target < 0) {
        cambc::bitboard roots, visited;
        roots.set(pos);
        my_field.compute(static_block, roots, visited);
        my_field_round = r;
    }
    // Gold buys shards early; gems win the game.
    const float gold_w = r < 600 ? 0.6f : (r < 1200 ? 0.4f : 0.25f);
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
                const int v = my_field.get(n);
                if (v != cambc::distance_field::unreachable && v < d) d = v;
            }
        }
        if (d >= (1 << 20)) continue;
        const float left = g.hp_left >= 0 ? g.hp_left : g.max_hp;
        const float clear = left / my_dps;
        // Trips: each one lasts until camp damage takes us to the recall threshold.
        float overhead = 0;
        if (g.strength > 0) {
            const float first = std::max(1.0f, (hp - max_hp * kRecallHp) / g.strength);
            const float full = std::max(1.0f, max_hp * (1.0f - kRecallHp) / g.strength);
            if (first * my_dps < left) {
                const float extra = std::ceil((left - first * my_dps) / (full * my_dps));
                if (extra > 6) continue;
                overhead = extra * (6.0f + 2.0f * static_cast<float>(g.home_dist));
            }
        }
        const int wait = std::max(0, g.dead_until - r - d);
        const float value = static_cast<float>(g.gems) + static_cast<float>(g.gold) * gold_w;
        float score = value / (static_cast<float>(d + wait) + clear + overhead + 6.0f);
        // Allies already on it: leave it to them.
        for (int a = 0; a < 5 && a < team_count; ++a)
            if (a != my_index && mem[static_cast<std::size_t>(a)] == i) score *= 0.3f;
        if (g.danger_until > r) score *= 0.6f;
        if (i == target) score *= 1.25f;
        if (score > best) best = score, best_i = i;
    }
    target = best_i;
}

// Farm the target group. Returns true when the turn was used.
bool Bot::farm() {
    const int r = static_cast<int>(round);
    bool replan = target < 0 || r - last_plan >= 8;
    if (target >= 0) {
        const auto& g = groups[static_cast<std::size_t>(target)];
        // Never switch while its monsters are awake and in sight: finish or leave first.
        for (const auto& c : camps)
            if (c.group == target && c.awake && cheb(c.pos, pos) <= 6) replan = false;
        if (g.dead_until > r + 5) replan = true;
    }
    if (replan) choose_target();
    if (my_index < 5) mem[static_cast<std::size_t>(my_index)] = static_cast<std::uint8_t>(target >= 0 ? target : 255);
    if (target < 0) return false;
    auto& g = groups[static_cast<std::size_t>(target)];

    // Living camps of the group in sight: hit the weakest in range, else close in.
    const CampSeen* hit = nullptr;
    const CampSeen* near = nullptr;
    for (const auto& c : camps) {
        if (c.group != target) continue;
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
                if (cc.group == target) goals.draw_chebyshev(cc.pos, static_cast<float>(range));
            step_to(goals);
            for (const auto& cc : camps)
                if (cc.group == target && cheb(cc.pos, pos) <= range) { attack_camp(cc); break; }
        }
        if (attacked) try_reset_attack();
        if (!attacked) {
            for (const auto& cc : camps)
                if (cc.group == target && cheb(cc.pos, pos) <= range) { attack_camp(cc); break; }
        }
        if (!cast) cast_support();
        return true;
    }
    // No monsters left in sight: stand on a spawn tile to capture.
    bool group_visible = false;
    for (auto s : g.spawns)
        if (visible.is_set(s)) { group_visible = true; break; }
    if (group_visible && g.state == 2) {
        for (auto s : g.spawns)
            if (s == pos) return true;  // capturing
        cambc::bitboard goals;
        for (auto s : g.spawns) goals.set(s);
        step_to(goals);
        return true;
    }
    if (group_visible && g.state == 3) {
        choose_target();
        return false;
    }
    cambc::bitboard goals;
    for (auto s : g.spawns) goals.draw_chebyshev(s, static_cast<float>(std::max(range, 1)));
    step_to(goals);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Turn
// ---------------------------------------------------------------------------------------------

void Bot::play_turn() {
    auto& me = cambc::self();
    round = cambc::round_number();
    if (!ready) init();
    read_shared();

    if (!me.is_alive()) {
        debug_block({
            if (was_alive) std::printf("DEATH r%u i%d t%d last=%s %s\n", round, my_index, type, last_act, last_info);
        });
        was_alive = false;
        if (my_index < 5) mem[static_cast<std::size_t>(my_index)] = 255;
        cambc::shared_memory.write(0, std::span<const std::uint8_t>{mem});
        shop();
        return;
    }
    const auto st = me.state();
    if (st.contains(cambc::champion_state::recalling) || st.contains(cambc::champion_state::submitting_gems)
        || st.contains(cambc::champion_state::teleporting))
        return;

    snapshot();
    was_alive = true;
    update_groups();
    const bool at_spawn = ally_spawn.is_set(pos);
    if (at_spawn) shop();
    const int r = static_cast<int>(round);

    bool done = false;
    // Spawn heals at turn end: stay until healed, hitting anything in reach.
    if (at_spawn && hp < max_hp * 0.95f) {
        for (const auto& e : enemies)
            if (attack_body(e)) break;
        done = true;
    }
    try {
        if (!done && !enemies.empty()) done = combat();
    } catch (const std::exception& e) { warn("combat", e); }

    // Hit by something we cannot see (an enemy in a bush): get out of there.
    if (damaged && enemies.empty() && camp_threat_at(pos) <= 0 && !at_spawn) ambush_until = r + 2;
    if (!done && r <= ambush_until && !at_spawn) {
        step_home();
        done = true;
        debug_block({ last_act = "ambush"; });
    }
    // Leave early enough to walk out of camp reach alive.
    const float camp_dps = camp_threat_at(pos);
    const bool low = hp < max_hp * kRecallHp || (camp_dps > 0 && hp < camp_dps * 4.0f + max_hp * 0.1f);
    if (!done && low && !at_spawn) {
        debug_block({ last_act = "low"; });
        // Out of camp reach first; then recall (damage would cancel it).
        if (camp_threat_at(pos) > 0) {
            // Walk away from the awake camps, homewards when it is all the same.
            auto score = [&](vec2 p) {
                int near = 99;
                for (const auto& c : camps)
                    if (c.awake && c.range > 0) near = std::min(near, cheb(c.pos, p));
                const int hd = home_field.get(p);
                return camp_threat_at(p) * 100.0f - static_cast<float>(near) * 30.0f
                    + static_cast<float>(hd == cambc::distance_field::unreachable ? 500 : hd);
            };
            vec2 best = pos;
            float bt = score(pos);
            for (auto d : kDirs) {
                const vec2 n = pos + d;
                if (!free_tile(n)) continue;
                const float t = score(n);
                if (t < bt) bt = t, best = n;
            }
            // Melee can still hit on the way out.
            if (melee)
                for (const auto& c : camps)
                    if (c.group == target && attack_camp(c)) break;
            if (best != pos) {
                try {
                    if (me.move_absolute(best)) { moved = true; pos = best; }
                } catch (const std::exception& e) { warn("leave camp", e); }
            }
        } else if (damaged || static_cast<int>(round) - last_enemy_round <= 4) {
            // Enemies around would cancel the recall (or kill us while channelling): walk.
            step_home();
        } else {
            try {
                me.recall();
            } catch (const std::exception& e) { warn("recall", e); }
        }
        done = true;
    }
    if (!done && bank()) done = true;
    if (!done) {
        try {
            // Answer a teammate's winning fight nearby.
            if (alert_age <= 3 && alert_help && cheb(alert, pos) <= kHelpRange && hp > max_hp * 0.6f && gems < 30
                && cheb(alert, pos) > 2) {
                step_toward(alert, range);
                done = true;
            }
        } catch (const std::exception& e) { warn("help", e); }
    }
    if (!done) {
        try {
            const int ad = altar_field.get(pos);
            const bool late = r > 1850;
            if (gems > 0 && (gems >= kBankGems || late || (ad != cambc::distance_field::unreachable && ad <= 5)))
                banking = true;
            if (gems == 0) banking = false;
            if (banking) done = go_bank();
            debug_block({ if (banking) last_act = "bank"; });
        } catch (const std::exception& e) { warn("bank", e); }
    }
    if (!done) {
        try {
            done = farm();
            debug_block({ last_act = "farm"; });
        } catch (const std::exception& e) { warn("farm", e); }
    }
    debug_block({
        if (!done) last_act = "idle";
        if (std::string_view(last_act) != "engage" && std::string_view(last_act) != "retreat") {
            std::snprintf(last_info, sizeof last_info, "hp%.0f pos(%d,%d) tgt%d cthr%.0f dmg%d en%zu gems%u bank%d", hp, pos.x, pos.y, target, camp_threat_at(pos), (int)damaged, enemies.size(), gems, (int)banking);
        }
        if (my_index == 0 && round <= 300)
            std::printf("v r%u pos(%d,%d) hp%.0f/%.0f tgt%d cthr%.0f thr%.0f mv%d at%d gems%u gold%u\n", round, pos.x, pos.y, hp, max_hp, target,
                camp_threat_at(pos), threat_at(pos), (int)moved, (int)attacked, gems, gold);
        if (round % 50 == 0)
            std::printf("t r%u i%d t%d pos(%d,%d) hp%.0f gems%u gold%u tgt%d bank%d\n", round, my_index, type, pos.x, pos.y, hp,
                gems, gold, target, (int)banking);
    });
    cambc::shared_memory.write(0, std::span<const std::uint8_t>{mem});
}
