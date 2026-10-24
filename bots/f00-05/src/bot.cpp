// Champion decisions. main.cpp dispatches here after the draft.
// Early aggression: the team moves as one pack between camp groups, fights enemy champions whenever the
// local fight is favourable (focus the weakest, stun, reset, dash to chase) and retreats from losing fights.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;

namespace {

// ---- Tunables ---------------------------------------------------------------------------------------------
constexpr float kEngageRatio = 1.0f;    // fight when our local power / their power is at least this
constexpr float kHoldRatio = 0.65f;     // below this we retreat
constexpr float kFleeHp = 0.25f;        // flee below this health fraction unless the kill is on
constexpr float kRecallHp = 0.35f;      // recall below this health fraction when no enemy is close
constexpr int kBankGems = 25;           // carry at most this many gems before banking
constexpr int kEnemyMemory = 10;        // rounds after a sighting in which spells are saved for champions

int distance(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }

// ---- Static map data, computed once per champion ----------------------------------------------------------
struct GroupInfo {
    cambc::bitboard tiles;
    std::vector<vec2> spots;
    vec2 center;
    float value = 0;     // gems + weighted gold
    float hp = 0;        // total camp health
    bool monolith = false, golem = false;
};

int W = 0, H = 0;
cambc::bitboard static_walls;
std::vector<cambc::map::camp_group> groups_raw;
std::vector<GroupInfo> groups;
std::vector<std::int16_t> group_of; // tile -> group index or -1
cambc::distance_field home_field, altar_field;
cambc::bitboard home_visited, altar_visited;
vec2 ally_center, enemy_center;
std::vector<vec2> altar_tiles;

bool in_bounds(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < W && p.y < H; }

vec2 centroid(const cambc::bitboard& b) {
    long sx = 0, sy = 0, n = 0;
    for (auto p : b.positions()) { sx += p.x; sy += p.y; ++n; }
    if (!n) return {W / 2, H / 2};
    return {static_cast<int>(sx / n), static_cast<int>(sy / n)};
}

bool is_melee(const Seen& s) { return s.range <= 1; }

float dps_of(const Seen& s) { return s.str + 0.3f * s.focus; }

std::uint8_t mem_get(int i) { return static_cast<std::uint8_t>(cambc::shared_memory[i]); }
void mem_set(int i, std::uint8_t v) { cambc::shared_memory[i] = v; }

} // namespace

void Bot::init() {
    initialised = true;
    auto& me = cambc::self();
    W = cambc::map::width();
    H = cambc::map::height();
    static_walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    ally_center = centroid(cambc::map::spawn_region(team::ally));
    enemy_center = centroid(cambc::map::spawn_region(team::enemy));

    group_of.assign(static_cast<std::size_t>(W * H), -1);
    groups_raw = cambc::map::camp_groups();
    groups.clear();
    for (std::size_t g = 0; g < groups_raw.size(); ++g) {
        const auto& raw = groups_raw[g];
        GroupInfo info;
        info.tiles = raw.spawn_tiles();
        info.spots = info.tiles.positions();
        info.center = centroid(info.tiles);
        info.value = static_cast<float>(raw.gem_value()) + 0.35f * static_cast<float>(raw.gold_value());
        for (const auto& c : raw.camps()) {
            info.hp += c.base_stats().base_health;
            if (c.kind() == cambc::camp_type::monolith) info.monolith = true;
            if (c.kind() == cambc::camp_type::golem) info.golem = true;
        }
        for (auto p : info.spots) group_of[static_cast<std::size_t>(p.y * W + p.x)] = static_cast<std::int16_t>(g);
        groups.push_back(std::move(info));
    }
    respawn_until.assign(groups.size(), 0);

    home_field.compute(static_walls, cambc::map::spawn_region(team::ally), home_visited);

    cambc::bitboard approach;
    altar_tiles = cambc::map::altars().positions();
    for (auto a : altar_tiles)
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                vec2 p{a.x + dx, a.y + dy};
                if (in_bounds(p) && !static_walls.is_set(p)) approach.set(p);
            }
    if (!approach.is_empty()) altar_field.compute(static_walls, approach, altar_visited);

    // Cache ability metadata.
    const auto abilities = me.abilities();
    for (int i = 0; i < 2; ++i) {
        const auto& info = abilities[i].info();
        if (!info) continue;
        Spell& s = spells[i];
        s.used = true;
        std::visit([&](const auto& p) {
            s.kind = std::decay_t<decltype(p)>::kind;
            s.exclusive = p.exclusive_with_movement;
            s.enemies = p.valid_targets.contains(cambc::ability_target_mask::enemies);
            s.camps = p.valid_targets.contains(cambc::ability_target_mask::camps);
            s.ground = p.valid_targets.contains(cambc::ability_target_mask::ground);
            s.self = p.valid_targets.contains(cambc::ability_target_mask::self);
            s.allies = p.valid_targets.contains(cambc::ability_target_mask::allies);
            if constexpr (requires { p.range; }) s.range = p.range;
            if constexpr (requires { p.modifier; }) s.modifier = p.modifier;
            if constexpr (requires { p.radius; }) s.radius = p.radius;
            if constexpr (requires { p.duration; }) s.duration = p.duration;
        }, *info);
    }
}

// Read visible champions once per turn.
void Bot::scan() {
    allies.clear();
    enemies.clear();
    const vec2 here = cambc::self().position();
    for (int side = 0; side < 2; ++side) {
        const team t = side == 0 ? team::ally : team::enemy;
        for (auto pos : cambc::vision::champion_tiles(t).positions()) {
            auto c = cambc::vision::champion_at(pos);
            if (!c.is_visible()) continue;
            Seen s;
            s.pos = pos;
            s.hp = c.health();
            s.max_hp = std::max(1.0f, c.max_health());
            s.str = c.strength();
            s.focus = c.focus();
            s.range = c.attack_range();
            s.stunned = c.buffs().has_stun();
            s.self = pos == here;
            (side == 0 ? allies : enemies).push_back(s);
        }
    }
}

void Bot::shop() {
    auto& me = cambc::self();
    using it = cambc::item_type;
    // Two power emblems of strength, then durability health, alternating afterwards.
    static const std::array<cambc::item_type, 4> plan{it::power_shard_of_strength, it::power_shard_of_strength,
        it::durability_shard_of_health, it::power_shard_of_health};
    for (int guard = 0; guard < 40; ++guard) {
        const int stage = std::min(purchases / 5, 3);
        const auto item = plan[static_cast<std::size_t>(stage)];
        if (me.gold_count() < item.cost() || !me.can_buy(item, 1)) return;
        me.buy(item, 1);
        ++purchases;
    }
}

void Bot::observe_groups() {
    const auto visible = cambc::vision::visible_tiles();
    const int round = static_cast<int>(cambc::round_number());
    for (std::size_t g = 0; g < groups.size(); ++g) {
        if ((groups[g].tiles & visible).is_empty()) continue;
        const auto st = groups_raw[g].state();
        if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&st))
            respawn_until[g] = round + std::max(1, r->timer);
        else
            respawn_until[g] = 0;
    }
}

int Bot::choose_group() {
    auto& me = cambc::self();
    const vec2 here = me.position();
    cambc::bitboard roots;
    roots.set(here);
    cambc::distance_field field;
    cambc::bitboard visited;
    auto walls = static_walls;
    walls.set(here, false);
    field.compute(walls, roots, visited);
    const int round = static_cast<int>(cambc::round_number());
    const float team_dps = 55.0f * static_cast<float>(std::max<std::size_t>(1, cambc::champions(team::ally).size()));

    int best = -1;
    float best_score = -1;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const auto& info = groups[g];
        if (info.monolith) continue;
        if (info.golem && round < 250) continue;
        int d = 1 << 20;
        for (auto p : info.spots)
            if (visited.is_set(p)) d = std::min(d, static_cast<int>(field.get(p)));
        if (d >= (1 << 20)) continue;
        const int wait = respawn_until[g] - round - d;
        if (wait > 0 && respawn_until[g] > 0) {
            if (wait > 15) continue;
        }
        const float clear = info.hp / team_dps;
        float score = info.value / (static_cast<float>(d) + clear + 8.0f + std::max(0, wait));
        if (static_cast<int>(g) == target_group) score *= 1.2f;
        if (score > best_score) { best_score = score; best = static_cast<int>(g); }
    }
    return best;
}

// Keep the whole team on one camp group: adopt the shared target unless we know it is spent.
void Bot::sync_target() {
    const int round = static_cast<int>(cambc::round_number());
    int shared = static_cast<int>(mem_get(0)) - 1;
    bool valid = shared >= 0 && shared < static_cast<int>(groups.size());
    if (valid && respawn_until[static_cast<std::size_t>(shared)] > round + 10) valid = false;
    if (valid && stuck_turns > 12 && shared == target_group) valid = false;
    if (!valid) {
        shared = choose_group();
        stuck_turns = 0;
        mem_set(0, static_cast<std::uint8_t>(shared + 1));
    }
    target_group = shared;
    if (target_group >= 0 && field_group != target_group) {
        field_group = target_group;
        auto walls = static_walls;
        target_field.compute(walls, groups[static_cast<std::size_t>(target_group)].tiles, target_visited);
    }
}

// ---- Movement -------------------------------------------------------------------------------------------

namespace {
cambc::bitboard occupied() {
    return cambc::vision::champion_tiles() | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
}
} // namespace

bool Bot::step_field(const cambc::distance_field& field, const cambc::bitboard& visited, bool allow_equal) {
    auto& me = cambc::self();
    if (moved || move_failed || !me.can_move()) return false;
    const vec2 here = me.position();
    if (!visited.is_set(here)) return false;
    const auto occ = occupied();
    const int cur = field.get(here);
    vec2 best = here;
    int best_v = allow_equal ? cur + 1 : cur;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            if (!dx && !dy) continue;
            const vec2 p{here.x + dx, here.y + dy};
            if (!in_bounds(p) || static_walls.is_set(p) || occ.is_set(p) || !visited.is_set(p)) continue;
            const int v = field.get(p);
            if (v < best_v) { best_v = v; best = p; }
        }
    if (best == here) return false;
    if (me.move_absolute(best)) { moved = true; return true; }
    move_failed = true;
    return false;
}

// Short local path towards a tile (e.g. an enemy champion), routing around visible units.
bool Bot::step_towards(vec2 goal, int max_dist) {
    auto& me = cambc::self();
    if (moved || move_failed || !me.can_move()) return false;
    const vec2 here = me.position();
    auto walls = static_walls | occupied();
    walls.set(here, false);
    walls.set(goal, false);
    cambc::bitboard roots;
    roots.set(goal);
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(walls, roots, visited, static_cast<std::uint16_t>(max_dist));
    if (!visited.is_set(here)) return false;
    const auto occ = occupied();
    const int cur = field.get(here);
    vec2 best = here;
    int best_v = cur;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            if (!dx && !dy) continue;
            const vec2 p{here.x + dx, here.y + dy};
            if (!in_bounds(p) || static_walls.is_set(p) || occ.is_set(p) || !visited.is_set(p)) continue;
            const int v = field.get(p);
            if (v < best_v) { best_v = v; best = p; }
        }
    if (best == here) return false;
    if (me.move_absolute(best)) { moved = true; return true; }
    move_failed = true;
    return false;
}

// Step away from visible enemies, preferring the way home.
bool Bot::step_away() {
    auto& me = cambc::self();
    if (moved || move_failed || !me.can_move()) return false;
    const vec2 here = me.position();
    const auto occ = occupied();
    auto score = [&](vec2 p) {
        int near = 99;
        for (const auto& e : enemies) near = std::min(near, distance(p, e.pos));
        const int home = home_visited.is_set(p) ? static_cast<int>(home_field.get(p)) : 999;
        return near * 3 - home;
    };
    vec2 best = here;
    int best_s = score(here);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            if (!dx && !dy) continue;
            const vec2 p{here.x + dx, here.y + dy};
            if (!in_bounds(p) || static_walls.is_set(p) || occ.is_set(p)) continue;
            const int s = score(p);
            if (s > best_s) { best_s = s; best = p; }
        }
    if (best == here) return false;
    if (me.move_absolute(best)) { moved = true; return true; }
    move_failed = true;
    return false;
}

// ---- Combat ---------------------------------------------------------------------------------------------

bool Bot::try_attack_champion(vec2 pos) {
    auto& me = cambc::self();
    if (attacked || !me.can_attack()) return false;
    if (distance(me.position(), pos) > me.attack_range()) return false;
    if (!cambc::vision::has_champion(pos)) return false;
    auto c = cambc::vision::champion_at(pos);
    if (!c.is_visible() || !c.is_enemy() || c.health() <= 0) return false;
    me.attack(c);
    attacked = true;
    return true;
}

// After our own basic attack this turn, an attack reset grants a second one.
bool Bot::try_reset_attack(vec2 pos, bool champion_target) {
    auto& me = cambc::self();
    if (!attacked || cast_done || me.can_attack() || !me.can_cast()) return false;
    const auto abilities = me.abilities();
    for (int i = 0; i < 2; ++i) {
        if (!spells[i].used || spells[i].kind != cambc::ability_kind::attack_reset) continue;
        if (!abilities[i].is_ready()) continue;
        // Only worth it when the target is still there.
        if (champion_target) {
            if (!cambc::vision::has_champion(pos)) return false;
        } else if (!cambc::vision::has_camp(pos)) {
            return false;
        }
        if (!abilities[i].use()) return false;
        cast_done = true;
        attacked = false;
        if (!me.can_attack()) return true;
        if (champion_target) {
            try_attack_champion(pos);
        } else {
            auto camp = cambc::vision::camp_at(pos);
            if (camp.is_visible() && camp.health() > 0 && distance(me.position(), pos) <= me.attack_range()) {
                me.attack(camp);
                attacked = true;
            }
        }
        return true;
    }
    return false;
}

bool Bot::cast_on_champion(const Seen& target, bool before_move) {
    auto& me = cambc::self();
    if (cast_done || !me.can_cast()) return false;
    const vec2 here = me.position();
    const int d = distance(here, target.pos);
    const auto abilities = me.abilities();
    // Prefer crowd control on a target that is not stunned yet, then raw damage.
    int order[2] = {0, 1};
    auto rank = [&](const Spell& s) {
        if (!s.used) return 9;
        if (s.kind == cambc::ability_kind::cc) return target.stunned ? 3 : 0;
        if (s.kind == cambc::ability_kind::aoe_damage) return s.duration > 0 && !target.stunned ? 1 : 2;
        if (s.kind == cambc::ability_kind::damage) return 2;
        return 9;
    };
    if (rank(spells[1]) < rank(spells[0])) std::swap(order[0], order[1]);
    for (int i : order) {
        const Spell& s = spells[i];
        if (rank(s) >= 9) continue;
        if (s.exclusive && (moved || !before_move)) continue;
        if (d > static_cast<int>(s.range)) continue;
        if (!abilities[i].is_ready()) continue;
        bool ok = false;
        if (s.kind == cambc::ability_kind::aoe_damage) {
            if (!s.ground) continue;
            ok = abilities[i].use(target.pos);
        } else {
            if (!s.enemies) continue;
            if (!cambc::vision::has_champion(target.pos)) continue;
            auto c = cambc::vision::champion_at(target.pos);
            if (!c.is_visible()) continue;
            ok = abilities[i].use(c);
        }
        if (ok) {
            cast_done = true;
            return true;
        }
        return false;
    }
    return false;
}

// Dash to a free tile next to the target (melee chase). Landing damage/stun spells (Heart of Ruin) fit too.
bool Bot::dash_to(vec2 target) {
    auto& me = cambc::self();
    if (cast_done || !me.can_cast()) return false;
    const auto abilities = me.abilities();
    for (int i = 0; i < 2; ++i) {
        const Spell& s = spells[i];
        if (!s.used || s.kind != cambc::ability_kind::dash) continue;
        if (s.exclusive && moved) continue;
        if (!abilities[i].is_ready()) continue;
        const vec2 here = me.position();
        const auto occ = occupied();
        const auto& walls = cambc::map::walls();
        vec2 landing = here;
        int best = 99;
        const int r = std::max(1, static_cast<int>(s.range));
        for (int dx = -r; dx <= r; ++dx)
            for (int dy = -r; dy <= r; ++dy) {
                const vec2 p{here.x + dx, here.y + dy};
                if (p == here || !in_bounds(p) || static_walls.is_set(p) || occ.is_set(p)) continue;
                const int dt = distance(p, target);
                if (dt > 1) continue;
                if (distance(p, here) == 2) {
                    bool path = false;
                    for (int mx = -1; mx <= 1 && !path; ++mx)
                        for (int my = -1; my <= 1 && !path; ++my) {
                            const vec2 mid{here.x + mx, here.y + my};
                            if (mid == here || !in_bounds(mid) || distance(mid, p) != 1) continue;
                            if (!walls.is_set(mid)) path = true;
                        }
                    if (!path) continue;
                }
                const int score = distance(p, here);
                if (score < best) { best = score; landing = p; }
            }
        if (landing == here) return false;
        if (abilities[i].use(landing)) {
            cast_done = true;
            moved = true;
            return true;
        }
        move_failed = true;
        return false;
    }
    return false;
}

void Bot::support_spells() {
    auto& me = cambc::self();
    if (cast_done || !me.can_cast()) return;
    const auto abilities = me.abilities();
    for (int i = 0; i < 2; ++i) {
        const Spell& s = spells[i];
        if (!s.used || !abilities[i].is_ready()) continue;
        if (s.exclusive && moved) continue;
        if (s.kind == cambc::ability_kind::heal && s.allies) {
            const Seen* best = nullptr;
            float best_missing = s.modifier * me.focus() * 0.6f;
            for (const auto& a : allies) {
                if (a.self || distance(a.pos, me.position()) > static_cast<int>(s.range)) continue;
                const float missing = a.max_hp - a.hp;
                if (missing > best_missing) { best_missing = missing; best = &a; }
            }
            if (best && cambc::vision::has_champion(best->pos)) {
                auto c = cambc::vision::champion_at(best->pos);
                if (c.is_visible() && abilities[i].use(c)) { cast_done = true; return; }
            }
        } else if (s.kind == cambc::ability_kind::shield) {
            bool threat = me.health() < me.max_health() * 0.8f;
            for (const auto& e : enemies)
                if (distance(e.pos, me.position()) <= 3) threat = true;
            if (threat && (s.self || s.ground)) {
                const bool ok = s.self ? abilities[i].use() : abilities[i].use(me.position());
                if (ok) { cast_done = true; return; }
            }
        }
    }
}

void Bot::engage(const Seen& target) {
    auto& me = cambc::self();
    const bool melee = me.attack_range() <= 1;
    int d = distance(me.position(), target.pos);

    if (d <= me.attack_range()) {
        try_attack_champion(target.pos);
        cast_on_champion(target, true);
        try_reset_attack(target.pos, true);
        if (melee && !attacked) step_towards(target.pos, 12);
        return;
    }

    if (melee) {
        // Dash straight in, or walk then dash.
        if (d <= 3 && dash_to(target.pos)) {
            try_attack_champion(target.pos);
            return;
        }
        if (d >= 2 && d <= 4 && cast_on_champion(target, true)) {
            // Exclusive stun/damage cast: no walking this turn.
            if (!moved) step_towards(target.pos, 12);
            return;
        }
        step_towards(target.pos, 12);
        d = distance(me.position(), target.pos);
        if (d <= 1) {
            try_attack_champion(target.pos);
            try_reset_attack(target.pos, true);
        } else if (d <= 3) {
            dash_to(target.pos);
            try_attack_champion(target.pos);
        }
        cast_on_champion(target, false);
        return;
    }

    // Ranged: spells first (the spell range exceeds the attack range), then walk into range.
    if (cast_on_champion(target, true)) return;
    step_towards(target.pos, 12);
    cast_on_champion(target, false);
}

void Bot::flee() {
    auto& me = cambc::self();
    // A ranged champion with a target in range and no melee threat adjacent keeps shooting.
    bool melee_threat = false;
    for (const auto& e : enemies)
        if (is_melee(e) && distance(e.pos, me.position()) <= 2) melee_threat = true;
    if (!melee_threat && me.attack_range() > 1 && me.health() > me.max_health() * kFleeHp) {
        for (const auto& e : enemies)
            if (try_attack_champion(e.pos)) return;
    }
    // Stun the closest chaser to cover the escape.
    const Seen* closest = nullptr;
    for (const auto& e : enemies)
        if (!closest || distance(e.pos, me.position()) < distance(closest->pos, me.position())) closest = &e;
    if (closest && distance(closest->pos, me.position()) <= 2) {
        for (int i = 0; i < 2; ++i)
            if (spells[i].used && spells[i].kind == cambc::ability_kind::cc && !spells[i].exclusive) {
                cast_on_champion(*closest, false);
                break;
            }
    }
    step_away();
    if (!attacked)
        for (const auto& e : enemies)
            if (try_attack_champion(e.pos)) break;
}

// Returns true when the fight logic used the turn.
bool Bot::fight() {
    if (enemies.empty()) return false;
    auto& me = cambc::self();
    const vec2 here = me.position();
    last_enemy_round = static_cast<int>(cambc::round_number());

    float a_hp = 0, a_dps = 0, e_hp = 0, e_dps = 0;
    for (const auto& a : allies)
        if (distance(a.pos, here) <= 7) { a_hp += a.hp; a_dps += dps_of(a); }
    for (const auto& e : enemies)
        if (distance(e.pos, here) <= 9) { e_hp += e.hp; e_dps += dps_of(e) * (e.stunned ? 0.5f : 1.0f); }
    if (e_hp <= 0) return false;
    const float ratio = (a_hp * a_dps) / std::max(1.0f, e_hp * e_dps);

    // Pick the enemy that dies soonest, weighting the walk to reach it.
    const int my_range = me.attack_range();
    const Seen* target = nullptr;
    float best = 1e9f;
    for (const auto& e : enemies) {
        const int d = distance(e.pos, here);
        float score = e.hp + 60.0f * static_cast<float>(std::max(0, d - my_range)) - (e.stunned ? 120.0f : 0.0f);
        if (score < best) { best = score; target = &e; }
    }
    if (!target) return false;
    const int d = distance(target->pos, here);
    const float burst = me.strength() * 2.0f + me.focus();
    const bool killable = target->hp <= burst && d <= my_range + 2;
    const float hp_frac = me.health() / std::max(1.0f, me.max_health());

    // Share the sighting so the pack converges.
    mem_set(2, static_cast<std::uint8_t>(target->pos.x));
    mem_set(3, static_cast<std::uint8_t>(target->pos.y));
    mem_set(4, static_cast<std::uint8_t>(cambc::round_number() & 0xFF));
    mem_set(5, static_cast<std::uint8_t>(std::min<std::size_t>(enemies.size(), 255)));

    if (hp_frac < kFleeHp && !killable) {
        flee();
        return true;
    }
    if (ratio >= kEngageRatio || killable || (target->hp < 0.35f * target->max_hp && ratio > kHoldRatio)) {
        engage(*target);
        return true;
    }
    if (ratio < kHoldRatio) {
        flee();
        return true;
    }
    // Even-ish: take free hits without advancing.
    if (d <= my_range) {
        try_attack_champion(target->pos);
        cast_on_champion(*target, true);
        try_reset_attack(target->pos, true);
        return true;
    }
    if (d <= 4 && cast_on_champion(*target, true)) return true;
    // Too far to matter: carry on with the plan.
    return d <= 5;
}

// ---- Economy --------------------------------------------------------------------------------------------

bool Bot::bank() {
    auto& me = cambc::self();
    const vec2 here = me.position();
    for (auto altar : altar_tiles) {
        if (distance(here, altar) > cambc::constants::submit_gems_range) continue;
        if (cambc::vision::has_altar_lock(altar)) {
            auto lock = cambc::vision::altar_lock_at(altar);
            if (lock.is_enemy()) {
                if (me.can_attack() && !attacked) {
                    me.attack(lock);
                    attacked = true;
                    return true;
                }
                continue;
            }
        }
        me.submit_gems(altar);
        return true;
    }
    if (altar_visited.is_set(here) && step_field(altar_field, altar_visited, false)) return true;
    return false;
}

bool Bot::attack_camps(bool use_spells) {
    auto& me = cambc::self();
    const vec2 here = me.position();
    const int range = me.attack_range();
    const int near_allies = static_cast<int>(std::count_if(allies.begin(), allies.end(),
        [&](const Seen& a) { return distance(a.pos, here) <= 6; }));

    // Best camp in reach: lowest health among target-group or awake camps.
    vec2 best_pos{-1, -1};
    float best_hp = 1e9f;
    int spell_reach = range;
    for (int i = 0; i < 2; ++i)
        if (use_spells && spells[i].used && spells[i].camps && !cast_done) spell_reach = std::max(spell_reach, static_cast<int>(spells[i].range));
    for (auto pos : cambc::vision::camp_tiles().positions()) {
        const int d = distance(pos, here);
        if (d > spell_reach) continue;
        auto c = cambc::vision::camp_at(pos);
        if (!c.is_visible()) continue;
        if (c.kind() == cambc::camp_type::monolith) continue;
        if (c.kind() == cambc::camp_type::golem && near_allies < 3) continue;
        const int g = group_of[static_cast<std::size_t>(pos.y * W + pos.x)];
        const bool awake = c.is_awake();
        if (g != target_group && !awake) continue;
        const float hp = c.health() + (d > range ? 10000.0f : 0.0f);
        if (hp <= 0) continue;
        if (hp < best_hp) { best_hp = hp; best_pos = pos; }
    }
    if (best_pos.x < 0) return false;
    const int d = distance(best_pos, here);

    bool acted = false;
    if (use_spells && !cast_done && me.can_cast()) {
        const auto abilities = me.abilities();
        for (int i = 0; i < 2 && !cast_done; ++i) {
            const Spell& s = spells[i];
            if (!s.used || !abilities[i].is_ready()) continue;
            if (s.exclusive && moved) continue;
            if (d > static_cast<int>(s.range)) continue;
            // Melee champions only spend walking-exclusive casts when the camp is already in reach.
            if (s.exclusive && range <= 1 && d > range) continue;
            if (s.kind == cambc::ability_kind::damage && s.camps) {
                auto c = cambc::vision::camp_at(best_pos);
                if (c.is_visible() && abilities[i].use(c)) { cast_done = true; acted = true; }
            } else if (s.kind == cambc::ability_kind::aoe_damage && s.ground) {
                if (abilities[i].use(best_pos)) { cast_done = true; acted = true; }
            }
        }
    }
    if (d <= range && !attacked && me.can_attack() && cambc::vision::has_camp(best_pos)) {
        auto c = cambc::vision::camp_at(best_pos);
        if (c.is_visible() && c.health() > 0) {
            me.attack(c);
            attacked = true;
            acted = true;
            if (use_spells) try_reset_attack(best_pos, false);
        }
    }
    return acted && d <= range;
}

void Bot::farm() {
    auto& me = cambc::self();
    const int round = static_cast<int>(cambc::round_number());
    const bool spells_ok = round - last_enemy_round > kEnemyMemory;
    if (attack_camps(spells_ok)) return;
    if (target_group < 0) return;
    const auto& info = groups[static_cast<std::size_t>(target_group)];
    const vec2 here = me.position();
    if (info.tiles.is_set(here)) return; // capturing
    const vec2 before = here;
    if (!step_field(target_field, target_visited, false)) step_field(target_field, target_visited, true);
    if (me.position() == before) ++stuck_turns; else stuck_turns = 0;
    attack_camps(spells_ok);
}

// ---- Turn -----------------------------------------------------------------------------------------------

void Bot::play_turn() {
    auto& me = cambc::self();
    if (!initialised) init();
    moved = attacked = cast_done = move_failed = false;

    if (!me.is_alive()) {
        shop();
        return;
    }
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    if (at_spawn) shop();
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;

    scan();
    observe_groups();
    sync_target();

    const int round = static_cast<int>(cambc::round_number());
    const float hp_frac = me.health() / std::max(1.0f, me.max_health());
    int nearest_enemy = 99;
    for (const auto& e : enemies) nearest_enemy = std::min(nearest_enemy, distance(e.pos, me.position()));

    if (fight()) {
        support_spells();
        return;
    }

    // Heal up on spawn before heading out again.
    if (at_spawn && hp_frac < 0.9f && nearest_enemy > 4) return;
    if (!at_spawn && hp_frac < kRecallHp && nearest_enemy > 6) {
        me.recall();
        return;
    }

    const int gems = static_cast<int>(me.gem_count());
    const int altar_d = altar_visited.is_set(me.position()) ? static_cast<int>(altar_field.get(me.position())) : 999;
    const bool late = round + altar_d + 20 >= static_cast<int>(cambc::constants::max_rounds);
    if (gems > 0 && (gems >= kBankGems || altar_d <= 3 || late)) {
        if (bank()) {
            support_spells();
            return;
        }
    }

    farm();
    support_spells();
}
