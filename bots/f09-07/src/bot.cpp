// Split farm: every champion clears its own region alone, tributes at its home altar, and only
// regroups when an ally reports an invader. Lone champions retreat and recall instead of fighting packs.
#include <cambc.h>
#include "bot.h"
#include "plan.h"
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

int cheb(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }


// Chebyshev box with our own bounds handling (draw_chebyshev has thrown near edges).
void add_box(cambc::bitboard& board, vec2 c, int r, int w, int h) {
    try {
        board.draw_chebyshev(c, static_cast<float>(r));
        return;
    } catch (const cambc::error&) {}
    for (int y = std::max(0, c.y - r); y <= std::min(h - 1, c.y + r); ++y)
        for (int x = std::max(0, c.x - r); x <= std::min(w - 1, c.x + r); ++x) board.set({x, y});
}

cambc::bitboard empty_board() {
    cambc::bitboard b;
    b.clear();
    return b;
}

constexpr float kRetreatFrac = 0.22f;

} // namespace

bool Bot::on_our_spawn(vec2 p) const { return in_bounds(p) && our_spawn.get(idx(p)); }

bool Bot::free_tile(vec2 p) const {
    if (!in_bounds(p)) return false;
    const int i = idx(p);
    return !blocked_static.get(i) && !occ.get(i);
}

float Bot::camp_mult() const {
    try {
        const auto counts = cambc::self().item_count();
        float m = 1.0f;
        if (counts[cambc::item_type::jungle_emblem_of_strength] || counts[cambc::item_type::jungle_emblem_of_health]
            || counts[cambc::item_type::jungle_emblem_of_focus])
            m *= 0.7f;
        return m;
    } catch (const cambc::error&) { return 1.0f; }
}

float Bot::my_dps() const {
    auto& me = cambc::self();
    float s = me.strength();
    float d = s;
    for (const auto& a : abilities) {
        if (a.kind == cambc::ability_kind::attack_reset) d += s / 9.0f;
        else if ((a.kind == cambc::ability_kind::damage || a.kind == cambc::ability_kind::cc
                     || a.kind == cambc::ability_kind::aoe_damage) && a.camps)
            d += me.focus() * a.modifier / 9.0f;
    }
    return std::max(10.0f, d);
}

void Bot::init() {
    ready = true;
    auto& me = cambc::self();
    width = cambc::map::width();
    height = cambc::map::height();
    const int n = width * height;
    const auto static_walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    blocked_static.load(static_walls);
    our_spawn.load(cambc::map::spawn_region(team::ally));
    camp_at_tile.assign(n, -1);
    altars = cambc::map::altars().positions();

    // Draft plan.
    const auto* mem = reinterpret_cast<const std::uint8_t*>(cambc::draft_memory.data());
    const bool have_plan = cambc::draft_memory.size() >= 1024 && mem[plan::kMagic0] == 'S' && mem[plan::kMagic1] == 'F';
    if (have_plan) {
        slots = std::clamp<int>(mem[plan::kSlots], 1, 5);
        const int mine = static_cast<int>(me.champion_name());
        for (int s = 0; s < 5; ++s)
            if (mem[plan::kTypes + s] == mine) slot = s;
        home_altar = {mem[plan::kAltars + 2 * slot], mem[plan::kAltars + 2 * slot + 1]};
        dash_map = mem[plan::kFlags] & 1;
        if (!in_bounds(home_altar)) home_altar = {-1, -1};
    }

    raw = cambc::map::camp_groups();
    groups.reserve(raw.size());
    std::size_t total_camps = 0;
    for (const auto& rg : raw) total_camps += rg.camps().size();
    camps.reserve(total_camps);
    for (std::size_t gi = 0; gi < raw.size(); ++gi) {
        GroupMem g;
        g.gems = static_cast<int>(raw[gi].gem_value());
        g.gold = static_cast<int>(raw[gi].gold_value());
        g.respawn = raw[gi].max_respawn_timer();
        g.owner = have_plan && gi < plan::kMaxGroups ? mem[plan::kOwners + gi] : 0;
        double cx = 0, cy = 0;
        for (const auto& c : raw[gi].camps()) {
            CampMem cm;
            cm.pos = c.spawn_position();
            // Design stats by kind (patched values); live values replace them once seen.
            switch (c.kind()) {
            case cambc::camp_type::wolf: cm.max_hp = 1350; cm.str = 20; cm.range = 3; break;
            case cambc::camp_type::elk: cm.max_hp = 2400; cm.str = 60; cm.range = 4; break;
            case cambc::camp_type::gem_cluster: cm.max_hp = 120; cm.str = 0; cm.range = 0; break;
            case cambc::camp_type::gold_cluster: cm.max_hp = 120; cm.str = 0; cm.range = 0; break;
            case cambc::camp_type::monolith: cm.max_hp = 20000; cm.str = 200; cm.range = 5; break;
            default: cm.max_hp = 5000; cm.str = 120; cm.range = 4; break;
            }
            cm.hp = cm.max_hp;
            if (in_bounds(cm.pos)) camp_at_tile[idx(cm.pos)] = static_cast<int>(camps.size());
            g.camps.push_back(static_cast<int>(camps.size()));
            g.tiles.push_back(cm.pos);
            cx += cm.pos.x; cy += cm.pos.y;
            camps.push_back(cm);
        }
        if (!g.tiles.empty()) g.center = {static_cast<int>(std::lround(cx / g.tiles.size())),
                                          static_cast<int>(std::lround(cy / g.tiles.size()))};
        groups.push_back(std::move(g));
    }

    for (int s = 0; s < 2; ++s) {
        Ability a;
        try {
            const auto abs = me.abilities();
            const auto& ab = abs[s];
            const auto& info = ab.info();
            if (!info) continue;
            a.slot = s;
            a.kind = std::visit([](const auto& v) { return std::decay_t<decltype(v)>::kind; }, *info);
            a.exclusive = std::visit([](const auto& v) { return v.exclusive_with_movement; }, *info);
            a.range = std::visit([](const auto& v) -> float {
                if constexpr (requires { v.range; }) return v.range; else return 0.0f; }, *info);
            a.modifier = std::visit([](const auto& v) -> float {
                if constexpr (requires { v.modifier; }) return v.modifier; else return 0.0f; }, *info);
            a.camps = ab.can_target_camps();
            a.enemies = ab.can_target_enemies();
            if (a.kind == cambc::ability_kind::dash && dash_map) has_dash = true;
            abilities.push_back(a);
        } catch (const std::exception&) {}
    }
}

void Bot::init_fields() {
    auto bfs_walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    home_field.compute(bfs_walls | cambc::map::spawn_region(team::enemy), cambc::map::spawn_region(team::ally), home_seen);
    if (has_dash) {
        cambc::distance_field jump;
        cambc::bitboard jump_seen;
        jump.compute(cambc::map::walls() | cambc::map::altars() | cambc::map::spawn_region(team::enemy),
            cambc::map::spawn_region(team::ally), jump_seen);
        for (auto& g : groups)
            for (auto t : g.tiles)
                if (jump_seen.is_set(t)) g.home_dist = std::min<int>(g.home_dist, jump.get(t));
    } else {
        for (auto& g : groups)
            for (auto t : g.tiles)
                if (home_seen.is_set(t)) g.home_dist = std::min<int>(g.home_dist, home_field.get(t));
    }

}

void Bot::scan() {
    auto& me = cambc::self();
    vis.load(cambc::vision::visible_tiles());
    const auto champs = cambc::vision::champion_tiles();
    const auto camp_board = cambc::vision::camp_tiles();
    occ.load(champs | camp_board | cambc::vision::teleport_tiles());
    occ.clear(idx(pos));
    ranged = me.attack_range() > 1;

    enemies.clear();
    allies.clear();
    for (auto p : champs.positions()) {
        if (p.x == pos.x && p.y == pos.y) continue;
        try {
            auto c = cambc::vision::champion_at(p);
            Foe f;
            f.pos = p;
            f.hp = c.health();
            f.max_hp = c.max_health();
            f.str = c.strength();
            f.range = c.attack_range();
            f.channel = c.is_recalling() || c.is_submitting_gems();
            f.power = f.hp * (f.str + 0.25f * c.focus());
            if (c.is_enemy()) enemies.push_back(f); else allies.push_back(f);
        } catch (const std::exception&) {}
    }

    incoming = 0;
    visible_camps.clear();
    Bits camp_seen;
    camp_seen.load(camp_board);
    for (auto p : camp_board.positions()) {
        try {
            auto c = cambc::vision::camp_at(p);
            const auto sp = c.spawn_position();
            if (!in_bounds(sp)) continue;
            const int ci = camp_at_tile[idx(sp)];
            if (ci < 0) continue;
            auto& cm = camps[ci];
            cm.hp = c.health();
            cm.max_hp = c.max_health();
            cm.str = c.strength();
            cm.range = c.attack_range();
            cm.pos = p;
            if (cm.hp <= 0) continue;
            visible_camps.push_back(ci);
            if (c.is_awake() && cheb(p, pos) <= cm.range) incoming += cm.str;
        } catch (const std::exception&) {}
    }
    // Spawn tiles in view without a camp: that camp is dead.
    for (auto& cm : camps) {
        if (cm.hp <= 0 || !in_bounds(cm.pos)) continue;
        const int i = idx(cm.pos);
        if (vis.get(i) && !camp_seen.get(i)) cm.hp = 0;
    }
    mult_now = camp_mult();
    dps_now = my_dps();
    incoming *= mult_now;
}

void Bot::update_groups() {

    for (std::size_t gi = 0; gi < groups.size() && gi < raw.size(); ++gi) {
        auto& g = groups[gi];
        bool visible = false;
        for (auto t : g.tiles) if (in_bounds(t) && vis.get(idx(t))) { visible = true; break; }
        if (!visible) {
            if (g.state == 2 && round >= g.respawn_at) {
                g.state = 0;
                for (int ci : g.camps) camps[ci].hp = camps[ci].max_hp;
            } else if (g.state == 1 && round - g.seen > 25) {
                // Someone else probably took it; assume it respawns on its timer.
                g.state = 2;
                g.respawn_at = round + g.respawn / 2;
            }
            continue;
        }
        g.seen = round;
        try {
            const auto st = raw[gi].state();
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) {
                bool any = false;
                for (int ci : g.camps) if (camps[ci].hp > 0) any = true;
                if (g.state == 2 || !any) {
                    for (int ci : g.camps) camps[ci].hp = std::max(camps[ci].hp, camps[ci].max_hp);
                    g.trips = 0;
                }
                g.state = 0;
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                g.state = 1;
                for (int ci : g.camps) camps[ci].hp = 0;
            } else if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&st)) {
                if (g.state != 2) g.trips = 0;
                g.state = 2;
                g.respawn_at = round + std::max(1, r->timer);
                for (int ci : g.camps) camps[ci].hp = 0;
            }
        } catch (const std::exception&) {}
    }
}

void Bot::read_report(int& rx, int& ry, int& count, int& age) {
    std::array<std::uint8_t, 4> b{};
    cambc::shared_memory.read(0, std::span{b});
    rx = b[0]; ry = b[1]; count = b[2];
    age = (round - b[3]) & 0xFF;
    if (count == 0) age = 999;
}

void Bot::write_report() {
    if (enemies.empty()) return;
    // Report invaders on our half only.
    int best = -1;
    for (int i = 0; i < static_cast<int>(enemies.size()); ++i) {
        const auto p = enemies[i].pos;
        if (!home_seen.is_set(p)) continue;
        const int dh = home_field.get(p);
        const int de = enemy_field_ready && enemy_seen.is_set(p) ? enemy_field.get(p) : 999;
        if (dh <= de + 2 && (best < 0 || cheb(p, pos) < cheb(enemies[best].pos, pos))) best = i;
    }
    if (best < 0) return;
    std::array<std::uint8_t, 4> b{static_cast<std::uint8_t>(enemies[best].pos.x),
        static_cast<std::uint8_t>(enemies[best].pos.y), static_cast<std::uint8_t>(enemies.size()),
        static_cast<std::uint8_t>(round & 0xFF)};
    cambc::shared_memory.write(0, std::span{b});
}

void Bot::claim(int group) {
    if (slot >= 4) return;
    std::array<std::uint8_t, 1> b{static_cast<std::uint8_t>(group >= 0 && group < 255 ? group + 1 : 0)};
    cambc::shared_memory.write(4 + slot, std::span{b});
}

void Bot::buy_items() {
    auto& me = cambc::self();
    const bool vesper = me.champion_name() == cambc::champion_type::vesper_vale;
    const bool tanky = me.champion_name() == cambc::champion_type::varg;
    for (int guard = 0; guard < 40; ++guard) {
        cambc::item_type item = cambc::item_type::power_shard_of_strength;
        int step = build_step;
        if (vesper) {
            if (step < 5) item = cambc::item_type::treasure_shard_of_strength;
            step = step >= 5 ? step - 5 : -1;
        }
        if (step >= 0) {
            if (step < 5) item = cambc::item_type::jungle_shard_of_strength;
            else if (step < 10) item = tanky ? cambc::item_type::durability_shard_of_health : cambc::item_type::monster_shard_of_strength;
            else if (step < 15) item = tanky ? cambc::item_type::monster_shard_of_strength : cambc::item_type::power_shard_of_health;
            else item = (step & 1) ? cambc::item_type::power_shard_of_health : cambc::item_type::power_shard_of_strength;
        }
        if (me.gold_count() < item.cost() || !me.can_buy(item, 1)) return;
        me.buy(item, 1);
        ++build_step;
    }
}

float Bot::danger_at(vec2 p) const {
    float d = 0;
    for (const auto& e : enemies) {
        const int dist = cheb(p, e.pos);
        if (dist <= e.range + 1) d += e.str;
    }
    return d;
}

bool Bot::step_with(const cambc::distance_field& field, const cambc::bitboard& seen, bool away) {
    auto& me = cambc::self();
    if (moved || move_failed || !me.can_move() || cast_exclusive) return false;
    if (!seen.is_set(pos)) return false;
    const int here = field.get(pos);
    vec2 best = pos;
    double best_score = 1e18;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            const vec2 q{pos.x + dx, pos.y + dy};
            if (!free_tile(q) || !seen.is_set(q)) continue;
            const int d = field.get(q);
            if (d > here) continue;
            double s = (d - here) * 10.0;
            if (d == here) s += 6;  // sideways only when the straight steps are blocked
            if (away) {
                int mind = 99;
                for (const auto& e : enemies) mind = std::min(mind, cheb(q, e.pos));
                s -= std::min(mind, 6) * 4.0;
            }
            if (s < best_score) { best_score = s; best = q; }
        }
    if (best.x == pos.x && best.y == pos.y) return false;
    if (cambc::vision::has_champion(best) || cambc::vision::has_camp(best)) return false;
    try {
        if (me.move_absolute(best)) {
            moved = true;
            pos = best;
            return true;
        }
    } catch (const cambc::error&) {}
    move_failed = true;
    return false;
}

bool Bot::try_dash(const cambc::distance_field& field, const cambc::bitboard& seen) {
    auto& me = cambc::self();
    if (cast || moved || !seen.is_set(pos)) return false;
    const int here = field.get(pos);
    if (here < 4) return false;
    for (const auto& a : abilities) {
        if (a.kind != cambc::ability_kind::dash) continue;
        try {
            const auto abs = me.abilities();
            if (abs[a.slot].cooldown() != 0 || !me.can_cast()) return false;
            const auto d1 = field.next_move_from(pos);
            if (!d1) return false;
            const vec2 p1{pos.x + d1->x, pos.y + d1->y};
            const auto d2 = field.next_move_from(p1);
            if (!d2) return false;
            const vec2 p2{p1.x + d2->x, p1.y + d2->y};
            if (!free_tile(p2) || !free_tile(p1) || field.get(p2) + 2 != here) return false;
            if (cambc::vision::has_champion(p2) || cambc::vision::has_camp(p2)) return false;
            if (abs[a.slot].use(p2)) {
                cast = true;
                pos = p2;
                return true;
            }
            cast = true;  // a failed cast still counts as our try this turn
        } catch (const std::exception&) { cast = true; }
        return false;
    }
    return false;
}

bool Bot::hole_jump(const cambc::distance_field& field, const cambc::bitboard& seen) {
    // The best step is a hole: dash over it to a solid tile closer to the goal, or wait for the dash.
    auto& me = cambc::self();
    const auto& holes = cambc::map::holes();
    const int here = field.get(pos);
    bool hole_next = false;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const vec2 q{pos.x + dx, pos.y + dy};
            if ((dx || dy) && in_bounds(q) && seen.is_set(q) && field.get(q) < here) {
                if (!holes.is_set(q) && free_tile(q)) return false;  // a normal step exists
                if (holes.is_set(q)) hole_next = true;
            }
        }
    if (!hole_next) return false;
    vec2 best = pos;
    int best_d = here;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const vec2 q{pos.x + dx, pos.y + dy};
            if (!in_bounds(q) || !seen.is_set(q) || !free_tile(q)) continue;
            const int d = field.get(q);
            if (d < best_d) { best_d = d; best = q; }
        }
    if (best.x == pos.x && best.y == pos.y) return true;  // nowhere to land: hold
    for (const auto& a : abilities) {
        if (a.kind != cambc::ability_kind::dash) continue;
        try {
            const auto abs = me.abilities();
            if (abs[a.slot].cooldown() != 0 || !me.can_cast() || cast) return true;  // wait for it
            if (cambc::vision::has_champion(best) || cambc::vision::has_camp(best)) return true;
            cast = true;
            if (abs[a.slot].use(best)) { pos = best; moved = true; }
        } catch (const std::exception&) {}
        return true;
    }
    return false;
}

bool Bot::route_to(const cambc::bitboard& goals_in) {
    auto& me = cambc::self();
    if (moved || move_failed || !me.can_move() || cast_exclusive) return false;
    if (cambc::fuel_remaining() < 30000 || cambc::round_number() != static_cast<std::uint32_t>(round)) return false;
    auto walls = cambc::map::walls() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy) | cambc::vision::champion_tiles() | cambc::vision::camp_tiles();
    if (!has_dash) walls |= cambc::map::holes();
    walls.set(pos, false);
    auto goals = goals_in & ~walls;
    if (has_dash) goals &= ~cambc::map::holes();
    if (goals.is_empty() || goals.is_set(pos)) return false;
    auto dest = empty_board();
    dest.set(pos);
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute_to(walls, goals, dest, visited);
    if (!visited.is_set(pos)) return false;
    if (has_dash && hole_jump(field, visited)) return true;
    if (try_dash(field, visited)) {
        if (goals.is_set(pos)) return true;
        // Fresh vision after the dash: refresh occupancy for the walk.
        occ.load(cambc::vision::champion_tiles() | cambc::vision::camp_tiles());
        occ.clear(idx(pos));
        step_with(field, visited, false);
        return true;
    }
    return step_with(field, visited, false);
}

bool Bot::flee_home() {
    auto& me = cambc::self();
    // Melee champions hit back before running.
    if (!ranged && me.can_attack()) attack_enemies();
    if (try_dash(home_field, home_seen)) {
        occ.load(cambc::vision::champion_tiles() | cambc::vision::camp_tiles());
        occ.clear(idx(pos));
    }
    step_with(home_field, home_seen, true);
    return true;
}

bool Bot::escape_camps() {
    auto& me = cambc::self();
    purpose = 3;
    if (incoming <= 0) {
        bool enemy_near = false;
        for (const auto& e : enemies) if (cheb(e.pos, pos) <= 7) enemy_near = true;
        if (!enemy_near && !on_our_spawn(pos)) {
            try { me.recall(); return true; } catch (const cambc::error&) {}
        }
        return false;
    }
    auto danger = empty_board();
    for (int ci : visible_camps) {
        const auto& cm = camps[ci];
        add_box(danger, cm.pos, cm.range, width, height);
    }
    auto safe = ~danger;
    // Melee: a last hit on the way out if something is in reach.
    if (!ranged && me.can_attack()) {
        for (int ci : visible_camps) {
            if (cheb(camps[ci].pos, pos) <= 1) {
                try { me.attack(cambc::vision::camp_at(camps[ci].pos)); } catch (const std::exception&) {}
                break;
            }
        }
    }
    return route_to(safe);
}

void Bot::spells_on_camp(const cambc::camp& c) {
    auto& me = cambc::self();
    if (cast) return;
    const int dist = cheb(c.position(), pos);
    const float chp = c.health();
    if (chp < me.strength() * 1.5f) return;  // basic attacks finish it
    for (const auto& a : abilities) {
        const bool harm = a.kind == cambc::ability_kind::damage || a.kind == cambc::ability_kind::cc
            || a.kind == cambc::ability_kind::aoe_damage;
        if (!harm || !a.camps || dist > a.range) continue;
        if (a.exclusive && moved) continue;
        try {
            const auto abs = me.abilities();
            if (abs[a.slot].cooldown() != 0 || !me.can_cast()) continue;
            bool ok = a.kind == cambc::ability_kind::aoe_damage ? abs[a.slot].use(c.position()) : abs[a.slot].use(c);
            cast = true;
            if (ok && a.exclusive) cast_exclusive = true;
            return;
        } catch (const std::exception&) { cast = true; return; }
    }
}

void Bot::spells_on_enemy() {
    auto& me = cambc::self();
    if (cast) return;
    // Weakest enemy within range of a harmful spell.
    for (const auto& a : abilities) {
        const bool harm = a.kind == cambc::ability_kind::damage || a.kind == cambc::ability_kind::cc
            || a.kind == cambc::ability_kind::aoe_damage;
        if (!harm || !a.enemies) continue;
        if (a.exclusive && moved) continue;
        int best = -1;
        for (int i = 0; i < static_cast<int>(enemies.size()); ++i)
            if (cheb(enemies[i].pos, pos) <= a.range && (best < 0 || enemies[i].hp < enemies[best].hp)) best = i;
        if (best < 0) continue;
        try {
            const auto abs = me.abilities();
            if (abs[a.slot].cooldown() != 0 || !me.can_cast()) continue;
            auto target = cambc::vision::champion_at(enemies[best].pos);
            bool ok = a.kind == cambc::ability_kind::aoe_damage ? abs[a.slot].use(enemies[best].pos) : abs[a.slot].use(target);
            cast = true;
            if (ok && a.exclusive) cast_exclusive = true;
            return;
        } catch (const std::exception&) { cast = true; return; }
    }
}

void Bot::shield_if_hit() {
    auto& me = cambc::self();
    if (cast) return;
    bool threat = incoming > 0;
    for (const auto& e : enemies) if (cheb(e.pos, pos) <= e.range + 1) threat = true;
    if (!threat) return;
    for (const auto& a : abilities) {
        if (a.kind != cambc::ability_kind::shield) continue;
        try {
            const auto abs = me.abilities();
            if (abs[a.slot].cooldown() != 0 || !me.can_cast()) return;
            abs[a.slot].use();
            cast = true;
        } catch (const std::exception&) { cast = true; }
        return;
    }
}

bool Bot::use_reset_and_hit() {
    auto& me = cambc::self();
    if (cast) return false;
    for (const auto& a : abilities) {
        if (a.kind != cambc::ability_kind::attack_reset) continue;
        try {
            const auto abs = me.abilities();
            if (abs[a.slot].cooldown() != 0 || !me.can_cast()) return false;
            abs[a.slot].use();
            cast = true;
            return true;
        } catch (const std::exception&) { cast = true; }
        return false;
    }
    return false;
}

bool Bot::attack_enemies() {
    auto& me = cambc::self();
    if (!me.can_attack()) return false;
    const int r = me.attack_range();
    int best = -1;
    for (int i = 0; i < static_cast<int>(enemies.size()); ++i)
        if (cheb(enemies[i].pos, pos) <= r && (best < 0 || enemies[i].hp < enemies[best].hp)) best = i;
    if (best < 0) return false;
    spells_on_enemy();
    try {
        auto t = cambc::vision::champion_at(enemies[best].pos);
        if (!t.is_visible() || t.health() <= 0) return false;
        me.attack(t);
        if (t.is_visible() && t.health() > 0 && use_reset_and_hit() && me.can_attack()) me.attack(t);
        return true;
    } catch (const std::exception&) {}
    return false;
}

bool Bot::fight_or_flight() {
    if (enemies.empty()) return false;
    purpose = 2;
    last_enemy_seen = round;
    auto& me = cambc::self();
    std::vector<int> near;
    const auto& their_spawn = cambc::map::spawn_region(team::enemy);
    for (int i = 0; i < static_cast<int>(enemies.size()); ++i)
        if (cheb(enemies[i].pos, pos) <= 9 && !their_spawn.is_set(enemies[i].pos)) near.push_back(i);
    if (near.empty()) return false;

    // Camp fire on our tile counts against us.
    float mine = std::max(0.0f, hp - incoming * 3) * (me.strength() + 0.25f * me.focus());
    for (const auto& a : allies) if (cheb(a.pos, pos) <= 5) mine += a.power;
    float theirs = 0;
    int closest = near[0];
    for (int i : near) {
        theirs += enemies[i].channel ? enemies[i].power * 0.3f : enemies[i].power;
        if (cheb(enemies[i].pos, pos) < cheb(enemies[closest].pos, pos)) closest = i;
    }
    float need = 1.5f + me.gem_count() / 100.0f + (hp < 0.5f * max_hp ? 0.5f : 0.0f);
    if (on_our_spawn(pos)) need *= 0.6f;
    if (round < fleeing_until) need *= 1.6f;  // once running, keep running unless clearly ahead
    const bool strong = mine >= need * theirs && hp >= 0.35f * max_hp;
    debug_block({
        if (round < 400)
            std::cout << "FIGHT r" << round << " s" << slot << " hp " << static_cast<int>(hp) << " near " << near.size()
                      << " allies " << allies.size() << " ratio " << (theirs > 0 ? mine / theirs : 99) << " strong " << strong
                      << " cd " << cheb(enemies[closest].pos, pos) << " inc " << incoming << "\n";
    });

    // A kill in reach is always worth taking.
    for (int i : near)
        if (cheb(enemies[i].pos, pos) <= me.attack_range() && enemies[i].hp <= me.strength() * 1.5f) {
            attack_enemies();
            if (!ranged) step_with(home_field, home_seen, true);
            return true;
        }

    if (strong) {
        if (attack_enemies()) return true;
        // Chase the weakest nearby enemy.
        int prey = near[0];
        for (int i : near) if (enemies[i].hp < enemies[prey].hp) prey = i;
        if (cheb(enemies[prey].pos, pos) <= 5) {
            spells_on_enemy();
            auto goal = empty_board();
            add_box(goal, enemies[prey].pos, me.attack_range(), width, height);
            route_to(goal);
            if (!ranged) attack_enemies();
            return true;
        }
        return false;
    }
    // Weaker: run when they are close enough to engage, recall once clear.
    const int cd = cheb(enemies[closest].pos, pos);
    if (cd <= std::max(6, enemies[closest].range + 4) || (incoming > 0 && cd <= 9)) {
        fleeing_until = round + 8;
        spells_on_enemy();
        flee_home();
        return true;
    }
    if (cd >= 7 && incoming <= 0 && !on_our_spawn(pos)
        && (round < fleeing_until + 6 || me.gem_count() >= 10 || hp < 0.6f * max_hp)) {
        try { me.recall(); return true; } catch (const cambc::error&) {}
    }
    return false;
}

bool Bot::tribute(bool force) {
    auto& me = cambc::self();
    purpose = 1;
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0) return false;
    int nearest = 999;
    for (auto a : altars) nearest = std::min(nearest, cheb(a, pos));
    int rx, ry, rcount, rage;
    read_report(rx, ry, rcount, rage);
    const bool danger = round - last_enemy_seen < 80 || rage <= 20;
    const bool want = force || gems >= 25 || (gems >= 10 && nearest <= 10) || (gems >= 6 && danger)
        || round > 1850;
    if (!want) return false;
    for (const auto& e : enemies) if (cheb(e.pos, pos) <= 4 && !e.channel) return false;

    auto enemy_locked = [&](vec2 a) {
        try {
            if (!cambc::vision::has_altar_lock(a)) return false;
            return cambc::vision::altar_lock_at(a).is_enemy();
        } catch (const std::exception&) { return false; }
    };
    for (auto a : altars) {
        if (cheb(a, pos) <= cambc::constants::submit_gems_range && !enemy_locked(a)) {
            try { me.submit_gems(a); return true; } catch (const cambc::error&) {}
        }
    }
    auto goals = empty_board();
    bool any = false;
    const bool home_ok = in_bounds(home_altar) && !enemy_locked(home_altar)
        && cheb(home_altar, pos) <= nearest + 10;
    for (auto a : altars) {
        if (home_ok && !(a.x == home_altar.x && a.y == home_altar.y)) continue;
        if (enemy_locked(a)) continue;
        add_box(goals, a, cambc::constants::submit_gems_range, width, height);
        any = true;
    }
    if (!any) {
        // Every altar we know of is locked: tributing at one hits the lock.
        for (auto a : altars) {
            if (cheb(a, pos) <= cambc::constants::submit_gems_range) {
                try { me.submit_gems(a); return true; } catch (const cambc::error&) {}
            }
            add_box(goals, a, cambc::constants::submit_gems_range, width, height);
        }
    }
    return route_to(goals);
}

double Bot::group_score(int gi, double& trips_out, double floor) {
    trips_out = 0;
    const auto& g = groups[gi];
    if (g.tiles.empty() || g.home_dist >= 999) return 0;
    double factor = 1.0;
    const std::uint8_t own = g.owner;
    if (own == plan::kEnemySide) factor = round - last_enemy_seen < 150 ? 0.25 : 0.45;
    else if (own == plan::kNoOwner) factor = 0.5;
    else {
        const int s = own & 0x7F;
        const bool middle = own & plan::kMiddleBit;
        if (s == slot) factor = middle ? 0.8 : 1.0;
        else factor = middle ? 0.6 : 0.7;
    }
    if (gi == target) factor *= round - target_since > 8 ? 1.5 : 1.25;
    if (report_age <= 20 && report_count >= 2 && cheb(report_pos, g.center) <= 10) factor *= 0.2;
    if (g.state == 1 && (round - g.seen > 10 || cheb(pos, g.center) > 15) && gi != target) return 0;
    const double w = round < 600 ? 0.3 : round < 1400 ? 0.15 : 0.05;
    const double value = (g.gems + w * g.gold) * factor;
    if (value <= 0) return 0;
    const double travel = (travel_ok && g.travel >= 0 ? g.travel : cheb(pos, g.center) * 1.3) + 1;
    double wait = 0;
    if (g.state == 2) {
        wait = std::max(0.0, g.respawn_at - round - travel);
        if (wait > 30 && gi != target) wait *= 1.5;
    }
    // Far groups also cost the walk back to heal and bank.
    const double away = 0.4 * g.home_dist;
    // Cheap upper bound first: most groups lose before the clear simulation.
    if (value / (travel + wait + away + 8) <= floor) return 0;
    double clear = 0, dmg = 0;
    if (g.state != 1) {
        // Kill order: weakest first; damage taken = remaining strength x time.
        float hps[64], strs[64];
        int k = 0;
        float total_str = 0;
        for (int ci : g.camps) {
            if (k >= 64) break;
            const auto& c = camps[ci];
            const float h = g.state == 2 ? c.max_hp : c.hp;
            if (h <= 0) continue;
            int j = k++;
            while (j > 0 && hps[j - 1] > h) { hps[j] = hps[j - 1]; strs[j] = strs[j - 1]; --j; }
            hps[j] = h; strs[j] = c.str;
            total_str += c.str;
        }
        const double inv = 1.0 / dps_now;
        for (int i = 0; i < k; ++i) {
            const double dt = hps[i] * inv;
            clear += dt;
            dmg += total_str * dt;
            total_str -= strs[i];
        }
        dmg *= mult_now;
    }
    const double first = std::max(0.0, hp - 0.25 * max_hp);
    double trips = 0;
    if (dmg > first) trips = std::ceil((dmg - first) / (0.7 * max_hp));
    if (trips > (round - last_enemy_seen < 100 ? 1 : 3)) return 0;
    trips_out = trips;
    const double trip_cost = trips * (g.home_dist * 2.0 + 14);
    return value / (travel + wait + away + clear + trip_cost + 8);
}

int Bot::choose_target() {
    {
        int rx, ry;
        read_report(rx, ry, report_count, report_age);
        report_pos = {rx, ry};
    }
    // Walking distances from here to every group (one host search, static terrain).
    travel_ok = false;
    if (cambc::fuel_remaining() > 65000) {
        try {
            auto walls = cambc::map::walls() | cambc::map::altars() | cambc::map::spawn_region(team::enemy);
            if (!has_dash) walls |= cambc::map::holes();
            auto root = empty_board();
            root.set(pos);
            cambc::distance_field field;
            cambc::bitboard seen;
            field.compute(walls, root, seen);
            for (auto& g : groups) {
                g.travel = -1;
                if (g.tiles.empty()) continue;
                const auto t = g.tiles[0];
                if (seen.is_set(t)) g.travel = field.get(t);
                else g.travel = 9999;
            }
            travel_ok = true;
        } catch (const std::exception&) {}
    }
    std::array<std::uint8_t, 4> claims{};
    cambc::shared_memory.read(4, std::span{claims});
    int best = -1;
    double best_score = 0;
    // Score the current target first so its bound prunes the rest.
    auto consider = [&](int gi) {
        double trips;
        double s = group_score(gi, trips, best_score);
        if (s <= 0) return;
        for (int k = 0; k < 4; ++k)
            if (k != slot && claims[k] == gi + 1) s *= 0.3;
        if (s > best_score) { best_score = s; best = gi; }
    };
    if (target >= 0) consider(target);
    for (int gi = 0; gi < static_cast<int>(groups.size()); ++gi)
        if (gi != target) consider(gi);
    return best;
}

bool Bot::attack_camps(int gi) {
    auto& me = cambc::self();
    auto& g = groups[gi];
    // Retreat before the camps kill us; camps never regenerate, so the next trip finishes the job.
    float threat = incoming;
    for (const auto& e : enemies) if (cheb(e.pos, pos) <= e.range + 1) threat += e.str;
    if (threat > 0 && (hp < kRetreatFrac * max_hp || hp < threat * 3.0f + 0.08f * max_hp)) {
        retreating_until = round + 20;
        ++g.trips;
        escape_camps();
        return true;
    }
    // Camp in reach: hit the weakest.
    const int r = me.attack_range();
    int best = -1;
    for (int ci : visible_camps) {
        if (std::find(g.camps.begin(), g.camps.end(), ci) == g.camps.end()) continue;
        if (cheb(camps[ci].pos, pos) <= r && (best < 0 || camps[ci].hp < camps[best].hp)) best = ci;
    }
    // A fresh group with low health: heal first.
    bool awake = incoming > 0;
    if (!awake && hp < 0.5f * max_hp) {
        double trips;
        group_score(gi, trips, 0);
        if (trips > 0 || hp < 0.35f * max_hp) {
            bool enemy_near = false;
            for (const auto& e : enemies) if (cheb(e.pos, pos) <= 7) enemy_near = true;
            if (!enemy_near) {
                try { me.recall(); return true; } catch (const cambc::error&) {}
            }
        }
    }
    if (best >= 0 && me.can_attack()) {
        try {
            auto c = cambc::vision::camp_at(camps[best].pos);
            spells_on_camp(c);
            if (c.is_visible() && c.health() > 0) {
                me.attack(c);
                last_hit_round = round;
                if (c.is_visible() && c.health() > 0 && use_reset_and_hit() && me.can_attack()) me.attack(c);
            }
            return true;
        } catch (const std::exception&) {}
    }
    // Walk towards the nearest living camps of the group; fall back to the next one when no tile reaches.
    int cand[3] = {-1, -1, -1};
    for (int ci : g.camps) {
        if (camps[ci].hp <= 0) continue;
        const int d = cheb(camps[ci].pos, pos);
        for (int k = 0; k < 3; ++k) {
            if (cand[k] < 0 || d < cheb(camps[cand[k]].pos, pos)) {
                for (int m = 2; m > k; --m) cand[m] = cand[m - 1];
                cand[k] = ci;
                break;
            }
        }
    }
    if (cand[0] < 0) return false;
    // Ranged nukes reach a tile further than basic attacks.
    if (std::find(visible_camps.begin(), visible_camps.end(), cand[0]) != visible_camps.end()) {
        try { spells_on_camp(cambc::vision::camp_at(camps[cand[0]].pos)); } catch (const std::exception&) {}
    }
    // Ranged champions that stopped hitting (no line of sight) close in.
    int reach = r;
    if (ranged && round - last_hit_round > 5 && round - target_since > 5)
        reach = std::max(1, r - (round - last_hit_round > 12 ? 2 : 1));
    for (int k = 0; k < 3 && cand[k] >= 0; ++k) {
        const auto cpos = camps[cand[k]].pos;
        auto goal = empty_board();
        try {
            auto box = empty_board();
            add_box(box, cpos, reach, width, height);
            if (reach > 1) {
                auto sight = (cambc::map::walls() | cambc::map::bushes()).shadow_cast(cpos, static_cast<std::uint16_t>(reach));
                goal = sight & box;
            } else {
                goal = box;
            }
        } catch (const std::exception&) {
            add_box(goal, cpos, reach, width, height);
        }
        if (goal.is_set(pos)) break;  // already in place
        if (route_to(goal) || moved || move_failed) break;
    }
    // Melee can still swing after the step.
    if (!ranged && me.can_attack()) {
        for (int ci : g.camps) {
            if (camps[ci].hp > 0 && cheb(camps[ci].pos, pos) <= r) {
                try {
                    auto c = cambc::vision::camp_at(camps[ci].pos);
                    if (c.is_visible() && c.health() > 0) {
                        me.attack(c);
                        last_hit_round = round;
                    }
                } catch (const std::exception&) {}
                break;
            }
        }
    }
    return true;
}

bool Bot::farm() {
    auto& me = cambc::self();
    purpose = 0;
    if ((target < 0 || round - target_round >= 5) && cambc::fuel_remaining() > 45000) {
        const int t = choose_target();
        if (t != target) { claim(t); target_since = round; }
        target = t;
        target_round = round;
    }
    if (target < 0) {
        // Nothing worth doing: rest on spawn.
        if (!on_our_spawn(pos)) {
            if (try_dash(home_field, home_seen)) {}
            step_with(home_field, home_seen, false);
        }
        return false;
    }
    auto& g = groups[target];
    bool all_dead = true;
    for (int ci : g.camps) if (camps[ci].hp > 0) all_dead = false;
    if (g.state == 1 || (g.state == 0 && all_dead)) {
        // Capture: stand on one of its spawn tiles until the reward lands.
        purpose = 4;
        for (auto t : g.tiles) if (t.x == pos.x && t.y == pos.y) return true;
        auto goal = empty_board();
        for (auto t : g.tiles) goal.set(t);
        route_to(goal);
        return true;
    }
    if (g.state == 2) {
        // A long wait: heal on spawn meanwhile.
        if (g.respawn_at - round > g.home_dist + 10 && hp < 0.8f * max_hp && incoming <= 0 && enemies.empty()
            && !on_our_spawn(pos)) {
            try { me.recall(); return true; } catch (const cambc::error&) {}
        }
        // Wait near, never on, its spawn tiles.
        purpose = 5;
        if (cheb(pos, g.center) <= 3) {
            for (auto t : g.tiles)
                if (t.x == pos.x && t.y == pos.y) {
                    auto goal = empty_board();
                    add_box(goal, g.center, 4, width, height);
                    for (auto u : g.tiles) goal.set(u, false);
                    route_to(goal);
                }
            return true;
        }
        auto goal = empty_board();
        add_box(goal, g.center, 3, width, height);
        for (auto u : g.tiles) goal.set(u, false);
        route_to(goal);
        return true;
    }
    return attack_camps(target);
}

bool Bot::answer_report() {
    purpose = 7;
    int rx, ry, count, age;
    read_report(rx, ry, count, age);
    if (age > 3 || count > 2 || hp < 0.7f * max_hp || cambc::self().gem_count() > 20) return false;
    const vec2 r{rx, ry};
    if (!in_bounds(r) || cheb(r, pos) > 12 || cheb(r, pos) <= 3) return false;
    auto goal = empty_board();
    add_box(goal, r, 2, width, height);
    return route_to(goal);
}

void Bot::play_turn() {
    round = static_cast<int>(cambc::round_number());
    debug_block({
        if (round == 1000) {
            std::cout << "STATS s" << slot;
            for (int k = 0; k < 12; ++k) std::cout << " " << stat[k];
            std::cout << " | move";
            for (int k = 0; k < 8; ++k) std::cout << " " << mstat[k];
            std::cout << "\n";
        }
    });
    purpose = 6;
    play_inner();
    debug_block({
        auto& me2 = cambc::self();
        int k = 0;
        if (!me2.is_alive()) k = 1;
        else if (me2.is_recalling()) k = 2;
        else if (me2.is_submitting_gems()) k = 3;
        else if (round - last_hit_round == 0) k = 4;
        else if (moved) { k = 5; ++mstat[purpose]; }
        else if (on_our_spawn(pos)) k = 6;
        else k = 7;
        ++stat[k];
        if (k == 7 && target >= 0) ++stat[8 + std::min(groups[target].state, 2)];
    });
}

void Bot::play_inner() {
    if (!ready) {
        init();
        init_stage = 1;
        return;
    }
    auto& me = cambc::self();
    if (!me.is_alive()) {
        buy_items();
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;
    pos = me.position();
    hp = me.health();
    max_hp = std::max(1.0f, me.max_health());
    moved = move_failed = cast = cast_exclusive = false;

    // Staged start: the first turns build the static data and the distance fields.
    if (init_stage == 1) {
        init_stage = 2;
        init_fields();
        return;
    }
    if (init_stage == 2) {
        init_stage = 3;
        enemy_field_ready = true;
        auto bfs_walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
        enemy_field.compute(bfs_walls, cambc::map::spawn_region(team::enemy), enemy_seen);
        for (auto& g : groups)
            for (auto t : g.tiles)
                if (enemy_seen.is_set(t)) g.enemy_dist = std::min<int>(g.enemy_dist, enemy_field.get(t));
        return;
    }

    scan();
    update_groups();
    write_report();
    debug_block({
        if (round % 10 == 0 && round < 500)
            std::cout << "r" << round << " s" << slot << " pos " << pos.x << "," << pos.y << " hp " << static_cast<int>(hp)
                      << " tgt " << target << "/" << (target >= 0 ? groups[target].state : -1) << "/" << (target >= 0 ? groups[target].gems : 0) << " inc " << static_cast<int>(incoming) << " en " << enemies.size()
                      << " gems " << me.gem_count() << " fuel " << cambc::fuel_remaining() << "\n";
    });

    buy_items();
    if (on_our_spawn(pos)) {
        hp = me.health();
        max_hp = std::max(1.0f, me.max_health());
        bool threat = false;
        for (const auto& e : enemies) if (cheb(e.pos, pos) <= 4) threat = true;
        if (hp < 0.9f * max_hp && spawn_wait < 2 && !threat) {
            ++spawn_wait;
            return;
        }
    } else {
        spawn_wait = 0;
    }
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) return;

    shield_if_hit();
    if (fight_or_flight()) return;

    if (round > 1900 && tribute(true)) return;
    // Badly hurt: get out of camp range, then recall.
    if (incoming > 0 && (hp < kRetreatFrac * max_hp || hp < incoming * 2.5f + 0.05f * max_hp)) {
        escape_camps();
        return;
    }
    if (incoming <= 0 && hp < 0.35f * max_hp && !on_our_spawn(pos)) {
        if (me.gem_count() >= 10 && tribute(true)) return;
        if (escape_camps()) return;
    }
    if (tribute(false)) return;
    if (answer_report()) return;
    farm();
}
