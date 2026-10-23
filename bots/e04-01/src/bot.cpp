// Altar control: every champion carries an altar lock and locks the altars it passes, so enemy
// tributes are blocked while our own team still tributes there. One warden keeps re-locking
// altars the enemy has cleared; the others farm camps and bank gems at our locked altars.
#include <cambc.h>
#include "bot.h"
#include "plan.h"
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

constexpr float kRecallHealth = 0.35f;
constexpr int kLockRange = 2;           // altar locks reach altars within two tiles

int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

cambc::bitboard tiles_around(vec2 position, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    for (int dx = -radius; dx <= radius; ++dx)
        for (int dy = -radius; dy <= radius; ++dy) {
            const vec2 p{position.x + dx, position.y + dy};
            if (cambc::map::is_in_bounds(p)) tiles.set(p, true);
        }
    return tiles;
}

cambc::bitboard terrain_blocked() {
    return cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(team::enemy);
}

cambc::bitboard blocked_tiles() {
    auto blocked = terrain_blocked() | cambc::vision::champion_tiles()
        | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    blocked.set(cambc::self().position(), false);
    return blocked;
}

const std::vector<vec2>& altar_list() {
    static const auto list = cambc::map::altars().positions();
    return list;
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

// Shared memory: byte 0 holds the warden's champion type + 1; bytes 4..7 a mask of altars
// some ally saw without our lock (bit set) or with our lock (bit cleared).
std::uint8_t read_warden() {
    std::uint8_t value = 0;
    cambc::shared_memory.read(0, std::span{&value, 1});
    return value;
}

std::uint32_t read_open_altars() {
    std::uint32_t value = 0;
    cambc::shared_memory.read(4, std::span{&value, 1});
    return value;
}

void write_open_altars(std::uint32_t value) {
    cambc::shared_memory.write(4, std::span{&value, 1});
}

// The engine rejects actions on units our masks miss without spending anything: treat as blocked.
template<class Action>
bool try_action(Action&& action) {
    try {
        return action();
    } catch (const cambc::error&) {
        return false;
    }
}

enum class AltarOwner { none, ally, enemy, unknown };

AltarOwner altar_owner(vec2 altar) {
    if (!cambc::vision::in_vision(altar)) return AltarOwner::unknown;
    if (!cambc::vision::has_altar_lock(altar)) return AltarOwner::none;
    try {
        return cambc::vision::altar_lock_at(altar).is_ally() ? AltarOwner::ally : AltarOwner::enemy;
    } catch (const cambc::error&) {
        return AltarOwner::unknown;
    }
}

} // namespace

bool Bot::is_warden() {
    if (!plan().warden) return false;
    const auto mine = static_cast<std::uint8_t>(static_cast<int>(cambc::self().champion_name()) + 1);
    const auto current = read_warden();
    if (current == 0) {
        cambc::shared_memory.write(0, std::span{&mine, 1});
        return true;
    }
    return current == mine;
}

bool Bot::carries_lock() {
    return cambc::self().consumables()[cambc::consumable_type::altar_lock].count() > 0;
}

// Remember which altars hold our lock; share the ones that need one.
void Bot::observe_altars() {
    const auto& altars = altar_list();
    if (lock_seen.size() != altars.size()) lock_seen.assign(altars.size(), -1000);
    std::uint32_t open = read_open_altars();
    const std::uint32_t before = open;
    const int round = cambc::round_number();
    for (std::size_t i = 0; i < altars.size(); ++i) {
        const auto owner = altar_owner(altars[i]);
        if (owner == AltarOwner::unknown) continue;
        const std::uint32_t bit = i < 32 ? (1u << i) : 0u;
        if (owner == AltarOwner::ally) {
            lock_seen[i] = round;
            open &= ~bit;
        } else {
            lock_seen[i] = -1000;
            open |= bit;
        }
    }
    if (open != before) write_open_altars(open);
}

// Lock any reachable altar that is not ours; prefer altars closer to the enemy spawn.
bool Bot::place_locks() {
    auto& me = cambc::self();
    if (!carries_lock()) return false;
    const auto enemy_spawn = cambc::map::spawn_region(team::enemy).positions();
    const vec2 enemy_home = enemy_spawn.empty() ? me.position() : enemy_spawn[0];
    const auto& altars = altar_list();
    int best = -1;
    for (std::size_t i = 0; i < altars.size(); ++i) {
        if (distance(me.position(), altars[i]) > kLockRange) continue;
        if (altar_owner(altars[i]) != AltarOwner::none) continue;
        if (best < 0 || distance(altars[i], enemy_home) < distance(altars[best], enemy_home)) best = static_cast<int>(i);
    }
    if (best < 0) return false;
    const auto lock = me.consumables()[cambc::consumable_type::altar_lock];
    if (!try_action([&] { return lock.use(altars[best]); })) return false;
    lock_seen[best] = cambc::round_number();
    std::uint32_t open = read_open_altars();
    if (best < 32) open &= ~(1u << best);
    write_open_altars(open);
    return true;
}

// Basic-attack a visible enemy lock on one of the given altars in range.
bool Bot::clear_enemy_lock(cambc::bitboard altars) {
    auto& me = cambc::self();
    if (attacked || !me.can_attack()) return false;
    const auto attack_tiles = me.attack_tiles();
    for (const auto& lock : cambc::vision::altar_locks(team::enemy)) {
        if (!altars.is_set(lock.position()) || !attack_tiles.is_set(lock.position())) continue;
        if (try_action([&] { me.attack(lock); return true; })) {
            attacked = true;
            return true;
        }
    }
    return false;
}

// The warden walks to altars that are not known to hold our lock, clears enemy locks and locks them.
bool Bot::warden_duty() {
    auto& me = cambc::self();
    const auto& altars = altar_list();
    const int round = cambc::round_number();
    const std::uint32_t open = read_open_altars();

    cambc::bitboard targets;
    targets.clear();
    for (std::size_t i = 0; i < altars.size(); ++i) {
        const bool shared_open = i < 32 && (open & (1u << i));
        if (shared_open || lock_seen[i] < round - plan().relock_div4 * 4) targets.set(altars[i], true);
    }
    if (targets.is_empty()) return false;

    if (!carries_lock()) {
        // Fetch a lock from spawn when we can afford one and nothing threatens the recall.
        if (me.gold_count() >= cambc::consumable_type::altar_lock.cost() && threats().empty()) {
            me.recall();
            return true;
        }
        return false;
    }

    // Clear enemy locks on target altars in range, then lock them on the next turn.
    if (clear_enemy_lock(targets)) return true;

    cambc::bitboard goals;
    goals.clear();
    for (auto altar : targets.positions()) {
        // Melee champions must stand next to an enemy lock to clear it; locking needs range 2.
        const int reach = altar_owner(altar) == AltarOwner::enemy ? std::min(kLockRange, me.attack_range()) : kLockRange;
        goals |= tiles_around(altar, std::max(1, reach));
    }
    if (goals.is_set(me.position())) {
        // In place but the altar is not lockable yet (enemy lock being cleared): keep hitting it.
        return fight_best_target();
    }
    if (move_towards_closest(goals)) {
        observe_altars();
        place_locks();
        return true;
    }
    return false;
}

bool Bot::move_towards_closest(cambc::bitboard goals) {
    auto& me = cambc::self();
    if (cast_exclusive || move_failed || !me.can_move() || goals.is_set(me.position()))
        return false;

    const auto blocked = blocked_tiles();
    goals &= ~blocked | cambc::vision::camp_tiles();
    if (goals.is_empty())
        return false;

    const auto path_walls = blocked & ~goals;
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute_to(path_walls, goals, tiles_around(me.position(), 0), visited);

    auto step = me.position();
    // Unreached and blocked tiles read as zero: only free tiles the search reached qualify.
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

// An altar lock first, then Treasure and Durability emblems, then Power shards.
void Bot::buy_items() {
    auto& me = cambc::self();
    const auto lock = cambc::consumable_type::altar_lock;
    if (!carries_lock() && me.can_buy(lock)) try_action([&] { me.buy(lock); return true; });

    using I = cambc::item_type;
    const bool focus = is_spell_caster(me.champion_name());
    const auto treasure_shard = focus ? I::treasure_shard_of_focus : I::treasure_shard_of_strength;
    const auto treasure_emblem = focus ? I::treasure_emblem_of_focus : I::treasure_emblem_of_strength;
    const auto jungle_shard = focus ? I::jungle_shard_of_focus : I::jungle_shard_of_health;
    const auto jungle_emblem = focus ? I::jungle_emblem_of_focus : I::jungle_emblem_of_health;
    const auto power = focus ? I::power_shard_of_focus : I::power_shard_of_strength;

    for (int guard = 0; guard < 64; ++guard) {
        if (me.gold_count() < treasure_shard.cost()) return;
        const auto counts = me.item_count();
        auto item = power;
        if (counts[treasure_emblem] == 0) item = treasure_shard;
        else if (counts[jungle_emblem] == 0) item = jungle_shard;
        if (!me.can_buy(item, 1)) return;
        me.buy(item, 1);
    }
}

// Best visible target in basic attack range: lock-breakers and gem carriers first, then camps.
std::optional<std::variant<cambc::champion, cambc::camp>> Bot::pick_target() {
    auto& me = cambc::self();
    const auto attack_tiles = me.attack_tiles();
    std::optional<cambc::champion> best_enemy;
    float best_score = 0;
    for (auto tile : (cambc::vision::champion_tiles(team::enemy) & attack_tiles).positions()) {
        if (!cambc::vision::has_champion(tile)) continue;
        try {
            const auto enemy = cambc::vision::champion_at(tile);
            if (enemy.health() <= 0) continue;
            float score = enemy.health();
            // Punish champions standing at an altar (clearing a lock or tributing).
            for (auto altar : altar_list())
                if (distance(tile, altar) <= 1) { score -= 300; break; }
            if (!best_enemy || score < best_score) { best_enemy = enemy; best_score = score; }
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
            if (!is_awake && (kind == cambc::camp_type::monolith
                || (kind == cambc::camp_type::golem && me.strength() < 90))) continue;
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
    if (!attacked && me.can_attack() && me.attack_tiles().is_set(tile))
        attacked = try_action([&] { me.attack(target); return true; });

    // Attack reset only right after our own basic attack, then strike again.
    if (attacked && !cast_exclusive) {
        for (const auto& ability : me.abilities()) {
            if (!ability.info() || !ability.is_attack_reset() || !ability.is_ready()) continue;
            if (!ability.can_target_self()) continue;
            if (!still_there(target, tile)) return;
            if (!try_action([&] { return ability.use(); })) continue;
            if (still_there(target, tile) && me.can_attack() && me.attack_tiles().is_set(tile))
                try_action([&] { me.attack(target); return true; });
            return;
        }
    }

    for (const auto& ability : me.abilities()) {
        if (!ability.info() || !ability.is_ready()) continue;
        if (!still_there(target, tile)) return;
        const bool exclusive = exclusive_with_movement(ability);
        if (exclusive && moved) continue;
        if (ability.is_attack_reset() || ability.is_dash() || ability.is_heal()) continue;
        if (ability.is_damage() || ability.is_cc() || ability.is_aoe_damage()) {
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
                if (try_action([&] { return ability.use(); })) { if (exclusive) cast_exclusive = true; return; }
            }
        }
    }
}

bool Bot::fight_best_target() {
    const auto target = pick_target();
    if (!target) return false;
    try {
        std::visit([this](const auto& t) { fight(t); }, *target);
    } catch (const cambc::error&) {}
    return true;
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

// Tribute at altars free of enemy locks (our own locks do not block us); clear an enemy lock if none.
bool Bot::bank_gems() {
    auto& me = cambc::self();
    const unsigned gems = me.gem_count();
    if (gems == 0) {
        banking = false;
        return false;
    }

    const auto& altars = altar_list();
    int nearest = std::numeric_limits<int>::max();
    for (auto altar : altars)
        if (altar_owner(altar) != AltarOwner::enemy) nearest = std::min(nearest, distance(me.position(), altar));

    const bool late = cambc::round_number() > 1800;
    if (gems >= plan().bank_gems || late || (nearest <= 4 && gems >= 5)
        || cambc::global_submitted_gems(team::ally) + gems >= 1000)
        banking = true;
    if (!banking) return false;

    const int range = cambc::constants::submit_gems_range;
    cambc::bitboard goals, enemy_locked;
    goals.clear();
    enemy_locked.clear();
    for (auto altar : altars) {
        const auto owner = altar_owner(altar);
        if (owner == AltarOwner::enemy) { enemy_locked.set(altar, true); continue; }
        if (distance(me.position(), altar) <= range && owner != AltarOwner::unknown) {
            if (try_action([&] { me.submit_gems(altar); return true; })) return true;
        }
        goals |= tiles_around(altar, range);
    }
    if (move_towards_closest(goals)) return true;

    // Every reachable altar is locked by the enemy: break the nearest lock.
    if (enemy_locked.is_empty()) return false;
    if (clear_enemy_lock(enemy_locked)) return true;
    cambc::bitboard near_locked;
    near_locked.clear();
    for (auto altar : enemy_locked.positions()) near_locked |= tiles_around(altar, 1);
    return move_towards_closest(near_locked);
}

// Score camp groups by reward per turn of travel and clearing.
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
    const float dps = std::max(10.0f, me.strength() + me.focus() * 0.2f);

    const auto visible = cambc::vision::visible_tiles();
    int best = -1;
    float best_score = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& info = list[i];
        if (info.has_monolith) continue;
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
        const float damage = info.strength * clear_turns / k;
        const float value = info.group.gem_value() * 3.0f + info.group.gold_value();
        float score = value / (dist + clear_turns + 8.0f);
        if (damage > me.health() * 0.75f) score *= 0.05f;
        if (static_cast<int>(i) == target_group) score *= 1.3f;
        if (best < 0 || score > best_score) { best = static_cast<int>(i); best_score = score; }
    }
    return best;
}

void Bot::farm_camps() {
    if (fight_best_target()) { stuck_turns = 0; return; }

    const int group = choose_group();
    if (group != target_group) { stuck_turns = 0; last_distance = 1 << 20; }
    target_group = group;
    if (group < 0) {
        move_towards_closest(cambc::map::spawn_region(team::ally));
        return;
    }

    auto& me = cambc::self();
    const auto& spawns = groups()[group].spawns;
    if (spawns.is_set(me.position())) { stuck_turns = 0; return; } // capturing

    int dist = std::numeric_limits<int>::max();
    for (auto tile : spawns.positions()) dist = std::min(dist, distance(me.position(), tile));
    if (dist < last_distance) stuck_turns = 0;
    else if (++stuck_turns > 12) {
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

    const bool warden = is_warden();
    const bool at_spawn = me.is_alive() && cambc::map::spawn_region(team::ally).is_set(me.position());
    if (!me.is_alive() || at_spawn) buy_items();

    if (!me.is_alive() || me.is_recalling() || me.is_submitting_gems() || me.is_teleporting())
        return;

    observe_altars();
    // Locking does not use the move or attack allowance: do it whenever an altar is in reach.
    place_locks();

    if (!me.can_move() && !me.can_attack()) return;

    if (at_spawn && me.health() < me.max_health() * 0.9f && threats().empty()) return;

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

    if (!at_spawn && me.gold_count() >= plan().shop_div10 * 10u && me.gem_count() < 10 && threats().empty()
        && distance(me.position(), cambc::map::spawn_region(team::ally).positions()[0]) > 6) {
        me.recall();
        return;
    }

    if (!(cambc::vision::champion_tiles(team::enemy) & me.attack_tiles()).is_empty()) {
        fight_best_target();
        return;
    }

    if (bank_gems()) return;
    if (warden && me.gem_count() < plan().bank_gems && warden_duty()) return;
    farm_camps();
}
