// Safe banker: farm camps, bank every gem quickly, never fight unless clearly ahead.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;
using cambc::bitboard;
using cambc::distance_field;

namespace {

constexpr bool kDebug = false; // per-100-round trace lines on stdout
constexpr std::uint16_t UNREACHED = distance_field::unreachable;
// Indexed by camp_type: wolf, elk, gem cluster, gold cluster, monolith, golem.
constexpr std::array<float, 6> CAMP_HP{1350, 2400, 120, 120, 20000, 5000};
constexpr std::array<float, 6> CAMP_STR{20, 60, 0, 0, 200, 120};
constexpr std::array<int, 6> CAMP_RANGE{3, 4, 0, 0, 5, 4};

int cheb(vec2 a, vec2 b) { return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)); }

std::vector<int> tile_owner; // tile -> group * 256 + camp index, or -1

const std::array<vec2, 8> DIRS{vec2::north, vec2::north_east, vec2::east, vec2::south_east,
    vec2::south, vec2::south_west, vec2::west, vec2::north_west};

} // namespace

bool Bot::fresh() const { return cambc::round_number() == turn_round; }

int Bot::hd(vec2 p) const {
    if (p.x < 0 || p.y < 0 || p.x >= W || p.y >= H) return UNREACHED;
    return home_dist[p.y * W + p.x];
}

int Bot::ed(vec2 p) const {
    if (p.x < 0 || p.y < 0 || p.x >= W || p.y >= H) return UNREACHED;
    return enemy_dist[p.y * W + p.x];
}

// Chebyshev box; the engine helper is used only well inside the map.
void Bot::box(bitboard& b, vec2 c, int r) const {
    if (c.x - r >= 0 && c.y - r >= 0 && c.x + r < W && c.y + r < H) {
        b.draw_chebyshev(c, static_cast<float>(r));
        return;
    }
    for (int y = std::max(0, c.y - r); y <= std::min(H - 1, c.y + r); ++y)
        for (int x = std::max(0, c.x - r); x <= std::min(W - 1, c.x + r); ++x)
            b.set({x, y});
}

void Bot::read_kit() {
    auto& me = cambc::self();
    auto abilities = me.abilities();
    for (int i = 0; i < 2; ++i) {
        const auto& info = abilities[i].info();
        if (!info) continue;
        Kit k;
        k.used = true;
        std::visit([&](const auto& v) {
            k.kind = v.kind;
            k.excl = v.exclusive_with_movement;
            if constexpr (requires { v.range; }) k.range = v.range;
            k.camps = v.valid_targets.contains(cambc::ability_target_mask::camps);
            k.enemies = v.valid_targets.contains(cambc::ability_target_mask::enemies);
            k.ground = v.valid_targets.contains(cambc::ability_target_mask::ground);
        }, *info);
        kit[i] = k;
        if (k.kind == cambc::ability_kind::attack_reset) reset_slot = i;
        if (k.kind == cambc::ability_kind::shield) shield_slot = i;
    }
}

void Bot::init() {
    inited = true;
    auto& me = cambc::self();
    W = cambc::map::width();
    H = cambc::map::height();
    ranged = me.base_stats().base_attack_range > 1;
    static_block = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);

    // Walking distances from both spawns over static terrain.
    {
        distance_field f;
        bitboard visited;
        f.compute(static_block, cambc::map::spawn_region(team::ally), visited);
        home_dist = f.export_linear_buffer();
        bitboard walls_e = static_block & ~cambc::map::spawn_region(team::enemy);
        walls_e |= cambc::map::spawn_region(team::ally);
        f.compute(walls_e, cambc::map::spawn_region(team::enemy), visited);
        enemy_dist = f.export_linear_buffer();
    }

    tile_owner.assign(static_cast<std::size_t>(W * H), -1);
    int gi = 0;
    map_groups = cambc::map::camp_groups();
    for (const auto& group : map_groups) {
        Group g;
        g.gems = static_cast<int>(group.gem_value());
        g.gold = static_cast<int>(group.gold_value());
        g.respawn = group.max_respawn_timer();
        g.home = UNREACHED;
        g.enemy_home = UNREACHED;
        int ci = 0;
        for (const auto& c : group.camps()) {
            const int t = std::clamp(static_cast<int>(c.kind()), 0, 5);
            const vec2 p = c.spawn_position();
            g.tiles.push_back(p);
            g.types.push_back(t);
            g.hp.push_back(CAMP_HP[t]);
            g.maxhp.push_back(CAMP_HP[t]);
            g.str.push_back(CAMP_STR[t]);
            g.range.push_back(CAMP_RANGE[t]);
            if (t >= 4) g.boss = true;
            if (p.x >= 0 && p.y >= 0 && p.x < W && p.y < H) tile_owner[p.y * W + p.x] = gi * 256 + ci;
            // Distance from spawn to a free tile next to the camp.
            for (auto d : DIRS) {
                g.home = std::min(g.home, hd(p + d));
                g.enemy_home = std::min(g.enemy_home, ed(p + d));
            }
            ++ci;
        }
        groups.push_back(std::move(g));
        ++gi;
    }

    field_buf.assign(static_cast<std::size_t>(W * H), 0);
    vis_buf.assign(bitboard::linear_buffer_size(), 0);
    altars = cambc::map::altars().positions();
    altar_goal.clear();
    for (auto a : altars) box(altar_goal, a, cambc::constants::submit_gems_range);
    altar_goal &= ~static_block;

    // Index among allies by handle, for shared-memory claims.
    std::vector<std::uint32_t> handles;
    for (const auto& c : cambc::champions(team::ally))
        handles.push_back(static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(c)));
    std::sort(handles.begin(), handles.end());
    const auto mine = static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(me));
    my_index = static_cast<int>(std::find(handles.begin(), handles.end(), mine) - handles.begin());
    if (my_index > 4) my_index = 4;

    read_kit();
    assign_sectors(static_cast<int>(handles.size()));
}

// Same split on every ally: our side's groups sorted by angle around our spawn, cut into equal-value sectors.
void Bot::assign_sectors(int n) {
    if (n <= 1) return;
    const auto spawn = cambc::map::spawn_region(team::ally).positions();
    if (spawn.empty()) return;
    float cx = 0, cy = 0;
    for (auto p : spawn) { cx += p.x; cy += p.y; }
    cx /= spawn.size();
    cy /= spawn.size();
    std::vector<std::pair<float, int>> order;
    float total = 0;
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& g = groups[gi];
        if (g.home == UNREACHED || g.home > g.enemy_home || g.boss) continue;
        order.push_back({std::atan2(g.tiles[0].y - cy, g.tiles[0].x - cx), static_cast<int>(gi)});
        total += g.gems + 0.35f * g.gold;
    }
    std::sort(order.begin(), order.end());
    float acc = 0;
    for (const auto& [angle, gi] : order) {
        auto& g = groups[gi];
        const float v = g.gems + 0.35f * g.gold;
        g.owner = std::min(n - 1, static_cast<int>((acc + v * 0.5f) / total * n));
        acc += v;
    }
}

// Health first (the archetype), with camp-damage reduction to keep the farming loop alive.
void Bot::buy_items() {
    auto& me = cambc::self();
    using IT = cambc::item_type;
    static const std::array<IT, 5> plan{IT::jungle_shard_of_strength, IT::monster_shard_of_strength,
        IT::durability_shard_of_health, IT::power_shard_of_health, IT::power_shard_of_strength};
    for (int guard = 0; guard < 40; ++guard) {
        IT item = bought < 20 ? plan[bought / 5] : (bought % 2 ? IT::power_shard_of_strength : IT::power_shard_of_health);
        if (me.gold_count() < item.cost() || !me.can_buy(item, 1)) return;
        try {
            me.buy(item, 1);
        } catch (const cambc::error&) {
            return;
        }
        ++bought;
    }
}

float Bot::my_dps() const {
    auto& me = cambc::self();
    float d = me.strength();
    for (int i = 0; i < 2; ++i)
        if (kit[i].used && kit[i].camps && (kit[i].kind == cambc::ability_kind::damage
            || kit[i].kind == cambc::ability_kind::aoe_damage || kit[i].kind == cambc::ability_kind::cc))
            d += me.focus() * 0.15f;
    if (reset_slot >= 0) d *= 1.12f;
    return std::max(d, 10.0f);
}

// Damage per round from awake camps that could hit tile p.
float Bot::incoming_at(vec2 p) const {
    float sum = 0;
    for (const auto& t : threats)
        if (cheb(p, t.pos) <= t.range) sum += t.str;
    return sum;
}

void Bot::observe() {
    auto& me = cambc::self();
    visible = cambc::vision::visible_tiles();
    visible.export_linear_buffer(std::span{vis_buf});
    const int now = static_cast<int>(turn_round);
    auto vis = [&](vec2 t) { const int i = t.y * W + t.x; return (vis_buf[i >> 3] >> (i & 7)) & 1; };

    // Camp groups in view: state, live health, deaths.
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        auto& g = groups[gi];
        if (g.state == 3 && now >= g.respawn_at) {
            g.state = 0;
            for (std::size_t i = 0; i < g.hp.size(); ++i) g.hp[i] = g.maxhp[i];
        }
        if (g.state == 2 && now - g.seen > 20) {
            g.state = 3;
            g.respawn_at = g.seen + 5 + g.respawn;
        }
        bool any = false;
        for (auto t : g.tiles)
            if (cheb(t, pos) <= 9 && vis(t)) { any = true; break; }
            else if (cheb(t, pos) > 14) break;
        if (!any) continue;
        g.seen = now;
        try {
            const auto st = map_groups[gi].state();
            if (std::holds_alternative<cambc::map::camp_group_respawning>(st)) {
                g.state = 3;
                g.respawn_at = now + std::get<cambc::map::camp_group_respawning>(st).timer;
                for (auto& h : g.hp) h = 0;
                continue;
            }
            if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                g.state = 2;
                for (auto& h : g.hp) h = 0;
                continue;
            }
            if (g.state != 0) for (std::size_t i = 0; i < g.hp.size(); ++i) g.hp[i] = g.maxhp[i];
            g.state = 0;
        } catch (const std::exception&) {
            continue;
        }
        for (std::size_t i = 0; i < g.tiles.size(); ++i) {
            const auto t = g.tiles[i];
            if (!vis(t)) continue;
            try {
                if (!cambc::vision::has_camp(t)) { g.hp[i] = 0; continue; }
                const auto c = cambc::vision::camp_at(t);
                g.hp[i] = c.health();
            } catch (const std::exception&) {
            }
        }
    }

    // Awake camps in view: the only camps that hit us.
    threats.clear();
    try {
        for (auto p : cambc::vision::awake_camp_tiles().positions()) {
            const int o = tile_owner[p.y * W + p.x];
            if (o < 0) continue;
            const auto& g = groups[o / 256];
            const int i = o % 256;
            if (g.str[i] > 0) threats.push_back({p, g.str[i], g.range[i]});
        }
    } catch (const std::exception&) {
    }

    // Enemy champions in view.
    foes.clear();
    for (auto p : cambc::vision::champion_tiles(team::enemy).positions()) {
        try {
            const auto c = cambc::vision::champion_at(p);
            Foe f{p, c.health(), c.max_health(), c.strength() + 0.25f * c.focus(), c.attack_range(), c.gem_count(),
                c.is_recalling(), c.is_submitting_gems()};
            foes.push_back(f);
        } catch (const std::exception&) {
        }
    }
    ally_pos.clear();
    ally_hp.clear();
    ally_dps.clear();
    for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
        if (p == pos) continue;
        try {
            const auto c = cambc::vision::champion_at(p);
            ally_pos.push_back(p);
            ally_hp.push_back(c.health());
            ally_dps.push_back(c.strength() + 0.25f * c.focus());
        } catch (const std::exception&) {
        }
    }
    (void)me;

    // Share the closest enemy sighting; read the team's latest one.
    try {
        const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
        for (const auto& f : foes)
            if (!enemy_spawn.is_set(f.pos)) {
                std::array<std::uint8_t, 3> rec{static_cast<std::uint8_t>(f.pos.x), static_cast<std::uint8_t>(f.pos.y),
                    static_cast<std::uint8_t>(turn_round & 0xff)};
                cambc::shared_memory.write(5, std::span{rec});
                break;
            }
        std::array<std::uint8_t, 3> rec{};
        cambc::shared_memory.read(5, std::span{rec});
        const int age = static_cast<int>((turn_round - rec[2]) & 0xff);
        if (!(rec[0] == 0 && rec[1] == 0 && rec[2] == 0) && age < 40) {
            last_enemy = {rec[0], rec[1]};
            last_enemy_round = now - age;
        }
    } catch (const std::exception&) {
    }
}

// Spells on a camp in range; casts that exclude walking only before we move.
bool Bot::cast_on_camp(const cambc::camp& c) {
    auto& me = cambc::self();
    vec2 cp;
    try { cp = c.position(); } catch (const std::exception&) { return false; }
    auto abilities = me.abilities();
    for (int i = 0; i < 2; ++i) {
        const auto& k = kit[i];
        if (!k.used || (k.excl && moved)) continue;
        const bool dmg = k.kind == cambc::ability_kind::damage || k.kind == cambc::ability_kind::cc
            || k.kind == cambc::ability_kind::aoe_damage;
        if (!dmg) continue;
        if (cheb(pos, cp) > static_cast<int>(k.range)) continue;
        try {
            if (!abilities[i].is_ready()) continue;
            if (k.camps && k.kind != cambc::ability_kind::aoe_damage) {
                if (abilities[i].use(c)) return true;
            } else if (k.ground) {
                if (abilities[i].use(cp)) return true;
            }
        } catch (const std::exception&) {
        }
    }
    return false;
}

bool Bot::cast_on_champion(const cambc::champion& c, bool kill_only) {
    auto& me = cambc::self();
    vec2 cp;
    float hp;
    try { cp = c.position(); hp = c.health(); } catch (const std::exception&) { return false; }
    auto abilities = me.abilities();
    for (int i = 0; i < 2; ++i) {
        const auto& k = kit[i];
        if (!k.used || (k.excl && moved)) continue;
        const bool dmg = k.kind == cambc::ability_kind::damage || k.kind == cambc::ability_kind::cc
            || k.kind == cambc::ability_kind::aoe_damage;
        if (!dmg) continue;
        if (cheb(pos, cp) > static_cast<int>(k.range)) continue;
        try {
            if (!abilities[i].is_ready()) continue;
            if (kill_only && k.kind != cambc::ability_kind::cc && abilities[i].damage_amount(c) < hp) continue;
            if (k.enemies && k.kind != cambc::ability_kind::aoe_damage) {
                if (abilities[i].use(c)) return true;
            } else if (k.ground) {
                if (abilities[i].use(cp)) return true;
            }
        } catch (const std::exception&) {
        }
    }
    return false;
}

// Attack reset right after our own hit, then hit again.
void Bot::after_hit(const cambc::unit& target) {
    auto& me = cambc::self();
    if (reset_slot < 0) return;
    try {
        auto abilities = me.abilities();
        if (!abilities[reset_slot].is_ready()) return;
        if (!abilities[reset_slot].use()) return;
        if (me.can_attack() && target.is_visible() && target.health() > 0
            && cheb(pos, target.position()) <= me.attack_range())
            me.attack(target);
    } catch (const std::exception&) {
    }
}

// One step along a shortest path towards any goal tile.
bool Bot::step_towards(const bitboard& goals_in, bool enter_goals) {
    auto& me = cambc::self();
    if (moved || move_failed || !me.can_move()) return false;
    bitboard walls_saved;
    if (enter_goals) {
        walls_saved = walls;
        walls &= ~goals_in;
    }
    const bool r = step_inner(goals_in);
    if (enter_goals) walls = std::move(walls_saved);
    return r;
}

bool Bot::step_inner(const bitboard& goals_in) {
    bitboard saved;
    const bool avoid = any_danger && !danger.is_set(pos);
    if (avoid) {
        saved = walls;
        walls |= danger & ~goals_in;
    }
    const bool r = step_core(goals_in);
    if (avoid) walls = std::move(saved);
    return r;
}

bool Bot::step_core(const bitboard& goals_in) {
    auto& me = cambc::self();
    bitboard goals = goals_in & ~walls;
    if (goals.is_empty() || goals.is_set(pos)) return false;
    distance_field f;
    bitboard visited, dest;
    dest.set(pos);
    f.compute_to(walls, goals, dest, visited);
    const auto here = f.get(pos);
    if (here == UNREACHED) {
        // Route blocked by units: fall back to static terrain only.
        bitboard w2 = static_block;
        w2.set(pos, false);
        f.compute_to(w2, goals, dest, visited);
    }
    std::uint16_t best = f.get(pos);
    vec2 step = pos;
    for (auto d : DIRS) {
        const vec2 n = pos + d;
        if (n.x < 0 || n.y < 0 || n.x >= W || n.y >= H) continue;
        if (walls.is_set(n)) continue;
        const auto v = f.get(n);
        if (v < best) { best = v; step = n; }
    }
    if (step == pos || !fresh()) return false;
    try {
        if (me.move_absolute(step)) {
            moved = true;
            pos = step;
            return true;
        }
    } catch (const std::exception& e) {
    }
    move_failed = true;
    return false;
}

// Walk out of reach of awake camps, preferring tiles closer to home.
bool Bot::step_away_from_camps() {
    auto& me = cambc::self();
    if (moved || move_failed || !me.can_move()) return false;
    float best_inc = incoming_at(pos);
    int best_h = hd(pos);
    vec2 step = pos;
    for (auto d : DIRS) {
        const vec2 n = pos + d;
        if (n.x < 0 || n.y < 0 || n.x >= W || n.y >= H || walls.is_set(n)) continue;
        const float inc = incoming_at(n);
        const int h = hd(n);
        if (inc < best_inc || (inc == best_inc && h < best_h)) { best_inc = inc; best_h = h; step = n; }
    }
    if (step == pos || !fresh()) return false;
    try {
        if (me.move_absolute(step)) { moved = true; pos = step; return true; }
    } catch (const std::exception&) {
    }
    move_failed = true;
    return false;
}

// Enemies decide everything else: fight only when clearly stronger, else run home.
bool Bot::handle_enemies() {
    auto& me = cambc::self();
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    std::vector<int> near;
    for (std::size_t i = 0; i < foes.size(); ++i)
        if (cheb(foes[i].pos, pos) <= 7 && !enemy_spawn.is_set(foes[i].pos)) near.push_back(static_cast<int>(i));
    if (near.empty()) return false;

    float my_hp = me.health(), sum_hp = my_hp, sum_dps = me.strength() + 0.25f * me.focus();
    for (std::size_t i = 0; i < ally_pos.size(); ++i)
        if (cheb(ally_pos[i], pos) <= 6) { sum_hp += ally_hp[i]; sum_dps += ally_dps[i]; }
    float e_hp = 0, e_dps = 0;
    for (int i : near) { e_hp += foes[i].hp; e_dps += foes[i].dps; }
    const float ratio = (sum_hp * sum_dps) / std::max(1.0f, e_hp * e_dps);
    const float need = 1.6f + me.gem_count() / 60.0f;
    flee_until = static_cast<int>(turn_round) + 3;

    if (ratio >= need && me.health() > me.max_health() * 0.4f) return fight(near);
    int dclose = 99;
    for (int i : near) dclose = std::min(dclose, cheb(foes[i].pos, pos) - foes[i].range);
    if (ratio >= 1.1f && me.health() > me.max_health() * 0.5f && (dclose > 1 || me.gem_count() < 20)) {
        // Even fight: do not start it, but answer anyone already in reach and keep farming.
        for (int i : near)
            if (cheb(foes[i].pos, pos) <= me.attack_range() && me.can_attack()) {
                try {
                    const auto c = cambc::vision::champion_at(foes[i].pos);
                    if (fresh()) { cast_on_champion(c, false); me.attack(c); attacked = true; after_hit(c); }
                } catch (const std::exception&) {
                }
                return true;
            }
        return false;
    }

    // Losing side. Take a free kill shot, bank gems if the altar is right here, else run.
    for (int i : near) {
        const auto& f = foes[i];
        if (cheb(f.pos, pos) <= me.attack_range() && f.hp <= me.strength() * 0.9f && me.can_attack()) {
            try {
                const auto c = cambc::vision::champion_at(f.pos);
                if (fresh()) { me.attack(c); attacked = true; }
            } catch (const std::exception&) {
            }
        }
    }
    if (me.gem_count() > 0) {
        int dmin = 99;
        for (int i : near) dmin = std::min(dmin, cheb(foes[i].pos, pos));
        if (dmin >= 3)
            for (auto a : altars)
                if (cheb(a, pos) <= cambc::constants::submit_gems_range && !cambc::vision::has_altar_lock(a)) {
                    try {
                        if (fresh()) { me.submit_gems(a); return true; }
                    } catch (const std::exception&) {
                    }
                }
    }
    flee();
    return true;
}

bool Bot::fight(const std::vector<int>& near) {
    auto& me = cambc::self();
    // Best target: killable, then channeling, then lowest health.
    int best = -1;
    float best_score = -1e9f;
    for (int i : near) {
        const auto& f = foes[i];
        float s = -f.hp;
        if (f.hp <= me.strength() * 0.9f) s += 5000;
        if (f.tributing || f.recalling) s += 1000;
        s += f.gems * 10.0f;
        s -= cheb(f.pos, pos) * 100.0f;
        if (s > best_score) { best_score = s; best = i; }
    }
    if (best < 0) return false;
    const vec2 tp = foes[best].pos;
    try {
        auto c = cambc::vision::champion_at(tp);
        if (cheb(tp, pos) <= me.attack_range()) {
            cast_on_champion(c, false);
            if (me.can_attack() && fresh()) { me.attack(c); attacked = true; after_hit(c); }
            return true;
        }
        // Do not chase far from home with gems in the bag.
        if (me.gem_count() > 30 && cheb(tp, pos) > 3) { flee(); return true; }
        bitboard goal;
        box(goal, tp, me.attack_range());
        step_towards(goal);
        if (cheb(tp, pos) <= me.attack_range()) {
            cast_on_champion(c, false);
            if (me.can_attack() && fresh() && c.is_visible()) { me.attack(c); attacked = true; after_hit(c); }
        }
    } catch (const std::exception&) {
    }
    return true;
}

void Bot::flee() {
    auto& me = cambc::self();
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(pos);
    if (at_spawn) return; // enemies cannot enter our spawn
    int dmin = 99;
    for (const auto& f : foes) dmin = std::min(dmin, cheb(f.pos, pos));
    if (dmin >= 6 && incoming_at(pos) == 0 && !me.is_recalling()
        && (me.health() < me.max_health() * 0.5f || me.gem_count() >= 20)) {
        try {
            if (fresh()) { me.recall(); return; }
        } catch (const std::exception&) {
        }
    }
    if (moved || move_failed || !me.can_move()) return;
    vec2 step = pos;
    float best = 1e9f;
    for (int k = -1; k < 8; ++k) {
        const vec2 n = k < 0 ? pos : pos + DIRS[k];
        if (n.x < 0 || n.y < 0 || n.x >= W || n.y >= H) continue;
        if (k >= 0 && walls.is_set(n)) continue;
        int dn = 99;
        float threat = 0;
        for (const auto& f : foes) {
            const int d = cheb(f.pos, n);
            dn = std::min(dn, d);
            if (d <= f.range + 1) threat += f.dps;
        }
        const float s = static_cast<float>(hd(n)) - 3.0f * std::min(dn, 7) + threat * 0.05f + incoming_at(n) * 0.05f;
        if (s < best) { best = s; step = n; }
    }
    if (step == pos || !fresh()) return;
    try {
        if (me.move_absolute(step)) { moved = true; pos = step; }
        else move_failed = true;
    } catch (const std::exception&) {
        move_failed = true;
    }
}

bool Bot::want_tribute() {
    auto& me = cambc::self();
    const auto gems = me.gem_count();
    if (gems == 0 || altars.empty()) return false;
    return true; // decided by distance in go_tribute
}

// Walk to the nearest unlocked altar and bank the bag. Returns true if this took the turn.
bool Bot::go_tribute() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems == 0) return false;
    for (auto a : altars)
        if (cheb(a, pos) <= cambc::constants::submit_gems_range && !cambc::vision::has_altar_lock(a)) {
            try {
                if (fresh()) { me.submit_gems(a); return true; }
            } catch (const std::exception&) {
            }
        }
    bitboard goals = altar_goal;
    for (auto a : altars)
        if (cambc::vision::has_altar_lock(a)) {
            bitboard b;
            box(b, a, cambc::constants::submit_gems_range);
            goals &= ~b;
        }
    goals &= ~walls;
    if (goals.is_empty()) return false;
    distance_field f;
    bitboard visited, dest;
    dest.set(pos);
    f.compute_to(walls, goals, dest, visited);
    const int d = f.get(pos);
    if (d == UNREACHED) return false;
    const int now = static_cast<int>(turn_round);
    const int left = static_cast<int>(cambc::constants::max_rounds) - now;
    bool go = gems >= 40 || d <= 4 + gems / 4 || left <= d + (gems + 9) / 10 + 6
        || (now - last_enemy_round < 30 && gems >= 8) || target < 0;
    if (!go) return false;
    // Give up a trip that stops making progress (blocked or guarded altar) for a while.
    const bool urgent = left <= d + (gems + 9) / 10 + 6;
    if (!urgent && now < tribute_ban) return false;
    if (d < trib_best) { trib_best = d; trib_since = now; }
    if (now - trib_last > 3) { trib_best = d; trib_since = now; }
    trib_last = now;
    if (!urgent && now - trib_since > 10) {
        tribute_ban = now + 60;
        trib_best = 1 << 20;
        return false;
    }
    step_towards(goals);
    return true;
}

bool Bot::maybe_recall() {
    auto& me = cambc::self();
    if (cambc::map::spawn_region(team::ally).is_set(pos)) return false;
    const float hp = me.health(), mx = me.max_health();
    const float inc = incoming_at(pos);
    bool want = hp < mx * 0.25f;
    // Gold to spend and nothing engaged: go shopping.
    if (!want && me.gold_count() >= 150 && inc == 0 && hd(pos) > 6 && hd(pos) < 40 && me.gem_count() == 0) want = true;
    // Hurt and not in the middle of a camp: heal before the next target.
    if (!want && inc == 0 && hp < mx * 0.65f) {
        bool engaged = false;
        if (target >= 0) {
            const auto& g = groups[target];
            if (g.state == 2) engaged = true;
            for (std::size_t i = 0; i < g.tiles.size(); ++i)
                if (g.state == 0 && g.hp[i] > 0 && g.hp[i] < g.maxhp[i] && g.hp[i] < hp * 2 && cheb(pos, g.tiles[i]) <= 6) engaged = true;
        }
        if (!engaged) want = true;
    }
    // Too weak to start the planned target again.
    if (!want && target >= 0 && inc == 0) {
        const auto& g = groups[target];
        float s = 0;
        for (std::size_t i = 0; i < g.tiles.size(); ++i) if (g.hp[i] > 0) s += g.str[i];
        if (s > 0 && hp < mx * 0.55f && hp < s * 8) want = true;
    }
    if (!want) return false;
    if (inc > 0) {
        step_away_from_camps();
        return true;
    }
    if (hd(pos) <= 5) {
        step_towards(cambc::map::spawn_region(team::ally));
        return true;
    }
    try {
        if (fresh()) { me.recall(); return true; }
    } catch (const std::exception&) {
    }
    return false;
}

int Bot::choose_target() {
    auto& me = cambc::self();
    const int now = static_cast<int>(turn_round);
    // Walking distances from here, camps treated as passable (we stand next to them).
    distance_field f;
    bitboard visited;
    bitboard roots;
    roots.set(pos);
    bitboard w = static_block;
    w.set(pos, false);
    f.compute(w, roots, visited);
    f.export_linear_buffer(std::span{field_buf});
    const float dps = my_dps() * (bought >= 20 ? 1.1f : 1.0f);
    const float mx = me.max_health();
    const float reduce = bought >= 5 ? 0.7f : 1.0f;
    const float gold_w = std::max(0.12f, 0.35f * (1.0f - now / 1500.0f));

    std::array<std::uint8_t, 5> claims{};
    try { cambc::shared_memory.read(0, std::span{claims}); } catch (const std::exception&) {}

    int best = -1;
    float best_score = 0;
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& g = groups[gi];
        int d = UNREACHED;
        for (auto t : g.tiles) {
            d = std::min<int>(d, field_buf[t.y * W + t.x]);
            if (d < 4) break;
        }
        if (d == UNREACHED) continue;
        float rem = 0, inc = 0;
        for (std::size_t i = 0; i < g.tiles.size(); ++i)
            if (g.state == 0 && g.hp[i] > 0) {
                rem += g.hp[i];
                inc += g.str[i];
            } else if (g.state == 3) {
                rem += g.maxhp[i];
                inc += g.str[i];
            }
        inc *= reduce;
        float clear = g.state == 2 ? 0 : rem / dps;
        float extra = 0;
        if (inc > 0) {
            const float tank = mx * 0.7f / inc;
            if (tank < 6) continue; // monoliths and the like are not for a banker
            const float trips = std::max(1.0f, std::ceil(clear / tank));
            extra = (trips - 1) * (2.0f * std::min(g.home, 60) + 15);
        }
        float wait = 0;
        if (g.state == 3) wait = std::max(0, g.respawn_at - now - d);
        if (wait > 40) continue;
        wait *= 2.5f;
        float value = g.gems + gold_w * g.gold;
        if (value <= 0) continue;
        float factor = 1.0f;
        if (g.home > g.enemy_home) factor *= 0.65f;
        if (g.owner >= 0 && g.owner != my_index) factor *= 0.6f;
        bool engaged = false;
        if (static_cast<int>(gi) == target && g.state != 3) {
            engaged = g.state == 2 || d <= 3;
            for (std::size_t i = 0; i < g.hp.size(); ++i) if (g.hp[i] > 0 && g.hp[i] < g.maxhp[i]) engaged = true;
            factor *= engaged ? 2.0f : 1.3f;
        }
        if (g.state == 2) factor *= 1.3f;
        if (!engaged) {
            for (int a = 0; a < 5; ++a)
                if (a < my_index && claims[a] == gi + 1 && clear < 15) factor *= 0.3f;
            if (now - last_enemy_round < 30 && last_enemy.x >= 0 && cheb(last_enemy, g.tiles[0]) <= 8) factor *= 0.5f;
        }
        const float score = value * factor / (d + clear + 5 + extra + wait + 5);
        if (score > best_score) { best_score = score; best = static_cast<int>(gi); }
    }
    return best;
}

// Hit a camp of the group (or any awake camp) in range. Returns true if we attacked.
bool Bot::attack_camps(int gi) {
    auto& me = cambc::self();
    if (!me.can_attack() && !me.can_cast()) return false;
    const int r = me.attack_range();
    // Best camp in range: damage dealers first, then lowest health.
    int best_g = -1, best_i = -1;
    float best_s = -1e9f;
    for (std::size_t g2 = 0; g2 < groups.size(); ++g2) {
        const auto& g = groups[g2];
        if (g.state != 0) continue;
        const bool mine = static_cast<int>(g2) == gi;
        if (!mine && gi == -2) {
            float gs = 0;
            for (std::size_t i = 0; i < g.tiles.size(); ++i) if (g.hp[i] > 0) gs += g.str[i];
            if (gs > 60) continue;
        }
        for (std::size_t i = 0; i < g.tiles.size(); ++i) {
            if (g.hp[i] <= 0) continue;
            const int d = cheb(g.tiles[i], pos);
            if (d > std::max(r, 4)) continue;
            float s = (mine ? 1000.0f : 0.0f) + g.str[i] * 2 - g.hp[i] * 0.01f;
            if (d > r) s -= 5000;
            if (!mine && g.seen < static_cast<int>(turn_round)) continue;
            if (s > best_s) { best_s = s; best_g = static_cast<int>(g2); best_i = static_cast<int>(i); }
        }
    }
    if (best_g < 0) return false;
    // Only wake a different group when it is awake already (it hits us anyway).
    const vec2 t = groups[best_g].tiles[best_i];
    if (best_g != gi && gi != -2) {
        bool awake = false;
        try { awake = cambc::vision::awake_camp_tiles().is_set(t); } catch (const std::exception&) {}
        if (!awake) return false;
    }
    try {
        if (!visible.is_set(t) || !cambc::vision::has_camp(t)) return false;
        const auto c = cambc::vision::camp_at(t);
        bool hit = false;
        if (cast_on_camp(c)) hit = true;
        if (cheb(t, pos) <= r && me.can_attack() && fresh() && c.is_visible() && c.health() > 0) {
            me.attack(c);
            attacked = true;
            hit = true;
            if (c.is_visible() && c.health() > 0) after_hit(c);
        }
        return hit;
    } catch (const std::exception&) {
    }
    return false;
}

void Bot::farm() {
    auto& me = cambc::self();
    const int prev = target;
    target = choose_target();
    if (target != prev) {
        target_radius = me.attack_range(); stuck_turns = 0;
    }
    try {
        std::array<std::uint8_t, 1> c{static_cast<std::uint8_t>(target + 1)};
        cambc::shared_memory.write(my_index, std::span{c});
    } catch (const std::exception&) {
    }

    // Shield up while camps are hitting us.
    if (shield_slot >= 0 && incoming_at(pos) > 0) {
        try {
            auto abilities = me.abilities();
            if (abilities[shield_slot].is_ready() && fresh()) abilities[shield_slot].use();
        } catch (const std::exception&) {
        }
    }

    if (target < 0) {
        attack_camps(-1);
        return;
    }
    auto& g = groups[target];

    if (g.state == 2) {
        ++modes[7];
        // Capture: stand on one of the group's tiles.
        bitboard goals;
        for (auto t : g.tiles) goals.set(t);
        if (goals.is_set(pos)) {
            // Standing still for the capture: work on a neighbouring camp meanwhile.
            if (me.health() > me.max_health() * 0.6f) attack_camps(-2);
            return;
        }
        try {
            goals &= ~(cambc::vision::awake_camp_tiles() | cambc::vision::asleep_camp_tiles()
                | cambc::vision::champion_tiles());
        } catch (const std::exception&) {
        }
        step_towards(goals, true);
        return;
    }
    if (g.state == 3) {
        ++modes[8];
        // Wait near the group for its respawn, off its spawn tiles (standing on one delays it).
        bitboard spawn_tiles, ring;
        for (auto t : g.tiles) { spawn_tiles.set(t); box(ring, t, 2); }
        ring &= ~spawn_tiles;
        if (ring.is_set(pos)) return;
        if (spawn_tiles.is_set(pos) || cheb(pos, g.tiles[0]) <= 6) {
            step_towards(ring);
            return;
        }
        step_towards(ring);
        return;
    }

    // Retreat between trips when the next few hits would kill us.
    const float inc = incoming_at(pos);
    const float hp = me.health();
    const float margin = 80.0f + me.gem_count() * 3.0f;
    int exit_steps = 0;
    for (const auto& t : threats) exit_steps = std::max(exit_steps, t.range + 1 - cheb(pos, t.pos));
    if (inc > 0 && hp < inc * (exit_steps + 2.0f) + margin) {
        ++modes[11];
        step_away_from_camps();
        return;
    }

    if (attack_camps(target)) {
        ++modes[9];
        stuck_turns = 0;
        return;
    }
    ++modes[10];

    // Approach the living camps of the group.
    bitboard goals;
    bool any = false;
    for (std::size_t i = 0; i < g.tiles.size(); ++i)
        if (g.hp[i] > 0) { box(goals, g.tiles[i], target_radius); any = true; }
    if (!any) {
        // Everything we know of is dead but the group did not report a reward yet: go look.
        for (auto t : g.tiles) box(goals, t, 1);
    }
    if (goals.is_set(pos)) {
        // In position but nothing to hit (no line of sight): come closer.
        if (++stuck_turns >= 2 && target_radius > 1) { --target_radius; stuck_turns = 0; }
        goals.clear();
        for (std::size_t i = 0; i < g.tiles.size(); ++i)
            if (g.hp[i] > 0) box(goals, g.tiles[i], target_radius);
    }
    step_towards(goals);
    attack_camps(target);
}

void Bot::play_turn() {
    turn_round = cambc::round_number();
    auto& me = cambc::self();
    if (!inited) init();
    moved = move_failed = attacked = false;

    if (!me.is_alive()) {
        ++modes[0];
        buy_items();
        return;
    }
    if (me.is_recalling()) { ++modes[1]; return; }
    if (me.is_submitting_gems() || me.is_teleporting()) { ++modes[2]; return; }
    pos = me.position();
    if (pos.x < 0) return;
    observe();
    if (false) {
        std::cout << "LOW r" << turn_round << " #" << my_index << " hp " << me.health() << " inc " << incoming_at(pos)
            << " foes " << foes.size() << " gems " << me.gem_count() << " tgt " << target << " pos " << pos.x << "," << pos.y;
        for (const auto& f : foes) std::cout << " foe@" << cheb(f.pos, pos);
        std::cout << "\n";
    }
    walls = static_block | cambc::vision::champion_tiles() | cambc::vision::camp_tiles()
        | cambc::vision::teleport_tiles();
    walls.set(pos, false);
    danger.clear();
    any_danger = false;
    for (const auto& t : threats)
        if (t.str >= 100) { box(danger, t.pos, t.range); any_danger = true; }

    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(pos);
    if (at_spawn) buy_items();
    if (!fresh()) return;
    if (handle_enemies()) { ++modes[3]; dbg("enemies"); return; }

    // Heal fully on spawn before heading out.
    if (at_spawn && me.health() < me.max_health() * 0.92f && heal_wait < 15) {
        ++heal_wait;
        ++modes[4];
        return;
    }
    if (!at_spawn) heal_wait = 0;

    if (go_tribute()) { ++modes[5]; dbg("tribute"); return; }
    if (maybe_recall()) { ++modes[6]; dbg("recall"); return; }
    farm();
    dbg("farm");
}

void Bot::mark(const char* what) {
    if (!kDebug || turn_round % 100 != 0) return;
    std::cout << "    " << what << " " << cambc::fuel_remaining() << " r" << cambc::round_number() << "\n";
}

void Bot::dbg(const char* what) {
    if (!kDebug || turn_round % 100 != 0) return;
    auto& me = cambc::self();
    std::cout << "r" << turn_round << " #" << my_index << " " << what << " pos " << pos.x << "," << pos.y
        << " hp " << me.health() << "/" << me.max_health() << " gems " << me.gem_count() << " gold " << me.gold_count()
        << " tgt " << target;
    if (target >= 0) {
        const auto& g = groups[target];
        std::cout << " st " << g.state << " at " << g.tiles[0].x << "," << g.tiles[0].y << " hp";
        for (auto h : g.hp) std::cout << " " << h;
    }
    std::cout << " inc " << incoming_at(pos) << " fuel " << cambc::fuel_remaining() << "\n";
}
