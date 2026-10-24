// Safe banker: farm camps the team can clear without losing anyone, tribute gems as soon as
// an altar is close, recall early, and step away from fights we do not clearly win.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>
#include <iostream>

using cambc::bitboard;
using cambc::distance_field;
using cambc::team;
using cambc::vec2;

namespace {

int dbg_reason = 0;
int W = 0, H = 0;
constexpr float DANGER = 0.75f;
constexpr int THREAT_R = 5;
constexpr unsigned SHOP_GOLD = 150;
constexpr float OUTMATCH = 1.1f;
constexpr std::uint16_t UNR = distance_field::unreachable;
constexpr std::array<vec2, 8> DIRS{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
                                   vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

int cheb(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
bool inb(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < W && p.y < H; }

// Chebyshev square with manual bounds checks (draw_chebyshev can fail near edges).
bitboard square(vec2 c, int r) {
    bitboard b;
    b.clear();
    if (c.x - r >= 0 && c.y - r >= 0 && c.x + r < W && c.y + r < H) {
        b.draw_chebyshev(c, static_cast<float>(r));
        return b;
    }
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            const vec2 p{c.x + dx, c.y + dy};
            if (inb(p)) b.set(p);
        }
    return b;
}

// Entity lookups that report absence instead of throwing: a unit can leave or die between
// the occupancy mask and the lookup (engine errors 97/98).
std::optional<cambc::camp> camp_here(vec2 p) {
    if (!inb(p) || !cambc::vision::has_camp(p)) return std::nullopt;
    try {
        auto c = cambc::vision::camp_at(p);
        if (!c.is_visible()) return std::nullopt;
        return c;
    } catch (const cambc::error&) {
        return std::nullopt;
    }
}

std::optional<cambc::champion> champion_here(vec2 p) {
    if (!inb(p) || !cambc::vision::has_champion(p)) return std::nullopt;
    try {
        auto c = cambc::vision::champion_at(p);
        if (!c.is_visible()) return std::nullopt;
        return c;
    } catch (const cambc::error&) {
        return std::nullopt;
    }
}

// Move to an adjacent tile; a tile the engine knows is occupied (e.g. by a ward or a unit
// revealed this round) counts as a failed move rather than an error.
bool safe_move(vec2 t) {
    if (cambc::vision::has_champion(t) || cambc::vision::has_camp(t) || cambc::vision::has_teleport(t)
        || cambc::vision::has_ward(t))
        return false;
    try {
        return cambc::self().move_absolute(t);
    } catch (const cambc::error& e) {
        if (e.code() == cambc::error_code::invalid_action_move_bad_destination) return false;
        throw;
    }
}

// Basic attack, then spend a ready attack reset on a second hit at the same tile.
template <class Lookup>
void attack_twice(vec2 tile, Lookup lookup) {
    auto& me = cambc::self();
    auto first = lookup(tile);
    if (!first || !me.can_attack()) return;
    me.attack(*first);
    if (me.can_attack() || !me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        try {
            if (!ab.use()) return;
        } catch (const cambc::error& e) {
            // "An available attack cannot be reset": the engine disagrees about the allowance.
            if (e.code() == cambc::error_code::invalid_action_spell_cast_cant_cast) return;
            throw;
        }
        auto again = lookup(tile);
        if (again && me.can_attack()) me.attack(*again);
        return;
    }
}

struct CampStat { float hp, str; int range; };
CampStat camp_stat(int kind) {
    switch (static_cast<cambc::camp_type>(kind)) {
    case cambc::camp_type::wolf: return {1350, 20, 3};
    case cambc::camp_type::elk: return {2400, 60, 4};
    case cambc::camp_type::gem_cluster: return {120, 0, 0};
    case cambc::camp_type::gold_cluster: return {120, 0, 0};
    case cambc::camp_type::monolith: return {20000, 200, 5};
    case cambc::camp_type::golem: return {5000, 120, 4};
    }
    return {1000, 50, 3};
}

std::vector<cambc::map::camp_group>& group_handles() {
    static std::vector<cambc::map::camp_group> handles = cambc::map::camp_groups();
    return handles;
}

bitboard occupied() {
    return cambc::vision::champion_tiles() | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
}


std::vector<Seen> visible_champions(team side) {
    std::vector<Seen> out;
    for (auto p : cambc::vision::champion_tiles(side).positions()) {
        auto c = champion_here(p);
        if (!c) continue;
        out.push_back({p, c->health(), c->strength()});
    }
    return out;
}

// Awake camps that can currently hit `tile`.
bool camp_threat_at(vec2 tile) {
    for (auto p : cambc::vision::camp_tiles().positions()) {
        if (cheb(p, tile) > 5) continue;
        auto c = camp_here(p);
        if (!c || !c->is_awake()) continue;
        if (c->strength() > 0 && cheb(p, tile) <= c->attack_range()) return true;
    }
    return false;
}

} // namespace

void Bot::init() {
    initialised = true;
    W = cambc::map::width();
    H = cambc::map::height();
    static_walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    own_spawn = cambc::map::spawn_region(team::ally);
    approach.clear();
    all_spawns.clear();
    for (auto a : cambc::map::altars().positions()) approach |= square(a, cambc::constants::submit_gems_range);
    approach &= ~static_walls;
    bitboard visited;
    {
        bitboard walls = static_walls;
        spawn_field.compute(walls, own_spawn, visited);
    }
    {
        bitboard walls = static_walls;
        altar_field.compute(walls, approach, visited);
    }

    for (const auto& g : group_handles()) {
        GroupInfo info;
        info.spawns = g.spawn_tiles();
        info.gems = static_cast<int>(g.gem_value());
        info.gold = static_cast<int>(g.gold_value());
        info.respawn = g.max_respawn_timer();
        int adist = UNR;
        for (const auto& c : g.camps()) {
            const vec2 t = c.spawn_position();
            const int kind = static_cast<int>(c.kind());
            info.tiles.push_back(t);
            info.kinds.push_back(kind);
            const auto st = camp_stat(kind);
            if (st.str > 0) {
                info.monster_hp += st.hp;
                info.strength += st.str;
                info.monsters++;
                info.max_range = std::max(info.max_range, st.range);
            } else {
                info.cluster_hp += st.hp;
            }
            adist = std::min<int>(adist, altar_field.get(t));
        }
        info.altar_dist = adist;
        for (auto t : info.tiles) info.radius = std::max(info.radius, cheb(t, info.tiles[0]));
        all_spawns |= info.spawns;
        groups.push_back(std::move(info));
    }
    if (cambc::self().champion_name() == cambc::champion_type::vendetta)
        for (std::size_t i = 0; i < groups.size(); ++i)
            std::cout << "DBG group " << i << " at " << groups[i].tiles[0].x << "," << groups[i].tiles[0].y << " n=" << groups[i].tiles.size()
                      << " gems=" << groups[i].gems << " gold=" << groups[i].gold << " mhp=" << groups[i].monster_hp << " spawnd=" << spawn_field.get(groups[i].tiles[0]) << " altar=" << groups[i].altar_dist << std::endl;
    ready_round.assign(groups.size(), 0);
    banned_until.assign(groups.size(), 0);

    const auto allies = cambc::champions(team::ally);
    const auto& me = cambc::self();
    for (std::size_t i = 0; i < allies.size(); ++i)
        if (allies[i] == me) my_index = static_cast<int>(i);
    if (my_index > 7) my_index = 7;
}

int Bot::read_claim(int index) {
    std::uint8_t b = 0;
    cambc::shared_memory.read(static_cast<std::size_t>(index), std::span{&b, 1});
    return static_cast<int>(b) - 1;
}

void Bot::write_claim(int group) {
    std::uint8_t b = static_cast<std::uint8_t>(group + 1);
    cambc::shared_memory.write(static_cast<std::size_t>(my_index), std::span{&b, 1});
}

void Bot::buy_items() {
    auto& me = cambc::self();
    const auto counts = me.item_count();
    using cambc::item_type;
    // Durability emblem of strength first (farming speed and 10% less damage), then
    // alternate power strength and health shards.
    int dur = static_cast<int>(counts[item_type::durability_shard_of_strength])
        + 5 * static_cast<int>(counts[item_type::durability_emblem_of_strength]);
    int power = static_cast<int>(counts[item_type::power_shard_of_strength])
        + 6 * static_cast<int>(counts[item_type::power_emblem_of_strength]);
    int health = static_cast<int>(counts[item_type::power_shard_of_health])
        + 6 * static_cast<int>(counts[item_type::power_emblem_of_health]);
    for (int guard = 0; guard < 20; ++guard) {
        item_type item = item_type::durability_shard_of_strength;
        if (dur >= 5) item = health * 2 < power ? item_type::power_shard_of_health : item_type::power_shard_of_strength;
        if (me.gold_count() < item.cost() || !me.can_buy(item, 1)) break;
        me.buy(item, 1);
        if (dur < 5) ++dur;
        else if (item == item_type::power_shard_of_health) ++health;
        else ++power;
    }
}

void Bot::update_groups() {
    const auto vis = cambc::vision::visible_tiles();
    const int now = static_cast<int>(cambc::round_number());
    auto& handles = group_handles();
    const vec2 pos = cambc::self().position();
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (cheb(groups[i].tiles[0], pos) > 8 + groups[i].radius) continue;   // out of sight anyway
        bool seen = false;
        for (auto t : groups[i].tiles)
            if (cheb(t, pos) <= 8 && vis.is_set(t)) { seen = true; break; }
        if (!seen) continue;
        const auto state = handles[i].state();
        if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&state))
            ready_round[i] = now + std::max(1, r->timer);
        else
            ready_round[i] = 0;
    }
}

bool Bot::step_to(const bitboard& goals, const distance_field* far_field) {
    auto& me = cambc::self();
    if (moved || cast_lock || !me.can_move()) return false;
    const vec2 pos = me.position();
    if (far_field) {
        // Far from the goal: follow a precomputed terrain field and only dodge units next to us.
        const auto cur = far_field->get(pos);
        if (cur != UNR && cur > 8) {
            vec2 best = pos;
            std::uint16_t bestd = cur;
            for (auto d : DIRS) {
                const vec2 t{pos.x + d.x, pos.y + d.y};
                if (!inb(t) || static_walls.is_set(t)) continue;
                const auto fd = far_field->get(t);
                if (fd >= bestd) continue;
                if (cambc::vision::has_champion(t) || cambc::vision::has_camp(t)) continue;
                bestd = fd;
                best = t;
            }
            if (best != pos) {
                moved = true;
                return safe_move(best);
            }
        }
    }
    bitboard blocked = static_walls | occupied();
    blocked.set(pos, false);
    bitboard roots = goals & ~blocked;
    if (roots.is_empty() || roots.is_set(pos)) return false;
    distance_field field;
    bitboard visited;
    {
        bitboard walls = blocked;   // the search gets its own copy
        field.compute_to(walls, roots, square(pos, 0), visited);
    }
    if (!visited.is_set(pos)) {
        // Allies may block a corridor: plan through champions, then take any free improving step.
        bitboard walls = static_walls | cambc::vision::camp_tiles();
        walls.set(pos, false);
        roots = goals & ~walls;
        if (roots.is_empty()) return false;
        field.compute_to(walls, roots, square(pos, 0), visited, 30);
        if (!visited.is_set(pos)) return false;
    }
    const auto cur = field.get(pos);
    vec2 best = pos;
    std::uint16_t bestd = cur;
    for (auto d : DIRS) {
        const vec2 t{pos.x + d.x, pos.y + d.y};
        if (!inb(t) || blocked.is_set(t) || !visited.is_set(t)) continue;
        if (cambc::vision::has_champion(t) || cambc::vision::has_camp(t)) continue;
        const auto fd = field.get(t);
        if (fd < bestd) { bestd = fd; best = t; }
    }
    if (best == pos) return false;
    moved = true;
    return safe_move(best);
}

bool Bot::flee(const std::vector<vec2>& threats) {
    auto& me = cambc::self();
    if (moved || cast_lock || !me.can_move() || threats.empty()) return false;
    const vec2 pos = me.position();
    auto walls = static_walls | occupied();
    walls.set(pos, false);
    auto score = [&](vec2 t) {
        int mind = 99;
        for (auto e : threats) mind = std::min(mind, cheb(t, e));
        const int sd = spawn_field.get(t);
        return mind * 10.0f - (sd >= UNR ? 500.0f : sd * 0.6f);
    };
    vec2 best = pos;
    float bests = score(pos);
    for (auto d : DIRS) {
        const vec2 t{pos.x + d.x, pos.y + d.y};
        if (!inb(t) || walls.is_set(t)) continue;
        if (cambc::vision::has_champion(t) || cambc::vision::has_camp(t)) continue;
        const float s = score(t);
        if (s > bests) { bests = s; best = t; }
    }
    if (best == pos) return false;
    moved = true;
    return safe_move(best);
}

bool Bot::go_bank() {
    auto& me = cambc::self();
    if (me.gem_count() == 0) return false;
    const vec2 pos = me.position();
    const int range = cambc::constants::submit_gems_range;
    bitboard goals = approach;
    for (auto a : cambc::map::altars().positions()) {
        const bool locked = cambc::vision::has_altar_lock(a);
        const bool enemy_lock = locked && cambc::vision::altar_lock_at(a).is_enemy();
        if (cheb(pos, a) <= range) {
            if (!enemy_lock) {
                me.submit_gems(a);
                return true;
            }
            if (me.can_attack()) {
                me.attack(cambc::vision::altar_lock_at(a));
                return true;
            }
        }
    }
    // Remember enemy locks so a lock leaving vision does not send us back and forth.
    const int now = static_cast<int>(cambc::round_number());
    const auto altars = cambc::map::altars().positions();
    if (lock_until.size() != altars.size()) lock_until.assign(altars.size(), 0);
    const auto vis = cambc::vision::visible_tiles();
    const auto enemy_locks = cambc::vision::altar_lock_tiles(team::enemy);
    bitboard free_goals;
    free_goals.clear();
    int near_free = 999, near_locked = 999;
    for (std::size_t i = 0; i < altars.size(); ++i) {
        if (enemy_locks.is_set(altars[i])) lock_until[i] = now + 40;
        else if (vis.is_set(altars[i])) lock_until[i] = 0;
        const int d = cheb(pos, altars[i]);
        if (lock_until[i] <= now) {
            free_goals |= square(altars[i], range);
            near_free = std::min(near_free, d);
        } else {
            near_locked = std::min(near_locked, d);
        }
    }
    free_goals &= approach;
    // A lock takes a few hits to break; only detour when a free altar is not much farther.
    const bool no_locks = near_locked == 999;
    if (!free_goals.is_empty() && near_free <= near_locked + 8
        && step_to(free_goals, no_locks ? &altar_field : nullptr)) return true;
    if (!moved) step_to(goals, &altar_field);
    return true;
}

bool Bot::cast_on_camp(const vec2& tile) {
    auto& me = cambc::self();
    if (moved || cast_lock || !me.can_cast()) return false;
    for (const auto& ab : me.abilities()) {
        if (!(ab.is_damage() || ab.is_aoe_damage() || ab.is_cc())) continue;
        if (!ab.is_ready()) continue;
        if (!(ab.can_target_camps() || ab.can_target_ground())) continue;
        if (!ab.valid_tiles().is_set(tile)) continue;
        auto c = camp_here(tile);
        if (!c) return false;
        cast_lock = true;
        return ab.can_target_camps() ? ab.use(*c) : ab.use(tile);
    }
    return false;
}

void Bot::cast_on_enemy(const std::vector<Seen>& enemies) {
    auto& me = cambc::self();
    if (moved || cast_lock || !me.can_cast() || enemies.empty()) return;
    for (const auto& ab : me.abilities()) {
        if (!(ab.is_damage() || ab.is_aoe_damage() || ab.is_cc())) continue;
        if (!ab.is_ready() || !(ab.can_target_enemies() || ab.can_target_ground())) continue;
        const auto mask = ab.valid_tiles();
        const Seen* best = nullptr;
        for (const auto& e : enemies)
            if (mask.is_set(e.pos) && (!best || e.hp < best->hp)) best = &e;
        if (!best) continue;
        auto c = champion_here(best->pos);
        if (!c) continue;
        cast_lock = true;
        if (ab.can_target_enemies()) ab.use(*c);
        else ab.use(best->pos);
        return;
    }
}

void Bot::use_support_spells() {
    auto& me = cambc::self();
    if (!me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_heal() && ab.can_target_allies()) {
            const auto mask = ab.valid_tiles();
            const float amount = ab.heal_amount();
            for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
                if (p == me.position() || !mask.is_set(p)) continue;
                auto c = champion_here(p);
                if (!c || c->health() <= 0) continue;
                const float missing = c->max_health() - c->health();
                if (missing >= amount * 0.7f || c->health() < 0.45f * c->max_health()) {
                    ab.use(*c);
                    return;
                }
            }
        } else if (ab.is_shield() && ab.can_target_self()) {
            bool danger = camp_threat_at(me.position());
            if (!danger)
                for (auto p : cambc::vision::champion_tiles(team::enemy).positions())
                    if (cheb(p, me.position()) <= 4) danger = true;
            if (danger && me.health() < 0.97f * me.max_health()) {
                ab.use();
                return;
            }
        }
    }
}

void Bot::choose_target(const distance_field& from_me, const bitboard& reach) {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    const float my_str = std::max(1.0f, me.strength());
    const float my_hp = me.health();
    int helpers = 1;
    for (auto p : cambc::vision::champion_tiles(team::ally).positions())
        if (p != me.position() && cheb(p, me.position()) <= 8) ++helpers;
    std::array<int, 8> claims{};
    for (int j = 0; j < 8; ++j) claims[j] = j == my_index ? -1 : read_claim(j);
    const float gold_w = 0.33f;

    int best = -1;
    float best_score = 0;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        if (banned_until[i] > now) continue;
        if (g.monster_hp >= 20000) continue;          // monoliths are not worth the risk
        if (g.gems == 0 && g.gold == 0) continue;
        int dist = UNR;
        for (std::size_t k = 0; k < g.tiles.size() && k < 3; ++k)
            dist = std::min<int>(dist, from_me.get(g.tiles[k]));
        if (dist >= UNR) continue;
        const int wait = std::max(0, ready_round[i] - now);
        const int arrival = std::max(dist, wait);
        int claimants = 0;
        for (int j = 0; j < 8; ++j) if (claims[j] == static_cast<int>(i)) ++claimants;
        float factor = 1.0f;
        const float k = static_cast<float>(std::max(helpers, 1 + claimants));
        const float dps = my_str * k;
        if (g.monsters > 0) {
            // Camps hit the nearest champion; spread the expected damage over the helpers.
            const float dmg = g.strength * (g.monster_hp / dps) / k * (g.monsters > 1 ? 0.7f : 1.0f);
            if (dmg > DANGER * my_hp) factor = 0.05f;   // last resort only
        } else if (claimants > 0) {
            factor = 0.1f;                             // someone else is already on this harmless group
        }
        const float clear = (g.monster_hp + g.cluster_hp) / dps;
        const float value = g.gems + gold_w * g.gold;
        const float cost = arrival + clear + 6.0f + 0.4f * std::min(g.altar_dist, 60);
        float score = factor * value / cost;
        if (static_cast<int>(i) == target) score *= 1.25f;
        if (score > best_score) { best_score = score; best = static_cast<int>(i); }
    }
    if (best != target) wait_turns = 0;
    target = best;
    if (best < 0 && now % 25 == 0) {
        int nb = 0, nu = 0, nm = 0;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            if (banned_until[i] > now) ++nb;
            int dist = UNR;
            for (auto t : groups[i].tiles) if (reach.is_set(t)) dist = std::min<int>(dist, from_me.get(t));
            if (dist >= UNR) ++nu;
            if (groups[i].monster_hp >= 20000) ++nm;
        }
        std::cout << "DBG none " << groups.size() << " banned=" << nb << " unreach=" << nu << " mono=" << nm << " reachcnt=" << reach.count() << std::endl;
    }
}

bool Bot::fight_group() {
    if (target < 0) return false;
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    const auto& g = groups[target];
    const vec2 pos = me.position();
    const auto vis = cambc::vision::visible_tiles();
    bool seen = false;
    for (auto t : g.tiles)
        if (vis.is_set(t)) { seen = true; break; }
    const int range = me.attack_range();
    if (!seen) {
        // Camps hidden in bushes: close in, then step into the bush next to them.
        bitboard goals;
        goals.clear();
        for (auto t : g.tiles) goals |= square(t, range);
        goals &= ~static_walls;
        if (goals.is_set(pos)) {
            goals.clear();
            for (auto t : g.tiles) goals |= square(t, 1);
            goals &= ~static_walls;
            if (goals.is_set(pos)) goals &= cambc::map::bushes();
        }
        if (nav_key != target * 4 + 1 && cheb(pos, g.tiles[0]) > 10) {
            bitboard walls = static_walls, visited;
            nav_field.compute(walls, goals, visited);
            nav_key = target * 4 + 1;
        }
        if (!step_to(goals, nav_key == target * 4 + 1 ? &nav_field : nullptr) && !moved) {
            if (++wait_turns > 6) {
                banned_until[target] = now + 60;
                target = -1;
                wait_turns = 0;
                { dbg_reason = 6; return false; }
            }
        }
        return true;
    }
    const auto state = group_handles()[target].state();
    if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&state)) {
        ready_round[target] = now + std::max(1, r->timer);
        if (r->timer <= 2) {
            if (++wait_turns < 25) {
                // About to respawn (or blocked by someone standing on it): clear the tiles and wait.
                if (all_spawns.is_set(pos)) step_to(square(pos, 2) & ~all_spawns & ~static_walls);
                return true;
            }
            banned_until[target] = now + 40;
        }
        target = -1;
        { dbg_reason = 1; return false; }
    }
    if (std::holds_alternative<cambc::map::camp_group_reward_available>(state)) {
        if (g.spawns.is_set(pos)) return true;          // capturing
        const bool vesper = me.champion_name() == cambc::champion_type::vesper_vale;
        for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
            if (p == pos) continue;
            const bool other_on = g.spawns.is_set(p);
            bool other_vesper = false;
            if (!vesper && cheb(p, g.tiles[0]) <= 6) {
                auto c = champion_here(p);
                other_vesper = c && c->champion_name() == cambc::champion_type::vesper_vale;
            }
            if ((other_on && !vesper) || other_vesper) {
                banned_until[target] = now + 8;
                target = -1;                            // an ally is capturing it (Vesper gets bonus gold)
                { dbg_reason = 2; return false; }
            }
        }
        const auto zone = square(g.tiles[0], 5);
        const int foes = static_cast<int>((cambc::vision::champion_tiles(team::enemy) & zone).count());
        const int friends = static_cast<int>((cambc::vision::champion_tiles(team::ally) & zone).count())
            + (zone.is_set(pos) ? 0 : 1);
        if (foes > friends) {
            banned_until[target] = now + 30;
            target = -1;
            { dbg_reason = 3; return false; }
        }
        if (!step_to(g.spawns)) {
            if (moved) return true;
            target = -1;
            { dbg_reason = 4; return false; }
        }
        return true;
    }

    // Alive: list living camps of this group.
    struct Live { vec2 pos; float hp, str; bool awake; };
    std::vector<Live> live;
    float monster_hp = 0, strength = 0;
    bool any_awake = false;
    for (auto t : g.tiles) {
        auto c = camp_here(t);
        if (!c || !c->is_alive()) continue;
        const Live l{t, c->health(), c->strength(), c->is_awake()};
        live.push_back(l);
        if (l.str > 0) { monster_hp += l.hp; strength += l.str; }
        any_awake = any_awake || l.awake;
    }
    if (live.empty()) {
        bitboard goals;
        goals.clear();
        for (auto t : g.tiles) goals |= square(t, 1);
        step_to(goals & ~static_walls);
        return true;
    }

    wait_turns = 0;

    // Choose a camp: killing blows first, then monsters (lowest health), then clusters.
    const auto atk = me.attack_tiles();
    const float my_str = me.strength();
    auto rank = [&](const Live& l) {
        float s = 0;
        if (l.hp <= my_str) s += 100000;
        if (l.str > 0) s += 50000;
        return s - l.hp;
    };
    const Live* in_range = nullptr;
    const Live* primary = nullptr;
    for (const auto& l : live) {
        if (!primary || rank(l) > rank(*primary)) primary = &l;
        if (atk.is_set(l.pos) && (!in_range || rank(l) > rank(*in_range))) in_range = &l;
    }
    // Prefer the in-range camp unless the primary target is a monster and the in-range one is not.
    const Live* tgt = in_range;
    if (in_range && primary->str > 0 && in_range->str <= 0 && in_range->hp > my_str) tgt = nullptr;

    if (tgt) {
        cast_on_camp(tgt->pos);
        if (me.can_attack()) {
            auto c = camp_here(tgt->pos);
            if (c && c->is_alive()) attack_twice(tgt->pos, camp_here);
        }
        return true;
    }
    cast_on_camp(primary->pos);
    bitboard goals = square(primary->pos, range) & ~static_walls;
    if (!step_to(goals) && !moved) {
        // The preferred camp may be walled in by its group: approach any living member.
        goals.clear();
        for (const auto& l : live) goals |= square(l.pos, range);
        step_to(goals & ~static_walls);
    }
    if (me.can_attack()) {
        const auto atk2 = me.attack_tiles();
        for (const auto& l : live) {
            if (!atk2.is_set(l.pos)) continue;
            auto c = camp_here(l.pos);
            if (c && c->is_alive()) { attack_twice(l.pos, camp_here); break; }
        }
    }
    return true;
}

static int act = 0;
static std::uint64_t prof[8], prof_max[8];
#define PROF_BEGIN(i) const auto _pf##i = cambc::fuel_remaining();
#define PROF_END(i) { const auto d = _pf##i - cambc::fuel_remaining(); prof[i] += d; prof_max[i] = std::max(prof_max[i], d); }
static int act_count[16];
void Bot::play_turn() {
    act = 12;
    play_turn_inner();
    act_count[act]++;
    static std::uint64_t min_fuel = 1u << 30;
    static int low_turns = 0;
    const auto fr = cambc::fuel_remaining();
    if (cambc::round_number() > 2) { min_fuel = std::min<std::uint64_t>(min_fuel, fr); if (fr < 20000) ++low_turns; }
    if (cambc::round_number() % 400 == 0) {
        std::cout << "FUEL r" << cambc::round_number() << " min=" << min_fuel << " low=" << low_turns;
        for (int i = 0; i < 5; ++i) std::cout << " p" << i << "=" << prof[i] / cambc::round_number() << "/" << prof_max[i];
        std::cout << std::endl;
    }
    if (cambc::round_number() == 800) {
        std::cout << "ACT " << cambc::to_string(cambc::self().champion_name());
        for (int i = 0; i < 13; ++i) std::cout << " " << i << ":" << act_count[i];
        std::cout << std::endl;
    }
}

void Bot::play_turn_inner() {
    if (!initialised) init();
    moved = false;
    cast_lock = false;
    auto& me = cambc::self();
    if (!me.is_alive()) {
        act = 0;
        buy_items();
        return;
    }
    act = me.is_recalling() ? 1 : 2;
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;
    act = 3;
    if (me.buffs().has_stun()) return;
    act = 4;
    PROF_BEGIN(0)
    update_groups();
    PROF_END(0)

    const int now = static_cast<int>(cambc::round_number());
    const vec2 pos = me.position();
    const bool at_spawn = own_spawn.is_set(pos);
    if (at_spawn) buy_items();

    PROF_BEGIN(1)
    const auto enemies = visible_champions(team::enemy);
    const auto allies = visible_champions(team::ally);
    PROF_END(1)
    if (!enemies.empty()) {
        last_enemy_round = now;
        // Share the sighting (bytes 6..7: round / 8, coarse 7-tile cell).
        const vec2 e = enemies[0].pos;
        std::array<std::uint8_t, 2> msg{static_cast<std::uint8_t>(std::min(255, now / 8)),
            static_cast<std::uint8_t>(std::min(14, e.x / 7) * 15 + std::min(14, e.y / 7))};
        cambc::shared_memory.write(6, std::span{msg});
    }
    {
        std::array<std::uint8_t, 2> msg{};
        cambc::shared_memory.read(6, std::span{msg});
        team_enemy_round = msg[0] == 0 ? -1000 : msg[0] * 8;
        team_enemy_pos = vec2{(msg[1] / 15) * 7 + 3, (msg[1] % 15) * 7 + 3};
    }
    use_support_spells();

    const float hp = me.health(), max_hp = me.max_health();
    const float my_str = me.strength();
    const unsigned gems = me.gem_count();

    // Enemy champions: fight only clear wins, otherwise step away.
    if (!enemies.empty()) {
        float ours = 0, theirs = 0;
        for (const auto& a : allies)
            if (cheb(a.pos, pos) <= 6) ours += a.hp * std::max(a.str, 1.0f);
        std::vector<vec2> threats;
        for (const auto& e : enemies)
            if (cheb(e.pos, pos) <= THREAT_R) { theirs += e.hp * std::max(e.str, 1.0f); threats.push_back(e.pos); }
        if (!threats.empty()) {
            const auto atk = me.attack_tiles();
            const Seen* weakest = nullptr;
            for (const auto& e : enemies)
                if (atk.is_set(e.pos) && (!weakest || e.hp < weakest->hp)) weakest = &e;
            const bool routed = theirs > 1.6f * ours || hp < 0.25f * max_hp || (gems >= 15 && theirs > ours);
            const bool outmatched = theirs > OUTMATCH * ours || (gems >= 10 && theirs > 0.7f * ours);
            if (!routed) cast_on_enemy(enemies);
            if (weakest && !routed && me.can_attack()) {
                // Engaged: fleeing only hands out free hits, so fight back.
                auto c = champion_here(weakest->pos);
                if (c) { attack_twice(weakest->pos, champion_here); return; }
            }
            if (routed || outmatched) {
                if (flee(threats)) return;
                if (weakest && me.can_attack()) {
                    auto c = champion_here(weakest->pos);
                    if (c) attack_twice(weakest->pos, champion_here);
                }
                return;
            }
        }
    }

    act = 5;
    PROF_BEGIN(2)
    const bool threat_here = camp_threat_at(pos);
    PROF_END(2)
    // Low health: get out of camp range, then recall.
    if (!at_spawn && hp < 0.35f * max_hp) {
        if (threat_here) {
            std::vector<vec2> threats;
            for (auto p : cambc::vision::camp_tiles().positions())
                if (cheb(p, pos) <= 5) threats.push_back(p);
            if (flee(threats)) return;
        }
        if (gems > 0 && altar_field.get(pos) <= 1 && go_bank()) return;
        if (!threat_here) { me.recall(); return; }
    }
    act = 6;
    if (at_spawn && hp < 0.9f * max_hp && enemies.empty()) return;
    act = 7;

    // Recall early to shop and heal.
    if (!at_spawn && gems < 10 && (me.gold_count() >= SHOP_GOLD || (hp < 0.6f * max_hp && me.gold_count() >= 90))
        && spawn_field.get(pos) > 6 && !threat_here && enemies.empty()) {
        me.recall();
        return;
    }

    // Banking: tribute when an altar is close, the bag is big, or enemies were seen recently.
    bool capturing = false;
    if (target >= 0 && groups[target].spawns.is_set(pos)) capturing = true;
    const bool engaged = threat_here;
    if (gems > 0 && !capturing && !engaged) {
        const int adist = altar_field.get(pos);
        const bool threatened = (now - last_enemy_round < 12 && (gems >= 12 || adist <= 6))
            || (now - team_enemy_round < 40 && gems >= 16);
        if (adist <= 3 + static_cast<int>(gems) / 4 || gems >= 24 || threatened || now > 1900) {
            act = 8;
            go_bank();
            return;
        }
    }
    act = 12;

    // Farming.
    distance_field from_me;
    bitboard reach;
    bool have_field = false;
    auto rechoose = [&] {
        if (!have_field) {
            bitboard walls = static_walls;
            from_me.compute(walls, square(pos, 0), reach);
            have_field = true;
        }
        choose_target(from_me, reach);
    };
    PROF_BEGIN(3)
    if (target < 0 || (now % 8 == 0 && !engaged && !capturing)) rechoose();
    PROF_END(3)
    if (all_spawns.is_set(pos) && !capturing && !engaged) {
        // Never block a camp group's respawn by idling on its spawn tiles.
        bool needed = false;
        if (target >= 0 && groups[target].spawns.is_set(pos)) {
            const auto st = group_handles()[target].state();
            needed = !std::holds_alternative<cambc::map::camp_group_respawning>(st);
        }
        if (!needed) step_to(square(pos, 2) & ~all_spawns & ~static_walls);
    }
    PROF_BEGIN(4)
    for (int tries = 0; tries < 4 && target >= 0 && !fight_group(); ++tries) {
        if (target >= 0) banned_until[target] = std::max(banned_until[target], now + 1);
        rechoose();
    }
    PROF_END(4)
    // No move, attack or cast while farming: count towards giving the group up.
    const bool idle_turn = target >= 0 && !moved && !cast_lock && me.can_attack()
        && !(target >= 0 && groups[target].spawns.is_set(pos));
    stuck_turns = idle_turn ? stuck_turns + 1 : 0;
    if (stuck_turns > 12 && wait_turns == 0) {
        banned_until[target] = now + 60;
        target = -1;
        stuck_turns = 0;
    }
    write_claim(target);
    act = target < 0 ? 12 : (!me.can_attack() && me.attack_range() > 1) || (me.attack_range() == 1 && !me.can_attack()) ? 10 : moved ? 9 : 11;
    if (now % 25 == 0) std::cout << "DBG r" << now << " " << cambc::to_string(me.champion_name()) << " idx=" << my_index << " tgt=" << target << " pos=" << pos.x << "," << pos.y << " hp=" << hp << " wait=" << wait_turns << " moved=" << moved << " why=" << dbg_reason << std::endl;
    if (target < 0 && gems > 0) go_bank();
    if (target < 0 && gems == 0 && !moved) {
        // Nothing to farm (e.g. every camp is tied to a monolith): gather in the middle as a
        // team; a single kill decides a 0-0 game on lifetime gold.
        const vec2 mid{W / 2, H / 2};
        step_to(square(mid, 3) & ~static_walls);
    }
}
