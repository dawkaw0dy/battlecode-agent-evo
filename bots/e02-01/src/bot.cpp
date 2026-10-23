// Champion decisions for a scaling economy: farm the richest reachable camp groups as a team,
// turn gold into Treasure, Jungle and Power shards, and bank gems in batches.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;

namespace {

constexpr unsigned kBankGems = 20;  // carried gems that justify a trip to an altar
constexpr unsigned kShopGold = 150; // gold that justifies a recall to shop
constexpr float kRecallHealth = 0.35f;
constexpr float kChaseOwnHealth = 0.6f;   // chase hurt enemies only above this share of our health
constexpr float kChaseEnemyHealth = 0.5f;  // enemies below this share of their health are chased
constexpr int kChaseRange = 4;

int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

cambc::bitboard tiles_around(vec2 position, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    tiles.draw_chebyshev(position, radius);
    return tiles;
}

cambc::bitboard terrain_blocked() {
    return cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(team::enemy);
}

// Combine terrain and visible occupants using native bitboard operations.
cambc::bitboard blocked_tiles() {
    auto blocked = terrain_blocked() | cambc::vision::champion_tiles()
        | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    blocked.set(cambc::self().position(), false);
    return blocked;
}

bool is_spell_caster(cambc::champion_type type) {
    using T = cambc::champion_type;
    return type == T::stellar || type == T::prism || type == T::cassi;
}

bool exclusive_with_movement(const cambc::ability& ability) {
    const auto& info = ability.info();
    if (!info) return false;
    return std::visit([](const auto& value) { return value.exclusive_with_movement; }, *info);
}

// Camp health and strength after patch 100.
struct CampCost { float health, strength; };
CampCost camp_cost(cambc::camp_type kind) {
    using C = cambc::camp_type;
    switch (kind) {
    case C::wolf: return {1350, 20};
    case C::elk: return {2400, 60};
    case C::gem_cluster: return {120, 0};
    case C::gold_cluster: return {120, 0};
    case C::monolith: return {20000, 200};
    case C::golem: return {5000, 120};
    }
    return {20000, 200};
}

struct GroupInfo {
    cambc::map::camp_group group;
    cambc::bitboard spawns;
    float health = 0, strength = 0;
    bool has_monolith = false;
};

const std::vector<GroupInfo>& groups() {
    static const auto result = [] {
        std::vector<GroupInfo> list;
        for (const auto& group : cambc::map::camp_groups()) {
            GroupInfo info{group, group.spawn_tiles()};
            for (const auto& camp : group.camps()) {
                const auto cost = camp_cost(camp.kind());
                info.health += cost.health;
                info.strength += cost.strength;
                if (camp.kind() == cambc::camp_type::monolith) info.has_monolith = true;
            }
            list.push_back(std::move(info));
        }
        return list;
    }();
    return result;
}

// Team target: little-endian u16 at shared-memory offset 0, group index + 1.
int read_team_target() {
    std::uint16_t value = 0;
    cambc::shared_memory.read(0, std::span{&value, 1});
    return static_cast<int>(value) - 1;
}

void write_team_target(int group) {
    std::uint16_t value = static_cast<std::uint16_t>(group + 1);
    cambc::shared_memory.write(0, std::span{&value, 1});
}

// Run one engine action. The engine knows of units our vision masks miss (tiles revealed to the team
// earlier this round, units dying mid-turn); it rejects those actions without spending anything,
// so a rejection is treated like a blocked action rather than an error.
template<class Action>
bool try_action(Action&& action) {
    try {
        return action();
    } catch (const cambc::error&) {
        return false;
    }
}

} // namespace

bool Bot::move_towards_closest(cambc::bitboard goals) {
    auto& me = cambc::self();
    if (cast_exclusive || move_failed || !me.can_move() || goals.is_set(me.position()))
        return false;

    // Only free goals or occupied camp tiles can seed the field.
    const auto blocked = blocked_tiles();
    goals &= ~blocked | cambc::vision::camp_tiles();
    if (goals.is_empty())
        return false;

    const auto path_walls = blocked & ~goals;
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute_to(path_walls, goals, tiles_around(me.position(), 0), visited);

    auto step = me.position();
    // Unreached and blocked tiles read as distance zero: only consider free tiles the search reached.
    for (auto tile : blocked.reachable_tiles(step, me.base_stats().base_move_speed).positions())
        if (!blocked.is_set(tile) && visited.is_set(tile) && field.get(tile) < field.get(step)) step = tile;

    if (step == me.position()) return false;
    if (try_action([&] { return me.move_absolute(step); })) {
        moved = true;
        return true;
    }
    // A hidden unit blocks the step; it is now known, so do not walk again this turn.
    move_failed = true;
    return false;
}

// Step to the free neighbouring tile farthest from the given threats.
bool Bot::move_away_from(const std::vector<vec2>& threats) {
    auto& me = cambc::self();
    if (threats.empty() || cast_exclusive || move_failed || !me.can_move()) return false;
    const auto blocked = blocked_tiles();
    auto score = [&](vec2 p) {
        int best = std::numeric_limits<int>::max();
        for (auto t : threats) best = std::min(best, distance(p, t));
        return best;
    };
    auto step = me.position();
    int best = score(step);
    for (auto tile : blocked.reachable_tiles(step, me.base_stats().base_move_speed).positions()) {
        if (blocked.is_set(tile)) continue;
        const int s = score(tile);
        if (s > best) { best = s; step = tile; }
    }
    if (step == me.position()) return false;
    if (try_action([&] { return me.move_absolute(step); })) {
        moved = true;
        return true;
    }
    // A hidden unit blocks the step; it is now known, so do not walk again this turn.
    move_failed = true;
    return false;
}

// Treasure emblem first (gold), then Jungle (camp damage taken), then Power shards (six for five).
void Bot::buy_items() {
    auto& me = cambc::self();
    using I = cambc::item_type;
    const bool focus = is_spell_caster(me.champion_name());
    const auto treasure_shard = focus ? I::treasure_shard_of_focus : I::treasure_shard_of_strength;
    const auto treasure_emblem = focus ? I::treasure_emblem_of_focus : I::treasure_emblem_of_strength;
    const auto jungle_shard = focus ? I::jungle_shard_of_focus : I::jungle_shard_of_health;
    const auto jungle_emblem = focus ? I::jungle_emblem_of_focus : I::jungle_emblem_of_health;
    const auto power_strength = focus ? I::power_shard_of_focus : I::power_shard_of_strength;

    for (int guard = 0; guard < 64; ++guard) {
        if (me.gold_count() < treasure_shard.cost()) return;
        const auto counts = me.item_count();
        auto item = power_strength;
        if (counts[treasure_emblem] == 0) item = treasure_shard;
        else if (counts[jungle_emblem] == 0) item = jungle_shard;
        if (!me.can_buy(item, 1)) return;
        me.buy(item, 1);
    }
}

// Pick the best visible target in basic attack range: enemies first, then camps.
std::optional<std::variant<cambc::champion, cambc::camp>> Bot::pick_target() {
    auto& me = cambc::self();
    const auto attack_tiles = me.attack_tiles();
    std::optional<cambc::champion> best_enemy;
    for (auto tile : (cambc::vision::champion_tiles(team::enemy) & attack_tiles).positions()) {
        if (!cambc::vision::has_champion(tile)) continue;
        // Stale handles (unit died or moved this turn) raise engine errors 97/98: skip the unit.
        try {
            const auto enemy = cambc::vision::champion_at(tile);
            if (enemy.health() <= 0) continue;
            if (!best_enemy || enemy.health() < best_enemy->health()) best_enemy = enemy;
        } catch (const cambc::error&) {}
    }
    if (best_enemy) return *best_enemy;

    std::optional<cambc::camp> best_camp;
    bool best_awake = false;
    const auto awake = cambc::vision::awake_camp_tiles();
    for (auto tile : (cambc::vision::camp_tiles() & attack_tiles).positions()) {
        if (!cambc::vision::has_camp(tile)) continue;
        try {
            const auto camp = cambc::vision::camp_at(tile);
            if (camp.health() <= 0) continue;
            const bool is_awake = awake.is_set(tile);
            const auto kind = camp.kind();
            // Never wake a camp we cannot finish.
            if (!is_awake && (kind == cambc::camp_type::monolith
                || (kind == cambc::camp_type::golem && me.strength() < 90))) continue;
            // Focus fire: awake camps first, then lowest health.
            if (!best_camp || (is_awake && !best_awake)
                || (is_awake == best_awake && camp.health() < best_camp->health())) {
                best_camp = camp;
                best_awake = is_awake;
            }
        } catch (const cambc::error&) {}
    }
    if (best_camp) return *best_camp;
    return std::nullopt;
}

// True while the target still stands, visible, on the tile it occupied when the fight started.
template<class Target>
bool still_there(const Target& target, vec2 tile) {
    if (!cambc::vision::in_vision(tile)) return false;
    if constexpr (std::is_same_v<Target, cambc::camp>) {
        return cambc::vision::has_camp(tile) && cambc::vision::camp_at(tile) == target;
    } else {
        return cambc::vision::has_champion(tile) && cambc::vision::champion_at(tile) == target;
    }
}

template<class Target>
void Bot::fight(const Target& target) {
    auto& me = cambc::self();
    if (!target.is_visible()) return;
    
    const vec2 tile = target.position();
    if (me.can_attack() && me.attack_tiles().is_set(tile)) {
        attacked = try_action([&] { me.attack(target); return true; });
    }

    // Attack reset right after our own basic attack, then strike again (resets throw otherwise).
    if (attacked && !cast_exclusive) {
        for (const auto& ability : me.abilities()) {
            if (!ability.info() || !ability.is_attack_reset() || !ability.is_ready()) continue;
            if (!ability.can_target_self()) continue;
            if (!still_there(target, tile)) return;
            if (!try_action([&] { return ability.use(); })) continue;
            if (still_there(target, tile) && me.can_attack() && me.attack_tiles().is_set(tile))
                try_action([&] { me.attack(target); return true; });
            return; // one cast per turn
        }
    }

    for (const auto& ability : me.abilities()) {
        if (!ability.info() || !ability.is_ready()) continue;
        if (!still_there(target, tile)) return;
        const bool exclusive = exclusive_with_movement(ability);
        if (exclusive && moved) continue;
        if (ability.is_attack_reset() || ability.is_dash() || ability.is_heal()) {
            continue; // resets are rejected when the attack is still available; dashes and heals are not fight spells
        } else if (ability.is_damage() || ability.is_cc() || ability.is_aoe_damage()) {
            if (!ability.valid_tiles().is_set(tile)) continue;
            if (ability.is_aoe_damage()) {
                if (!ability.can_target_ground()) continue;
                if (try_action([&] { return ability.use(tile); })) { if (exclusive) cast_exclusive = true; return; }
            } else {
                if constexpr (std::is_same_v<Target, cambc::camp>) {
                    if (!ability.can_target_camps()) continue;
                } else {
                    if (!ability.can_target_enemies()) continue;
                }
                if (try_action([&] { return ability.use(target); })) { if (exclusive) cast_exclusive = true; return; }
            }
        } else if (ability.is_shield()) {
            if (me.health() < me.max_health() * 0.8f && ability.can_target_self()) {
                if (ability.use()) { if (exclusive) cast_exclusive = true; return; }
            }
        }
    }
}

bool Bot::fight_best_target() {
    const auto target = pick_target();
    if (!target) return false;
    try {
        std::visit([this](const auto& t) { fight(t); }, *target);
    } catch (const cambc::error&) {} // target died or left vision mid-fight
    return true;
}

// Willow-style heals: patch the most injured visible ally in range.
void Bot::heal_allies() {
    
    auto& me = cambc::self();
    for (const auto& ability : me.abilities()) {
        if (!ability.info() || !ability.is_heal() || !ability.is_ready() || !ability.can_target_allies()) continue;
        if (exclusive_with_movement(ability) && moved) continue;
        const auto tiles = ability.valid_tiles();
        std::optional<cambc::champion> best;
        for (auto tile : (cambc::vision::champion_tiles(team::ally) & tiles).positions()) {
            if (tile == me.position() || !cambc::vision::has_champion(tile)) continue;
            try {
                const auto ally = cambc::vision::champion_at(tile);
                if (ally.max_health() - ally.health() < 150) continue;
                if (!best || ally.health() / ally.max_health() < best->health() / best->max_health()) best = ally;
            } catch (const cambc::error&) {}
        }
        if (best && try_action([&] { return ability.use(*best); })) {
            if (exclusive_with_movement(ability)) cast_exclusive = true;
        }
        return;
    }
}

std::vector<vec2> Bot::threats() {
    auto& me = cambc::self();
    std::vector<vec2> result;
    for (auto tile : cambc::vision::awake_camp_tiles().positions())
        if (distance(me.position(), tile) <= 6) result.push_back(tile);
    for (auto tile : cambc::vision::champion_tiles(team::enemy).positions())
        if (distance(me.position(), tile) <= 5) result.push_back(tile);
    return result;
}

bool Bot::bank_gems() {
    auto& me = cambc::self();
    const unsigned gems = me.gem_count();
    if (gems == 0) {
        banking = false;
        return false;
    }

    int nearest = std::numeric_limits<int>::max();
    for (auto altar : cambc::map::altars().positions())
        if (!cambc::vision::has_altar_lock(altar)) nearest = std::min(nearest, distance(me.position(), altar));

    const bool late = cambc::round_number() > 1800;
    // Once committed, keep heading to the altar so the decision cannot flicker each step.
    if (gems >= kBankGems || late || (nearest <= 4 && gems >= 5)
        || cambc::global_submitted_gems(team::ally) + gems >= 1000)
        banking = true;
    if (!banking) return false;

    cambc::bitboard goals;
    goals.clear();
    for (auto altar : cambc::map::altars().positions()) {
        if (cambc::vision::has_altar_lock(altar)) continue;
        if (distance(me.position(), altar) <= cambc::constants::submit_gems_range) {
            me.submit_gems(altar);
            return true;
        }
        goals |= tiles_around(altar, cambc::constants::submit_gems_range);
    }
    return move_towards_closest(goals);
}

// Score every camp group by reward per turn of travel and clearing; skip groups we cannot survive.
int Bot::choose_group() {
    
    auto& me = cambc::self();
    const auto& list = groups();
    const int round = cambc::round_number();
    if (skip_until.size() != list.size()) skip_until.assign(list.size(), 0);

    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(terrain_blocked(), tiles_around(me.position(), 0), visited);

    // Allies close enough to share the fight.
    int helpers = 1;
    for (auto tile : cambc::vision::champion_tiles(team::ally).positions())
        if (tile != me.position() && distance(tile, me.position()) <= 8) ++helpers;
    const float dps = std::max(10.0f, me.strength() + me.focus() * 0.2f);
    const bool jungle = me.item_count()[is_spell_caster(me.champion_name())
        ? cambc::item_type::jungle_emblem_of_focus : cambc::item_type::jungle_emblem_of_health] > 0;
    const int team_target = read_team_target();

    const auto visible = cambc::vision::visible_tiles();
    int best = -1;
    float best_score = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& info = list[i];
        if (info.has_monolith) continue;
        // Remember respawn timers so the goal does not flicker when the group leaves vision.
        if (!(info.spawns & visible).is_empty()) {
            try {
                const auto state = info.group.state();
                if (const auto* respawning = std::get_if<cambc::map::camp_group_respawning>(&state))
                    skip_until[i] = std::max(skip_until[i], round + std::max(respawning->timer, 1));
            } catch (const cambc::error&) {}
        }
        if (skip_until[i] > round) continue;

        int dist = std::numeric_limits<int>::max();
        for (auto tile : info.spawns.positions())
            if (visited.is_set(tile)) dist = std::min<int>(dist, field.get(tile));
        if (dist == std::numeric_limits<int>::max()) continue;

        const float k = static_cast<float>(helpers);
        const float clear_turns = info.health / (dps * k);
        float damage = info.strength * clear_turns / k;
        if (jungle) damage *= 0.7f;
        const float value = info.group.gem_value() * 3.0f + info.group.gold_value();
        float score = value / (dist + clear_turns + 8.0f);
        // Dangerous groups stay possible, but only as a last resort over idling.
        if (damage > me.health() * 0.75f) score *= 0.05f;
        if (static_cast<int>(i) == target_group) score *= 1.3f;
        if (static_cast<int>(i) == team_target) score *= 1.5f;
        if (best < 0 || score > best_score) { best = static_cast<int>(i); best_score = score; }
    }
    return best;
}

// Attack a camp already in range, otherwise walk to the chosen camp group.
void Bot::farm_camps() {
    if (fight_best_target()) { stuck_turns = 0; return; }

    const int group = choose_group();
    if (group != target_group) { stuck_turns = 0; last_distance = 1 << 20; }
    target_group = group;
    if (group < 0) {
        // Nothing worth doing: wait on spawn so shopping and healing keep up.
        move_towards_closest(cambc::map::spawn_region(team::ally));
        return;
    }
    write_team_target(group);

    auto& me = cambc::self();
    const auto& spawns = groups()[group].spawns;
    if (spawns.is_set(me.position())) { stuck_turns = 0; return; } // capturing

    int dist = std::numeric_limits<int>::max();
    for (auto tile : spawns.positions()) dist = std::min(dist, distance(me.position(), tile));
    if (dist < last_distance) stuck_turns = 0;
    else if (++stuck_turns > 12) {
        // No progress: skip the group for a while.
        skip_until[group] = cambc::round_number() + 50;
        stuck_turns = 0;
    }
    last_distance = std::min(last_distance, dist);

    if (move_towards_closest(spawns))
        fight_best_target();
}

void Bot::play_turn() {
    moved = false;
    move_failed = false;
    attacked = false;
    cast_exclusive = false;
    auto& me = cambc::self();

    const bool at_spawn = me.is_alive() && cambc::map::spawn_region(team::ally).is_set(me.position());
    if (!me.is_alive() || at_spawn) buy_items();

    if (!me.is_alive() || me.is_recalling() || me.is_submitting_gems() || me.is_teleporting())
        return;

    // Stunned or otherwise unable to act.
    if (!me.can_move() && !me.can_attack()) return;

    // Heal up on spawn before heading out again.
    if (at_spawn && me.health() < me.max_health() * 0.9f && threats().empty()) return;

    // Recall when badly hurt; step out of range first so damage does not cancel it.
    if (!at_spawn && me.health() < me.max_health() * kRecallHealth) {
        const auto danger = threats();
        if (danger.empty()) {
            me.recall();
            return;
        }
        if (move_away_from(danger)) return;
        fight_best_target();
        return;
    }

    // Recall to shop when enough gold has piled up and nothing is fighting us.
    if (!at_spawn && me.gold_count() >= kShopGold && me.gem_count() < 10 && threats().empty()
        && distance(me.position(), cambc::map::spawn_region(team::ally).positions()[0]) > 6) {
        me.recall();
        return;
    }

    heal_allies();

    // Enemies in range are fought before banking or farming.
    if (!(cambc::vision::champion_tiles(team::enemy) & me.attack_tiles()).is_empty()) {
        fight_best_target();
        return;
    }

    // Chase a nearby, badly hurt enemy while we are healthy: kills pay 100 gold plus their gems.
    if (me.health() > me.max_health() * kChaseOwnHealth) {
        cambc::bitboard prey;
        prey.clear();
        for (auto tile : cambc::vision::champion_tiles(team::enemy).positions()) {
            if (distance(me.position(), tile) > kChaseRange || !cambc::vision::has_champion(tile)) continue;
            try {
                const auto enemy = cambc::vision::champion_at(tile);
                if (enemy.health() > 0 && enemy.health() < enemy.max_health() * kChaseEnemyHealth)
                    prey.set(tile, true);
            } catch (const cambc::error&) {}
        }
        if (!prey.is_empty()) {
            cambc::bitboard goals;
            goals.clear();
            for (auto tile : prey.positions())
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy) {
                        const vec2 p{tile.x + dx, tile.y + dy};
                        if ((dx || dy) && cambc::map::is_in_bounds(p)) goals.set(p, true);
                    }
            if (move_towards_closest(goals)) {
                fight_best_target();
                return;
            }
        }
    }

    if (bank_gems()) return;
    farm_camps();
}
