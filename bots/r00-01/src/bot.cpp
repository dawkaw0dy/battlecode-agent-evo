// Champion decisions for a scaling-economy plan: farm gold, buy Treasure then Power shards, bank gems in batches.
#include <cambc.h>
#include "bot.h"
#include <array>
#include <cstdint>
#include <limits>
#include <algorithm>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;

namespace {

int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

cambc::bitboard tiles_around(vec2 position, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    tiles.draw_chebyshev(position, radius);
    return tiles;
}

// Combine terrain and visible occupants using native bitboard operations.
cambc::bitboard blocked_tiles() {
    auto blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy) | cambc::vision::champion_tiles()
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

} // namespace

bool Bot::move_towards_closest(cambc::bitboard goals) {
    auto& me = cambc::self();
    if (cast_exclusive || !me.can_move() || goals.is_set(me.position()))
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
    for (auto tile : blocked.reachable_tiles(step, me.base_stats().base_move_speed).positions())
        if (field.get(tile) < field.get(step)) step = tile;

    if (step == me.position()) return false;
    if (me.move_absolute(step)) {
        moved = true;
        return true;
    }
    return false;
}

// Step to the free neighbouring tile farthest from the given threats.
bool Bot::move_away_from(const std::vector<vec2>& threats) {
    auto& me = cambc::self();
    if (threats.empty() || cast_exclusive || !me.can_move()) return false;
    const auto blocked = blocked_tiles();
    auto score = [&](vec2 p) {
        int best = std::numeric_limits<int>::max();
        for (auto t : threats) best = std::min(best, distance(p, t));
        return best;
    };
    auto step = me.position();
    int best = score(step);
    for (auto tile : blocked.reachable_tiles(step, me.base_stats().base_move_speed).positions()) {
        const int s = score(tile);
        if (s > best) { best = s; step = tile; }
    }
    if (step == me.position()) return false;
    if (me.move_absolute(step)) {
        moved = true;
        return true;
    }
    return false;
}

void Bot::buy_items() {
    auto& me = cambc::self();
    using I = cambc::item_type;
    const bool focus = is_spell_caster(me.champion_name());
    const auto treasure_shard = focus ? I::treasure_shard_of_focus : I::treasure_shard_of_strength;
    const auto treasure_emblem = focus ? I::treasure_emblem_of_focus : I::treasure_emblem_of_strength;
    const auto power_shard = focus ? I::power_shard_of_focus : I::power_shard_of_strength;

    // First emblem is Treasure (+20% gold); afterwards Power gives six shards' worth per emblem.
    for (int guard = 0; guard < 64; ++guard) {
        if (me.gold_count() < treasure_shard.cost()) return;
        const auto counts = me.item_count();
        const auto item = counts[treasure_emblem] == 0 ? treasure_shard : power_shard;
        if (!me.can_buy(item, 1)) return;
        me.buy(item, 1);
    }
}

// Pick the best visible target in basic attack range: enemies first, then camps.
std::optional<std::variant<cambc::champion, cambc::camp>> Bot::pick_target() {
    auto& me = cambc::self();
    const auto attack_tiles = me.attack_tiles();
    std::optional<cambc::champion> best_enemy;
    for (const auto& enemy : cambc::vision::champions(team::enemy)) {
        if (!enemy.is_visible() || !enemy.is_alive() || enemy.health() <= 0) continue;
        if (!attack_tiles.is_set(enemy.position())) continue;
        if (!best_enemy || enemy.health() < best_enemy->health()) best_enemy = enemy;
    }
    if (best_enemy) return *best_enemy;

    std::optional<cambc::camp> best_camp;
    for (const auto& camp : cambc::vision::camps()) {
        if (!camp.is_visible() || camp.health() <= 0) continue;
        if (!attack_tiles.is_set(camp.position())) continue;
        if (!camp_allowed(camp.kind())) continue;
        // Focus fire: lowest health first, awake camps before sleeping ones.
        if (!best_camp || (camp.is_awake() && !best_camp->is_awake())
            || (camp.is_awake() == best_camp->is_awake() && camp.health() < best_camp->health()))
            best_camp = camp;
    }
    if (best_camp) return *best_camp;
    return std::nullopt;
}

bool Bot::camp_allowed(cambc::camp_type kind) const {
    auto& me = cambc::self();
    if (kind == cambc::camp_type::monolith) return false;
    if (kind == cambc::camp_type::golem) return me.strength() >= 85 || cambc::round_number() > 900;
    return true;
}

template<class Target>
void Bot::fight(const Target& target) {
    auto& me = cambc::self();
    if (!target.is_visible()) return;
    if (me.can_attack() && me.attack_tiles().is_set(target.position()))
        me.attack(target);

    for (const auto& ability : me.abilities()) {
        if (!ability.info() || !ability.is_ready()) continue;
        if (!target.is_visible() || target.health() <= 0) return;
        const bool exclusive = exclusive_with_movement(ability);
        if (exclusive && moved) continue;
        if (ability.is_attack_reset()) {
            if (!me.can_attack() && me.attack_tiles().is_set(target.position())) {
                if (ability.use()) {
                    if (exclusive) cast_exclusive = true;
                    if (me.can_attack() && target.is_visible() && me.attack_tiles().is_set(target.position()))
                        me.attack(target);
                }
                return;
            }
        } else if (ability.is_damage() || ability.is_cc() || ability.is_aoe_damage()) {
            if (!ability.valid_tiles().is_set(target.position())) continue;
            if (ability.is_aoe_damage()) {
                if (!ability.can_target_ground()) continue;
                if (ability.use(target.position())) { if (exclusive) cast_exclusive = true; return; }
            } else {
                if constexpr (std::is_same_v<Target, cambc::camp>) {
                    if (!ability.can_target_camps()) continue;
                } else {
                    if (!ability.can_target_enemies()) continue;
                }
                if (ability.use(target)) { if (exclusive) cast_exclusive = true; return; }
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
    std::visit([this](const auto& t) { fight(t); }, *target);
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
        for (const auto& ally : cambc::vision::champions(team::ally)) {
            if (ally == me || !ally.is_visible() || !ally.is_alive()) continue;
            if (!tiles.is_set(ally.position())) continue;
            if (ally.max_health() - ally.health() < 150) continue;
            if (!best || ally.health() / ally.max_health() < best->health() / best->max_health()) best = ally;
        }
        if (best && ability.use(*best)) {
            if (exclusive_with_movement(ability)) cast_exclusive = true;
        }
        return;
    }
}

std::vector<vec2> Bot::threats() {
    auto& me = cambc::self();
    std::vector<vec2> result;
    for (const auto& camp : cambc::vision::camps())
        if (camp.is_visible() && camp.is_awake() && distance(me.position(), camp.position()) <= 6)
            result.push_back(camp.position());
    for (const auto& enemy : cambc::vision::champions(team::enemy))
        if (enemy.is_visible() && enemy.is_alive() && distance(me.position(), enemy.position()) <= 5)
            result.push_back(enemy.position());
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

    const bool late = cambc::round_number() > 1850;
    // Once committed, keep heading to the altar so the decision cannot flicker each step.
    if (gems >= kBankGems || late || nearest <= 5 || cambc::global_submitted_gems(team::ally) + gems >= 1000)
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

// Attack a camp already in range, otherwise move toward the nearest allowed camp tiles.
void Bot::farm_camps() {
    if (fight_best_target()) return;

    static const auto camp_groups = [] {
        std::vector<std::pair<cambc::map::camp_group, cambc::bitboard>> groups;
        for (const auto& group : cambc::map::camp_groups()) groups.emplace_back(group, group.spawn_tiles());
        return groups;
    }();

    auto goals = cambc::bitboard{};
    goals.clear();
    const auto visible = cambc::vision::visible_tiles();
    const int round = cambc::round_number();
    if (skip_until.size() != camp_groups.size()) skip_until.assign(camp_groups.size(), 0);
    for (std::size_t i = 0; i < camp_groups.size(); ++i) {
        const auto& [group, spawns] = camp_groups[i];
        bool allowed = true;
        for (const auto& camp : group.camps())
            if (!camp_allowed(camp.kind())) allowed = false;
        if (!allowed) continue;
        // Remember respawn timers so the goal does not flicker when the group leaves vision.
        if (!(spawns & visible).is_empty()) {
            const auto state = group.state();
            if (const auto* respawning = std::get_if<cambc::map::camp_group_respawning>(&state))
                skip_until[i] = round + std::max(respawning->timer, 1);
            else
                skip_until[i] = 0;
        }
        if (skip_until[i] > round) continue;
        goals |= spawns;
    }

    if (move_towards_closest(goals))
        fight_best_target();
}

void Bot::play_turn() {
    moved = false;
    cast_exclusive = false;
    auto& me = cambc::self();

    const bool at_spawn = me.is_alive() && cambc::map::spawn_region(team::ally).is_set(me.position());
    if (!me.is_alive() || at_spawn) buy_items();

    if (!me.is_alive() || me.is_recalling() || me.is_submitting_gems() || me.is_teleporting())
        return;

    // Recall when badly hurt; step out of range first so damage does not cancel it.
    if (!at_spawn && me.health() < me.max_health() * 0.35f) {
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
    if (!at_spawn && me.gold_count() >= kShopGold && threats().empty()
        && distance(me.position(), cambc::map::spawn_region(team::ally).positions()[0]) > 6) {
        me.recall();
        return;
    }

    heal_allies();

    // Enemies in range are fought before banking or farming.
    for (const auto& enemy : cambc::vision::champions(team::enemy)) {
        if (enemy.is_visible() && enemy.is_alive() && me.attack_tiles().is_set(enemy.position())) {
            fight_best_target();
            return;
        }
    }

    if (bank_gems()) return;
    farm_camps();
}
