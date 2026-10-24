// Champion decisions. main.cpp dispatches here after the draft.
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
constexpr int far_away = 9999;
constexpr bool trace = false;

constexpr std::array<vec2, 8> directions{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1}, vec2{0, -1}, vec2{-1, -1},
    vec2{-1, 0}, vec2{-1, 1}};

int cheb(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
bool in_bounds(vec2 p) { return cambc::map::is_in_bounds(p); }

// Add a Chebyshev square, clipped to the map (draw_chebyshev can fail at the edge).
void add_square(cambc::bitboard& board, vec2 c, int r) {
    const int w = cambc::map::width(), h = cambc::map::height();
    if (c.x - r >= 0 && c.y - r >= 0 && c.x + r < w && c.y + r < h) {
        board.draw_chebyshev(c, static_cast<float>(r));
        return;
    }
    for (int y = std::max(0, c.y - r); y <= std::min(h - 1, c.y + r); ++y)
        for (int x = std::max(0, c.x - r); x <= std::min(w - 1, c.x + r); ++x)
            board.set({x, y});
}

int camp_range_at(const std::vector<Group>& groups, vec2 p) {
    for (const auto& g : groups)
        for (std::size_t i = 0; i < g.facts.tiles.size(); ++i)
            if (g.facts.tiles[i] == p) {
                const auto kind = g.handle.camps()[i].kind();
                if (kind == cambc::camp_type::monolith) return 5;
                if (kind == cambc::camp_type::golem || kind == cambc::camp_type::elk) return 4;
                if (kind == cambc::camp_type::wolf) return 3;
                return 0;
            }
    return 4;
}

int group_of_tile(const std::vector<Group>& groups, vec2 p) {
    for (std::size_t i = 0; i < groups.size(); ++i)
        for (auto t : groups[i].facts.tiles)
            if (t == p) return static_cast<int>(i);
    return -1;
}

bool is_monster_tile(const std::vector<Group>& groups, vec2 p) {
    for (const auto& g : groups)
        for (std::size_t i = 0; i < g.facts.tiles.size(); ++i)
            if (g.facts.tiles[i] == p) {
                const auto kind = g.handle.camps()[i].kind();
                return kind != cambc::camp_type::gem_cluster && kind != cambc::camp_type::gold_cluster;
            }
    return false;
}

std::uint8_t read_shared(int offset) {
    if (offset < 0 || offset >= static_cast<int>(cambc::shared_memory.size())) return 0;
    return cambc::shared_memory[static_cast<std::size_t>(offset)];
}
} // namespace

int Field::at(vec2 p) const {
    if (!in_bounds(p) || !seen.is_set(p)) return far_away;
    return dist.get(p);
}

void Bot::init() {
    auto& me = cambc::self();
    plan = the_plan();
    walls = static_walls();
    const auto handles = cambc::map::camp_groups();
    groups.reserve(handles.size());
    const bool plan_ok = plan.ngroups == std::min<std::size_t>(handles.size(), max_groups);
    for (std::size_t i = 0; i < handles.size(); ++i) {
        Group g{handles[i], group_facts(handles[i])};
        for (auto t : g.facts.tiles) g.tiles.set(t);
        if (plan_ok && i < max_groups) {
            g.flags = plan.flags[i];
            g.zone = plan.zone[i];
        } else {
            g.flags = group_reachable;
        }
        groups.push_back(std::move(g));
    }

    // Altar approach tiles and home tiles as static fields.
    cambc::bitboard altar_roots;
    for (auto altar : cambc::map::altars().positions())
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const vec2 t{altar.x + dx, altar.y + dy};
                if (in_bounds(t) && !walls.is_set(t)) altar_roots.set(t);
            }
    altar_field.roots = altar_roots;
    altar_field.dist.compute(walls, altar_roots, altar_field.seen);
    home_field.roots = cambc::map::spawn_region(team::ally);
    home_field.dist.compute(walls, home_field.roots, home_field.seen);

    const auto team_list = cambc::champions(team::ally);
    bodies = static_cast<int>(team_list.size());
    for (std::size_t i = 0; i < team_list.size(); ++i)
        if (team_list[i] == me) slot = static_cast<int>(i);

    ranged = me.attack_range() > 1;
    tank = !ranged && me.max_health() >= 1000;
    ready = true;
    if (trace)
        std::cout << "init slot " << slot << "/" << bodies << " mode " << int(plan.mode) << " groups " << groups.size()
                  << " fuel " << cambc::fuel_remaining() << " heal " << cambc::constants::spawn_healing << " passive "
                  << cambc::constants::passive_gold << '\n';
}

const Field& Bot::group_field(int index) {
    auto& g = groups[static_cast<std::size_t>(index)];
    if (!g.field) {
        g.field = std::make_unique<Field>();
        g.field->roots = g.tiles;
        g.field->dist.compute(walls, g.tiles, g.field->seen);
    }
    return *g.field;
}

void Bot::update_groups() {
    const auto pos = cambc::self().position();
    const auto visible = cambc::vision::visible_tiles();
    const int round = static_cast<int>(cambc::round_number());
    for (auto& g : groups) {
        if (cheb(g.facts.center, pos) > 14) continue;
        if ((g.tiles & visible).is_empty()) continue;
        try {
            const auto state = g.handle.state();
            if (std::holds_alternative<cambc::map::camp_group_alive>(state)) {
                g.respawn_at = 0;
                g.reward = false;
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(state)) {
                g.reward = true;
                g.respawn_at = 0;
            } else if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&state)) {
                g.reward = false;
                g.respawn_at = round + r->timer;
            }
        } catch (const std::exception&) {
        }
    }
}

void Bot::observe_units() {
    enemies.clear();
    allies.clear();
    for (const auto side : {team::enemy, team::ally}) {
        for (auto p : cambc::vision::champion_tiles(side).positions()) {
            if (side == team::ally && p == cambc::self().position()) continue;
            try {
                const auto c = cambc::vision::champion_at(p);
                if (!c.is_visible()) continue;
                Seen s{p, c.health(), c.max_health(), c.strength()};
                (side == team::enemy ? enemies : allies).push_back(s);
            } catch (const std::exception&) {
            }
        }
    }
}

float Bot::my_dps() const {
    auto& me = cambc::self();
    float dps = me.strength();
    try {
        for (const auto& a : me.abilities()) {
            if (a.is_attack_reset()) dps += me.strength() / 8.0f;
            else if (a.is_damage() || a.is_aoe_damage() || a.is_cc()) dps += a.damage_amount() / 8.0f;
        }
    } catch (const std::exception&) {
    }
    return dps;
}

cambc::bitboard Bot::threat_tiles() const {
    cambc::bitboard threat;
    for (auto p : cambc::vision::awake_camp_tiles().positions()) {
        const int r = camp_range_at(groups, p);
        if (r > 0) add_square(threat, p, r);
    }
    for (const auto& e : enemies) add_square(threat, e.pos, 4);
    return threat;
}

bool Bot::walk(vec2 destination) {
    auto& me = cambc::self();
    bool ok = false;
    try {
        ok = me.move_absolute(destination);
    } catch (const std::exception& error) {
        if (trace)
            std::cout << "warn move r" << cambc::round_number() << " " << cambc::to_string(me.position()) << " -> "
                      << cambc::to_string(destination) << ": " << error.what() << '\n';
    }
    if (!ok) walked_fail = true;
    return ok;
}

bool Bot::step_along(const Field& field) {
    auto& me = cambc::self();
    if (walked_fail || cast_exclusive || !me.can_move()) return false;
    const auto pos = me.position();
    if (field.at(pos) == 0) return false;
    const int round = static_cast<int>(cambc::round_number());
    const auto camps = cambc::vision::camp_tiles();
    const auto blocked = walls | cambc::vision::champion_tiles() | camps | cambc::vision::teleport_tiles();

    // Pick the best free neighbour on a field; report whether a unit or camp blocks a better one.
    const auto pick = [&](const Field& f, bool& unit_blocked, bool& camp_blocked) {
        vec2 best = pos;
        int best_value = f.at(pos);
        for (auto d : directions) {
            const vec2 q = pos + d;
            if (!in_bounds(q)) continue;
            const int v = f.at(q);
            if (v >= best_value) continue;
            if (blocked.is_set(q)) {
                if (!walls.is_set(q)) {
                    unit_blocked = true;
                    if (camps.is_set(q)) camp_blocked = true;
                }
                continue;
            }
            if (avoid_active && avoid.is_set(q) && !avoid.is_set(pos)) continue;
            best = q;
            best_value = v;
        }
        return best;
    };

    bool unit_blocked = false, camp_blocked = false;
    const bool sticky = detour && detour_for == &field && round <= detour_until;
    vec2 best = pick(sticky ? *detour : field, unit_blocked, camp_blocked);
    if (best == pos && (unit_blocked || field.at(pos) >= far_away)) {
        // Route around what we can see. Camps do not move, so a detour around them is kept
        // until we arrive; a detour around champions only for a few turns.
        if (!detour) detour = std::make_unique<Field>();
        auto dyn_walls = (camp_blocked ? (walls | camps) : blocked) & ~field.roots;
        dyn_walls.set(pos, false);
        detour->roots = field.roots;
        detour->dist.compute(dyn_walls, field.roots, detour->seen);
        detour_for = &field;
        detour_until = round + (camp_blocked ? 60 : 3);
        bool ub = false, cb = false;
        best = pick(*detour, ub, cb);
    }
    if (best == pos) return false;
    return walk(best);
}

bool Bot::step_away(const cambc::bitboard& threat) {
    auto& me = cambc::self();
    if (walked_fail || cast_exclusive || !me.can_move()) return false;
    const auto pos = me.position();
    const auto blocked = walls | cambc::vision::champion_tiles() | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    vec2 best = pos;
    int best_cost = (threat.is_set(pos) ? 100000 : 0) + home_field.at(pos);
    for (auto d : directions) {
        const vec2 q = pos + d;
        if (!in_bounds(q) || blocked.is_set(q)) continue;
        const int cost = (threat.is_set(q) ? 100000 : 0) + home_field.at(q);
        if (cost < best_cost) { best = q; best_cost = cost; }
    }
    if (best == pos) return false;
    return walk(best);
}

bool Bot::step_off(const cambc::bitboard& avoid) {
    auto& me = cambc::self();
    if (walked_fail || cast_exclusive || !me.can_move()) return false;
    const auto pos = me.position();
    const auto blocked = walls | avoid | cambc::vision::champion_tiles() | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    for (auto d : directions) {
        const vec2 q = pos + d;
        if (!in_bounds(q) || blocked.is_set(q)) continue;
        return walk(q);
    }
    return false;
}

bool Bot::try_spells() {
    auto& me = cambc::self();
    const auto pos = me.position();
    const bool threatened = !enemies.empty() || !cambc::vision::awake_camp_tiles().is_empty();
    // Camp candidates: awake camps and members of our target group.
    auto camp_mask = cambc::vision::awake_camp_tiles();
    if (target >= 0) camp_mask |= cambc::vision::camp_tiles() & groups[static_cast<std::size_t>(target)].tiles;
    const auto camp_positions = camp_mask.positions();

    for (const auto& a : me.abilities()) {
        try {
            if (!a.is_ready()) continue;
            const auto& info = a.info();
            if (!info) continue;
            const bool exclusive = std::visit([](const auto& v) { return v.exclusive_with_movement; }, *info);
            if (a.is_damage() || a.is_cc()) {
                const float range = a.is_damage() ? a.get<cambc::ability_damage>().range : a.get<cambc::ability_cc>().range;
                const int r = static_cast<int>(range);
                const Seen* best = nullptr;
                for (const auto& e : enemies)
                    if (cheb(e.pos, pos) <= r && (!best || e.hp < best->hp)) best = &e;
                if (best) {
                    const auto c = cambc::vision::champion_at(best->pos);
                    if (c.is_visible() && a.use(c)) { cast_exclusive = exclusive; return true; }
                    continue;
                }
                if (!enemies.empty()) continue; // save burst for champions nearby
                vec2 chosen{-1, -1};
                float chosen_hp = 0;
                for (auto p : camp_positions) {
                    if (cheb(p, pos) > r || !is_monster_tile(groups, p) || !cambc::vision::has_camp(p)) continue;
                    const auto c = cambc::vision::camp_at(p);
                    const float hp = c.health();
                    if (chosen.x < 0 || hp > chosen_hp) { chosen = p; chosen_hp = hp; }
                }
                if (chosen.x >= 0 && chosen_hp > 150) {
                    const auto c = cambc::vision::camp_at(chosen);
                    if (a.use(c)) { cast_exclusive = exclusive; return true; }
                }
            } else if (a.is_aoe_damage()) {
                const int r = static_cast<int>(a.get<cambc::ability_aoe_damage>().range);
                std::vector<vec2> units;
                for (const auto& e : enemies) units.push_back(e.pos);
                for (auto p : camp_positions)
                    if (is_monster_tile(groups, p) || !enemies.empty()) units.push_back(p);
                vec2 best{-1, -1};
                int best_hits = 0;
                for (auto c : units) {
                    if (cheb(c, pos) > r) continue;
                    int hits = 0;
                    for (auto u : units) hits += cheb(u, c) <= 1 ? 1 : 0;
                    if (hits > best_hits) { best = c; best_hits = hits; }
                }
                if (best.x >= 0 && a.use(best)) { cast_exclusive = exclusive; return true; }
            } else if (a.is_heal()) {
                const int r = static_cast<int>(a.get<cambc::ability_heal>().range);
                const float amount = a.heal_amount();
                const Seen* best = nullptr;
                for (const auto& s : allies)
                    if (cheb(s.pos, pos) <= r && s.max_hp - s.hp >= amount * 0.6f && (!best || s.hp / s.max_hp < best->hp / best->max_hp))
                        best = &s;
                if (best) {
                    const auto c = cambc::vision::champion_at(best->pos);
                    if (c.is_visible() && a.use(c)) { cast_exclusive = exclusive; return true; }
                }
            } else if (a.is_shield()) {
                if (threatened && me.health() < me.max_health() * 0.95f && a.use()) { cast_exclusive = exclusive; return true; }
            }
        } catch (const std::exception& error) {
            if (trace) std::cout << "warn spell: " << error.what() << '\n';
        }
    }
    return false;
}

bool Bot::basic_attack() {
    auto& me = cambc::self();
    if (!me.can_attack()) return false;
    const auto pos = me.position();
    const int range = me.attack_range();
    try {
        const Seen* best = nullptr;
        for (const auto& e : enemies)
            if (cheb(e.pos, pos) <= range && (!best || e.hp < best->hp)) best = &e;
        if (best) {
            const auto c = cambc::vision::champion_at(best->pos);
            if (!c.is_visible()) return false;
            me.attack(c);
            attacked = true;
        } else {
            const auto awake = cambc::vision::awake_camp_tiles();
            auto mask = awake;
            if (target >= 0) mask |= cambc::vision::camp_tiles() & groups[static_cast<std::size_t>(target)].tiles;
            vec2 chosen{-1, -1};
            float chosen_score = 0;
            for (auto p : mask.positions()) {
                if (cheb(p, pos) > range || !cambc::vision::has_camp(p)) continue;
                const auto c = cambc::vision::camp_at(p);
                const float hp = c.health();
                if (hp <= 0) continue;
                // Monsters first (they hit back), lowest health first to finish kills.
                const float score = (is_monster_tile(groups, p) ? 100000.0f : 0.0f) - hp + (awake.is_set(p) ? 50000.0f : 0.0f);
                if (chosen.x < 0 || score > chosen_score) { chosen = p; chosen_score = score; }
            }
            if (chosen.x < 0) return false;
            me.attack(cambc::vision::camp_at(chosen));
            attacked = true;
        }
    } catch (const std::exception& error) {
        if (trace) std::cout << "warn attack: " << error.what() << '\n';
        return attacked;
    }
    // Attack reset right after our own attack, then strike again.
    try {
        if (attacked && !me.can_attack() && !cast_exclusive) {
            for (const auto& a : me.abilities()) {
                if (!a.is_attack_reset() || !a.is_ready()) continue;
                if (a.use() && me.can_attack()) {
                    observe_units();
                    attacked = false;
                    basic_attack();
                    attacked = true;
                }
                break;
            }
        }
    } catch (const std::exception& error) {
        if (trace) std::cout << "warn reset: " << error.what() << '\n';
    }
    return true;
}

bool Bot::try_bank() {
    auto& me = cambc::self();

    const unsigned gems = me.gem_count();
    if (gems == 0) return false;
    const auto pos = me.position();
    const int round = static_cast<int>(cambc::round_number());
    const int to_altar = altar_field.at(pos);
    if (to_altar >= far_away) return false;
    const bool endgame = round + to_altar + static_cast<int>(gems / 10) + 8 >= static_cast<int>(cambc::constants::max_rounds);
    const bool full = gems >= plan.bank_gems;
    const bool close = to_altar <= 2 || (gems >= 15 && to_altar <= 6);
    if (!endgame && !full && !close) return false;
    if (attacked && !endgame && !full) return false;

    for (auto altar : cambc::map::altars().positions()) {
        if (cheb(pos, altar) > cambc::constants::submit_gems_range) continue;
        if (cambc::vision::has_altar_lock(altar)) {
            const auto lock = cambc::vision::altar_lock_at(altar);
            if (lock.is_enemy()) {
                if (me.can_attack() && cheb(pos, altar) <= me.attack_range()) {
                    me.attack(lock);
                    attacked = true;
                    return true;
                }
                continue;
            }
        }
        if (cast_exclusive) return true;
        me.submit_gems(altar);
        return true;
    }
    step_along(altar_field);
    return true;
}

void Bot::shop() {
    auto& me = cambc::self();
    using it = cambc::item_type;
    // Damage dealers want camp damage first; front-liners want camp damage reduction.
    static constexpr std::array ranged_plan{it::monster_shard_of_strength, it::power_shard_of_strength,
        it::jungle_shard_of_health, it::power_shard_of_strength};
    static constexpr std::array melee_plan{it::jungle_shard_of_strength, it::monster_shard_of_strength,
        it::power_shard_of_health, it::power_shard_of_strength};
    static constexpr std::array ryth_plan{it::jungle_shard_of_health, it::monster_shard_of_strength,
        it::power_shard_of_health, it::power_shard_of_strength};
    const bool is_ryth = me.champion_name() == cambc::champion_type::ryth;
    for (int guard = 0; guard < 30; ++guard) {
        const std::size_t stage = std::min<std::size_t>(static_cast<std::size_t>(purchases / 5), 3);
        const auto item = ranged ? ranged_plan[stage] : (is_ryth ? ryth_plan[stage] : melee_plan[stage]);
        if (!me.can_buy(item, 1)) return;
        me.buy(item, 1);
        ++purchases;
    }
}

Fight Bot::assess_fight(const Seen** weakest) const {
    auto& me = cambc::self();
    const auto pos = me.position();
    float theirs = 0;
    const Seen* low = nullptr;
    for (const auto& e : enemies) {
        if (cheb(e.pos, pos) > 7) continue;
        theirs += e.hp * std::max(e.str, 20.0f);
        if (cambc::map::spawn_region(team::enemy).is_set(e.pos)) continue;
        if (!low || e.hp < low->hp) low = &e;
    }
    *weakest = low;
    if (theirs <= 0) return Fight::none;
    float ours = me.health() * me.strength();
    for (const auto& s : allies)
        if (cheb(s.pos, pos) <= 6) ours += s.hp * std::max(s.str, 20.0f);
    const float ratio = ours / theirs;
    const bool finishable = low && low->hp <= me.strength() * 2.5f && cheb(low->pos, pos) <= 4;
    if (ratio >= 1.3f || (finishable && ratio >= 0.8f)) return plan.chase || ratio >= 1.6f ? Fight::engage : Fight::hold;
    if (ratio < 0.85f) return Fight::retreat;
    return Fight::hold;
}

int Bot::choose_target(float keep_bonus) {
    auto& me = cambc::self();
    const auto pos = me.position();
    const int round = static_cast<int>(cambc::round_number());

    Field from_me;
    cambc::bitboard root;
    root.set(pos);
    from_me.dist.compute(walls, root, from_me.seen);

    std::vector<int> claims(groups.size(), 0);
    for (int s = 0; s < std::min(bodies, 8); ++s) {
        if (s == slot) continue;
        const int t = static_cast<int>(read_shared(s)) - 1;
        if (t >= 0 && t < static_cast<int>(groups.size())) ++claims[static_cast<std::size_t>(t)];
    }
    const int leader_target = slot != 0 ? static_cast<int>(read_shared(0)) - 1 : -1;

    const float dps = std::max(20.0f, my_dps());
    // Gold matters early (shards) and decides gem-less maps on the tiebreak.
    const float gold_weight = plan.gemless ? 1.0f : (round < 700 ? 0.25f : 0.12f);
    const bool support = me.champion_name() == cambc::champion_type::willow;
    const int follow = slot != 0 ? static_cast<int>(read_shared(0)) - 1 : static_cast<int>(read_shared(1)) - 1;
    const float hp_now = me.health();
    const int zone_lo = slot * 256 / std::max(1, bodies), zone_hi = (slot + 1) * 256 / std::max(1, bodies);

    int best = -1;
    float best_score = 0;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        if (!(g.flags & group_reachable) || g.facts.monolith || (g.facts.gems <= 0 && g.facts.gold <= 0)) continue;
        int d = far_away;
        for (auto t : g.facts.tiles) d = std::min(d, from_me.at(t));
        if (d >= far_away) continue;
        const int k = claims[i];
        const float value = static_cast<float>(g.facts.gems) + gold_weight * static_cast<float>(g.facts.gold);
        const int wait = std::max(0, g.respawn_at - round);
        // Helpers needed so the front-liner survives the clear.
        const auto damage_with = [&](int helpers) {
            return g.facts.str * (g.facts.hp / (dps + 50.0f * static_cast<float>(helpers))) * 0.6f;
        };
        int need = 0;
        while (need < bodies - 1 && damage_with(need) > hp_now * 0.85f + 100.0f) ++need;
        const int helpers = plan.mode == mode_spread ? k : std::max(k, need);
        const float team_dps = dps + 50.0f * static_cast<float>(helpers);
        if (g.facts.golem && team_dps < plan.golem_dps * 10.0f) continue;
        const float clear = g.reward ? 0.0f : g.facts.hp / team_dps;
        const float time = static_cast<float>(std::max(d, wait)) + clear + 6.0f;
        float score = value / time;
        if (damage_with(helpers) > hp_now * 0.85f + 100.0f) score *= 0.03f;
        if (k > need) score *= std::pow(0.55f, static_cast<float>(k - need));
        else if (need > 0 && k >= need) score *= 1.3f;
        if (g.reward && k > 0) score *= 0.2f;
        if (plan.mode == mode_spread && g.zone >= zone_lo && g.zone < zone_hi) score *= 1.6f;
        if (plan.mode != mode_spread && static_cast<int>(i) == leader_target) score *= 4.0f;
        if (static_cast<int>(i) == target) score *= keep_bonus;
        if ((g.flags & group_enemy_side) && !plan.gemless) score *= g.facts.gems > 0 ? 0.5f : 0.25f;
        if (support && static_cast<int>(i) == follow) score *= 6.0f;
        if (score > best_score) { best_score = score; best = static_cast<int>(i); }
    }
    return best;
}

void Bot::recall() {
    try {
        cambc::self().recall();
    } catch (const std::exception& error) {
        if (trace) std::cout << "warn recall r" << cambc::round_number() << ": " << error.what() << '\n';
    }
}

bool Bot::shopping_trip(Fight fight) {
    auto& me = cambc::self();
    if (me.champion_name() == cambc::champion_type::tavi || fight != Fight::none || attacked) return false;
    const unsigned gold = me.gold_count();
    const auto pos = me.position();
    const int home = home_field.at(pos);
    if (gold >= 120 && home <= 6) {
        step_along(home_field);
        return true;
    }
    if (gold >= 270 && home > 12 && cambc::vision::awake_camp_tiles().is_empty()) {
        recall();
        return true;
    }
    return false;
}

bool Bot::farm() {
    auto& me = cambc::self();
    constexpr int retarget_period = 40;
    const int round = static_cast<int>(cambc::round_number());
    const auto pos = me.position();

    bool retarget = target < 0 || round - chosen_round >= retarget_period;
    if (target >= 0) {
        const auto& g = groups[static_cast<std::size_t>(target)];
        if (g.respawn_at > round + 6) retarget = true;
        if (g.reward) {
            for (const auto& s : allies)
                if (g.tiles.is_set(s.pos) && !g.tiles.is_set(pos)) retarget = true;
        }
    }
    if (retarget) {
        // Keep the target while standing on its region capturing.
        const bool capturing = target >= 0 && groups[static_cast<std::size_t>(target)].reward
            && groups[static_cast<std::size_t>(target)].tiles.is_set(pos);
        if (!capturing) {
            const bool valid = target >= 0 && groups[static_cast<std::size_t>(target)].respawn_at <= round + 6;
            target = choose_target(valid ? 1.5f : 1.0f);
            chosen_round = round;
        }
    }
    if (target < 0) return false;
    auto& g = groups[static_cast<std::size_t>(target)];
    const auto& field = group_field(target);

    if (g.reward) {
        if (g.tiles.is_set(pos)) return true; // hold the capture
        step_along(field);
        return true;
    }
    // Never stand on a captured region: an occupied spawn tile stops the group respawning.
    if (g.tiles.is_set(pos)) {
        step_off(g.tiles);
        return true;
    }
    // Close enough to strike a member: hold position (ranged) or close in (melee).
    const auto members = cambc::vision::camp_tiles() & g.tiles;
    if (!members.is_empty()) {
        for (auto p : members.positions())
            if (cheb(p, pos) <= me.attack_range()) return true;
    } else if (field.at(pos) <= 1 && g.respawn_at > 0) {
        return true; // wait beside the region for the respawn
    }
    step_along(field);
    return true;
}

void Bot::play_turn() {
    auto& me = cambc::self();
    if (!ready) init();
    walked_fail = false;
    avoid_active = false;
    cast_exclusive = false;
    attacked = false;

    if (!me.is_alive()) {
        shop();
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;

    const auto pos = me.position();
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(pos);
    if (at_spawn || me.champion_name() == cambc::champion_type::tavi) shop();
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) return;

    update_groups();
    observe_units();

    // Heal up on spawn before heading out.
    if (at_spawn && me.health() < me.max_health() * 0.9f && enemies.empty()) {
        try_spells();
        basic_attack();
        return;
    }

    // Retreat when badly hurt: step out of reach, then recall.
    const Seen* weakest = nullptr;
    const Fight fight = assess_fight(&weakest);
    const float hp_frac = me.health() / me.max_health();
    const float retreat_at = fight == Fight::none ? 0.3f : 0.45f;
    if (!at_spawn && (hp_frac < retreat_at || (fight == Fight::retreat && hp_frac < 0.75f))) {
        const auto threat = threat_tiles();
        if (me.gem_count() >= 10 && altar_field.at(pos) <= 1 && try_bank()) return;
        if (!threat.is_set(pos)) {
            recall();
            return;
        }
        if (!step_away(threat)) {
            try_spells();
            basic_attack();
        } else if (me.can_attack()) {
            observe_units();
            basic_attack();
        }
        return;
    }
    if (fight == Fight::retreat) {
        // Outmatched but healthy: back off without starting anything.
        const auto threat = threat_tiles();
        if (threat.is_set(pos) && step_away(threat)) {
            if (me.can_attack()) { observe_units(); basic_attack(); }
            return;
        }
    }

    try_spells();
    basic_attack();

    // Even fight: hold ground, do not walk into the enemy's reach.
    avoid_active = fight == Fight::hold;
    if (avoid_active) {
        avoid.clear();
        for (const auto& e : enemies) add_square(avoid, e.pos, 3);
    }
    bool chased = false;
    if (fight == Fight::engage && weakest && me.can_move() && !cast_exclusive && !walked_fail
        && cheb(weakest->pos, pos) > me.attack_range()) {
        try {
            chased = me.move_towards(weakest->pos);
        } catch (const std::exception& error) {
            if (trace) std::cout << "warn chase r" << cambc::round_number() << ": " << error.what() << '\n';
        }
        if (!chased) walked_fail = true;
    }
    if (!chased && !try_bank() && !shopping_trip(fight)) farm();

    // Strike after moving if the allowance remains.
    if (me.can_attack()) {
        observe_units();
        basic_attack();
    }

    if (slot < 8) cambc::shared_memory[static_cast<std::size_t>(slot)] = static_cast<std::uint8_t>(target + 1);
    if (trace && (cambc::round_number() % 25 == 0 || (!enemies.empty() && cambc::round_number() < 500)))
        std::cout << "r" << cambc::round_number() << " E" << enemies.size() << " A" << allies.size() << " s" << slot << " t" << target << " fight" << int(fight) << " hp"
                  << int(me.health()) << " gems " << me.gem_count() << " gold " << me.gold_count() << " at "
                  << cambc::to_string(me.position()) << " alt" << altar_field.at(me.position()) << " fuel " << cambc::fuel_remaining();
    if (trace && cambc::round_number() % 25 == 0 && target >= 0) {
        const auto& g = groups[static_cast<std::size_t>(target)];
        std::cout << " | g reward" << g.reward << " respawn" << g.respawn_at << " gems" << g.facts.gems << " tiles";
        for (auto t : g.facts.tiles) std::cout << cambc::to_string(t);
    }
    if (trace && (cambc::round_number() % 25 == 0 || (!enemies.empty() && cambc::round_number() < 500))) std::cout << '\n';
}
