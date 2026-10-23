// Champion decisions for a big-game hunter: farm cheap groups early, buy Jungle and Monster emblems,
// then gather the whole team at golems and the monolith and burst them down together.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
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
constexpr int kStageRange = 7;      // hold this far from a big camp until the team arrives
constexpr int kMaxWait = 30;        // turns to wait for allies before engaging anyway
constexpr float kBigBonus = 1.4f;   // score multiplier for golem and monolith groups

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

bool is_big(cambc::camp_type kind) {
    return kind == cambc::camp_type::golem || kind == cambc::camp_type::monolith;
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
    bool big = false, monolith = false;
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
                if (is_big(camp.kind())) info.big = true;
                if (camp.kind() == cambc::camp_type::monolith) info.monolith = true;
            }
            list.push_back(std::move(info));
        }
        return list;
    }();
    return result;
}

int team_size() {
    static const int size = [] {
        try {
            return static_cast<int>(cambc::draft::team_size());
        } catch (...) {
            return 4;
        }
    }();
    return std::clamp(size, 1, 6);
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

// Run one engine action; a rejected action counts as a failed one.
template<class Action>
bool try_action(Action&& action) {
    try {
        return action();
    } catch (const cambc::error&) {
        return false;
    }
}

bool has_jungle(const cambc::champion& me) {
    const auto counts = me.item_count();
    return counts[cambc::item_type::jungle_emblem_of_focus] > 0 || counts[cambc::item_type::jungle_emblem_of_health] > 0;
}

bool has_monster(const cambc::champion& me) {
    const auto counts = me.item_count();
    return counts[cambc::item_type::monster_emblem_of_focus] > 0 || counts[cambc::item_type::monster_emblem_of_strength] > 0;
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
    move_failed = true;
    return false;
}

// Jungle emblem first (camp damage taken), then Monster (camp damage dealt), then Power shards.
void Bot::buy_items() {
    auto& me = cambc::self();
    using I = cambc::item_type;
    const bool focus = is_spell_caster(me.champion_name());
    const auto jungle_shard = focus ? I::jungle_shard_of_focus : I::jungle_shard_of_health;
    const auto jungle_emblem = focus ? I::jungle_emblem_of_focus : I::jungle_emblem_of_health;
    const auto monster_shard = focus ? I::monster_shard_of_focus : I::monster_shard_of_strength;
    const auto monster_emblem = focus ? I::monster_emblem_of_focus : I::monster_emblem_of_strength;
    const auto power_dmg = focus ? I::power_shard_of_focus : I::power_shard_of_strength;
    const auto power_health = I::power_shard_of_health;

    for (int guard = 0; guard < 64; ++guard) {
        if (me.gold_count() < jungle_shard.cost()) return;
        const auto counts = me.item_count();
        auto item = power_dmg;
        if (counts[jungle_emblem] == 0) item = jungle_shard;
        else if (counts[monster_emblem] == 0) item = monster_shard;
        else if (counts[power_health] < counts[power_dmg]) item = power_health;
        if (!me.can_buy(item, 1)) return;
        me.buy(item, 1);
    }
}

// Pick the best visible target in basic attack range: enemies first, then camps.
std::optional<std::variant<cambc::champion, cambc::camp>> Bot::pick_target(bool allow_wake_big) {
    auto& me = cambc::self();
    const auto attack_tiles = me.attack_tiles();
    std::optional<cambc::champion> best_enemy;
    for (auto tile : (cambc::vision::champion_tiles(team::enemy) & attack_tiles).positions()) {
        if (!cambc::vision::has_champion(tile)) continue;
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
            // Big camps are only woken when the team has gathered.
            if (!is_awake && is_big(camp.kind()) && !allow_wake_big) continue;
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
    if (me.can_attack() && me.attack_tiles().is_set(tile))
        try_action([&] { me.attack(target); return true; });

    for (const auto& ability : me.abilities()) {
        if (!ability.info() || !ability.is_ready()) continue;
        if (!still_there(target, tile)) return;
        const bool exclusive = exclusive_with_movement(ability);
        if (exclusive && moved) continue;
        if (ability.is_attack_reset() || ability.is_dash() || ability.is_heal()) {
            continue;
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
            if (me.health() < me.max_health() * 0.85f && ability.can_target_self()) {
                if (try_action([&] { return ability.use(); })) { if (exclusive) cast_exclusive = true; return; }
            }
        }
    }
}

bool Bot::fight_best_target(bool allow_wake_big) {
    const auto target = pick_target(allow_wake_big);
    if (!target) return false;
    try {
        std::visit([this](const auto& t) { fight(t); }, *target);
    } catch (const cambc::error&) {}
    return true;
}

// Willow heals: patch the most injured visible ally in range.
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

bool Bot::bank_gems(bool force) {
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
    if (force || gems >= kBankGems || late || (nearest <= 4 && gems >= 5)
        || cambc::global_submitted_gems(team::ally) + gems >= 1000)
        banking = true;
    if (!banking) return false;

    cambc::bitboard goals;
    goals.clear();
    for (auto altar : cambc::map::altars().positions()) {
        if (cambc::vision::has_altar_lock(altar)) continue;
        if (distance(me.position(), altar) <= cambc::constants::submit_gems_range) {
            try_action([&] { me.submit_gems(altar); return true; });
            return true;
        }
        goals |= tiles_around(altar, cambc::constants::submit_gems_range);
    }
    return move_towards_closest(goals);
}

// Score every camp group by reward per turn; golems and the monolith use the whole team's damage.
int Bot::choose_group() {
    auto& me = cambc::self();
    const auto& list = groups();
    const int round = cambc::round_number();
    if (skip_until.size() != list.size()) skip_until.assign(list.size(), 0);

    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(terrain_blocked(), tiles_around(me.position(), 0), visited);

    int helpers = 1;
    for (auto tile : cambc::vision::champion_tiles(team::ally).positions())
        if (tile != me.position() && distance(tile, me.position()) <= 8) ++helpers;
    float dps = std::max(10.0f, me.strength() + me.focus() * 0.2f);
    if (has_monster(me)) dps *= 1.1f;
    const float taken = has_jungle(me) ? 0.7f : 1.0f;
    const int team = team_size();
    const int team_target = read_team_target();

    const auto visible = cambc::vision::visible_tiles();
    int best = -1;
    float best_score = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& info = list[i];
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

        // Big camps are fought by the whole team; the camp hits one champion at a time.
        const float k = info.big ? static_cast<float>(team) : static_cast<float>(helpers);
        const float clear_turns = info.health / (dps * k);
        const float damage = info.strength * taken * clear_turns;
        const float pool = me.max_health() * k;
        const float value = info.group.gem_value() * 3.0f + info.group.gold_value();
        float score = value / (dist + clear_turns + (info.big ? 20.0f : 8.0f));
        if (info.big) {
            // Commit only when the team can absorb the fight, and never start the monolith late.
            if (damage > pool * 0.6f || (info.monolith && round > 1700)) continue;
            score *= kBigBonus;
        } else if (damage > me.health() * 0.75f) {
            score *= 0.05f;
        }
        if (static_cast<int>(i) == target_group) score *= 1.3f;
        if (static_cast<int>(i) == team_target) score *= 1.5f;
        if (best < 0 || score > best_score) { best = static_cast<int>(i); best_score = score; }
    }
    return best;
}

// Attack a camp in range, otherwise walk to the chosen group; gather at big camps before waking them.
void Bot::farm_camps() {
    auto& me = cambc::self();
    const int group = choose_group();
    if (group != target_group) { stuck_turns = 0; last_distance = 1 << 20; wait_turns = 0; }
    target_group = group;

    const bool big = group >= 0 && groups()[group].big;
    bool ready = true;
    if (big) {
        const auto& spawns = groups()[group].spawns;
        int allies = 1;
        for (auto tile : cambc::vision::champion_tiles(team::ally).positions())
            if (tile != me.position() && distance(tile, me.position()) <= 5) ++allies;
        const int need = std::min(team_size(), 3);
        // Someone already woke it: join in rather than wait.
        bool engaged = false;
        for (auto tile : cambc::vision::awake_camp_tiles().positions())
            if (spawns.is_set(tile)) engaged = true;
        ready = engaged || allies >= need || wait_turns > kMaxWait;
    }

    if (fight_best_target(ready)) { stuck_turns = 0; return; }

    if (group < 0) {
        move_towards_closest(cambc::map::spawn_region(team::ally));
        return;
    }
    write_team_target(group);

    const auto& spawns = groups()[group].spawns;
    if (spawns.is_set(me.position())) { stuck_turns = 0; return; } // capturing

    int dist = std::numeric_limits<int>::max();
    for (auto tile : spawns.positions()) dist = std::min(dist, distance(me.position(), tile));

    if (big && !ready && dist <= kStageRange) {
        // A camp kill destroys carried gems: tribute first, then hold at the staging distance.
        if (me.gem_count() >= 5 && bank_gems(true)) return;
        ++wait_turns;
        stuck_turns = 0;
        return;
    }

    if (dist < last_distance) stuck_turns = 0;
    else if (++stuck_turns > 12) {
        skip_until[group] = cambc::round_number() + 50;
        stuck_turns = 0;
    }
    last_distance = std::min(last_distance, dist);

    if (move_towards_closest(spawns))
        fight_best_target(ready);
}

void Bot::play_turn() {
    moved = false;
    move_failed = false;
    cast_exclusive = false;
    auto& me = cambc::self();

    const bool at_spawn = me.is_alive() && cambc::map::spawn_region(team::ally).is_set(me.position());
    if (!me.is_alive() || at_spawn) buy_items();

    if (!me.is_alive() || me.is_recalling() || me.is_submitting_gems() || me.is_teleporting())
        return;

    if (!me.can_move() && !me.can_attack()) return;

    if (at_spawn && me.health() < me.max_health() * 0.9f && threats().empty()) return;

    // Recall when badly hurt; step out of range first so damage does not cancel it.
    if (!at_spawn && me.health() < me.max_health() * kRecallHealth) {
        const auto danger = threats();
        if (danger.empty()) {
            try_action([&] { me.recall(); return true; });
            return;
        }
        if (move_away_from(danger)) return;
        fight_best_target(false);
        return;
    }

    // Recall to shop when enough gold has piled up and nothing is fighting us.
    if (!at_spawn && me.gold_count() >= kShopGold && me.gem_count() < 10 && threats().empty()
        && distance(me.position(), cambc::map::spawn_region(team::ally).positions()[0]) > 6) {
        try_action([&] { me.recall(); return true; });
        return;
    }

    heal_allies();

    if (!(cambc::vision::champion_tiles(team::enemy) & me.attack_tiles()).is_empty()) {
        fight_best_target(false);
        return;
    }

    if (bank_gems(false)) return;
    farm_camps();
}
