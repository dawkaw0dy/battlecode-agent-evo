// Spell pack: the team farms one shared camp group together and spends every ability with intent.
// A spell engine scores each ready slot against every legal target (heal the most wounded ally,
// interrupt channelling enemies, finish kills, place area spells on clusters) and casts the best one;
// dashes speed up travel, shields absorb camp damage, attack resets follow our own basic attack.
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

constexpr unsigned kBankGems = 20;      // carried gems that justify a trip to an altar
constexpr unsigned kShopGold = 150;     // gold that justifies a recall to shop
constexpr float kRecallHealth = 0.35f;
constexpr float kDisengageHealth = 0.7f; // below this, flee fights where enemies outnumber us
constexpr float kPackBonus = 1.5f;   // score multiplier for the team's shared camp group
constexpr float kStickiness = 2.0f;  // score multiplier for the group we are already heading to

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

// Focus multiplier of a damage, stun, heal or shield ability (0 for pure stuns, resets and dashes).
float ability_modifier(const cambc::ability& ability) {
    const auto& info = ability.info();
    if (!info) return 0;
    return std::visit([](const auto& value) -> float {
        if constexpr (requires { value.modifier; }) return value.modifier;
        else return 0.0f;
    }, *info);
}

int ability_radius(const cambc::ability& ability) {
    const auto& info = ability.info();
    if (!info) return 0;
    return std::visit([](const auto& value) -> int {
        if constexpr (requires { value.radius; }) return static_cast<int>(value.radius);
        else return 0;
    }, *info);
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

// Shared memory byte 1: the pack's camp group + 1 (0: none).
int read_pack_group() {
    std::uint8_t value = 0;
    cambc::shared_memory.read(1, std::span{&value, 1});
    return static_cast<int>(value) - 1;
}

void write_pack_group(int group) {
    const auto value = static_cast<std::uint8_t>(group < 0 || group > 254 ? 0 : group + 1);
    cambc::shared_memory.write(1, std::span{&value, 1});
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

bool altar_enemy_locked(vec2 altar) {
    if (!cambc::vision::in_vision(altar) || !cambc::vision::has_altar_lock(altar)) return false;
    try {
        return !cambc::vision::altar_lock_at(altar).is_ally();
    } catch (const cambc::error&) {
        return false;
    }
}

// Camps a spell may hit without pulling a fight we did not choose.
bool camp_fair_game(const cambc::camp& camp, vec2 tile, const cambc::bitboard& awake, float strength) {
    if (awake.is_set(tile)) return true;
    const auto kind = camp.kind();
    if (kind == cambc::camp_type::monolith) return false;
    if (kind == cambc::camp_type::golem && strength < 90) return false;
    return true;
}

} // namespace

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

// After walking, spend a ready walking-compatible dash on two more tiles toward the goal.
bool Bot::dash_towards(cambc::bitboard goals) {
    auto& me = cambc::self();
    if (cast_done || cast_exclusive || move_failed || !me.can_cast() || goals.is_set(me.position())) return false;
    if (!threats().empty()) return false;
    for (const auto& ability : me.abilities()) {
        if (!ability.info() || !ability.is_dash() || !ability.is_ready()) continue;
        if (exclusive_with_movement(ability) || !ability.can_target_ground()) continue;

        const auto blocked = blocked_tiles();
        auto reachable_goals = goals & (~blocked | cambc::vision::camp_tiles());
        if (reachable_goals.is_empty()) return false;
        cambc::distance_field field;
        cambc::bitboard visited;
        field.compute_to(blocked & ~reachable_goals, reachable_goals, tiles_around(me.position(), 0), visited);
        if (!visited.is_set(me.position())) return false;

        cambc::bitboard range;
        try { range = ability.valid_tiles(); } catch (const cambc::error&) { return false; }
        auto best = me.position();
        // Only land where walking would take at least two more steps to match.
        int best_value = static_cast<int>(field.get(best)) - 1;
        for (auto tile : range.positions()) {
            if (blocked.is_set(tile) || !visited.is_set(tile) || distance(tile, me.position()) < 2) continue;
            const int value = static_cast<int>(field.get(tile));
            if (value < best_value) { best_value = value; best = tile; }
        }
        if (best == me.position()) return false;
        if (try_action([&] { return ability.use(best); })) {
            cast_done = true;
            return true;
        }
        return false;
    }
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

// Casters build Focus (their spells are their damage); everyone starts with a Treasure emblem.
void Bot::buy_items() {
    auto& me = cambc::self();
    using I = cambc::item_type;
    const bool focus = is_spell_caster(me.champion_name());
    const auto treasure_shard = focus ? I::treasure_shard_of_focus : I::treasure_shard_of_strength;
    const auto treasure_emblem = focus ? I::treasure_emblem_of_focus : I::treasure_emblem_of_strength;
    const auto jungle_shard = I::jungle_shard_of_health;
    const auto jungle_emblem = I::jungle_emblem_of_health;
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

// Best visible target in basic attack range: gem carriers and wounded champions first, then camps.
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
            float score = enemy.health() - 10.0f * static_cast<float>(enemy.gem_count());
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
            if (!camp_fair_game(camp, tile, awake, me.strength())) continue;
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

// Score every ready ability against every legal target and cast the best one.
bool Bot::cast_spells() {
    auto& me = cambc::self();
    if (cast_done || !me.can_cast()) return false;

    struct Choice {
        float score = 0;
        int slot = -1;
        int mode = 0; // 0 self, 1 ground, 2 champion, 3 camp
        vec2 tile{0, 0};
        std::optional<cambc::champion> champion;
        std::optional<cambc::camp> camp;
        bool exclusive = false;
    };
    Choice best;

    const vec2 here = me.position();
    const float focus = me.focus();
    const auto awake = cambc::vision::awake_camp_tiles();
    const auto enemy_tiles = cambc::vision::champion_tiles(team::enemy);
    // Sleeping camps are fair game only in our chosen group or in basic attack range.
    auto spell_camps = cambc::vision::camp_tiles() & (awake | me.attack_tiles());
    if (target_group >= 0 && target_group < static_cast<int>(groups().size()))
        spell_camps |= cambc::vision::camp_tiles() & groups()[target_group].spawns;
    const auto camp_tiles = spell_camps;
    bool enemy_close = false;
    for (auto tile : enemy_tiles.positions())
        if (distance(tile, here) <= 4) enemy_close = true;
    bool camp_close = false;
    for (auto tile : awake.positions())
        if (distance(tile, here) <= 4) camp_close = true;

    auto consider = [&](const Choice& c) { if (c.score > best.score) best = c; };

    const auto abilities = me.abilities();
    for (std::size_t slot = 0; slot < abilities.size(); ++slot) {
        const auto& ability = abilities[slot];
        if (!ability.info() || !ability.is_ready()) continue;
        if (ability.is_attack_reset() || ability.is_dash()) continue;
        const bool exclusive = exclusive_with_movement(ability);
        if (exclusive && moved) continue;
        const float amount = ability_modifier(ability) * focus;

        if (ability.is_shield()) {
            if (!ability.can_target_self()) continue;
            if ((enemy_close || camp_close) && me.health() < me.max_health() * 0.92f) {
                Choice c; c.score = 70; c.slot = static_cast<int>(slot); c.mode = 0; c.exclusive = exclusive;
                consider(c);
            }
            continue;
        }

        cambc::bitboard range;
        try { range = ability.valid_tiles(); } catch (const cambc::error&) { continue; }

        if (ability.is_heal()) {
            if (!ability.can_target_allies()) continue;
            for (auto tile : (cambc::vision::champion_tiles(team::ally) & range).positions()) {
                if (tile == here || !cambc::vision::has_champion(tile)) continue;
                try {
                    const auto ally = cambc::vision::champion_at(tile);
                    if (ally.health() <= 0) continue;
                    const float missing = ally.max_health() - ally.health();
                    if (missing < amount * 0.6f && ally.health() > ally.max_health() * 0.4f) continue;
                    Choice c; c.score = 90 + missing / 10; c.slot = static_cast<int>(slot); c.mode = 2;
                    c.tile = tile; c.champion = ally; c.exclusive = exclusive;
                    consider(c);
                } catch (const cambc::error&) {}
            }
            continue;
        }

        if (!(ability.is_damage() || ability.is_cc() || ability.is_aoe_damage())) continue;
        const bool stuns = ability.is_cc();

        if (ability.is_aoe_damage() && ability.can_target_ground()) {
            const int radius = std::max(1, ability_radius(ability));
            // Centre the area on a unit we can see; count enemies and fair camps inside it.
            for (auto center : ((enemy_tiles | camp_tiles) & range).positions()) {
                float score = 0;
                bool spoils = false;
                for (auto tile : (enemy_tiles & tiles_around(center, radius)).positions()) {
                    try {
                        const auto enemy = cambc::vision::champion_at(tile);
                        if (enemy.health() <= 0) continue;
                        score += 60 + 2.0f * static_cast<float>(enemy.gem_count());
                        if (enemy.health() <= amount) score += 250;
                    } catch (const cambc::error&) {}
                }
                for (auto tile : (camp_tiles & tiles_around(center, radius)).positions()) {
                    try {
                        const auto camp = cambc::vision::camp_at(tile);
                        if (camp.health() <= 0) continue;
                        if (!camp_fair_game(camp, tile, awake, me.strength())) { spoils = true; break; }
                        score += 40;
                    } catch (const cambc::error&) {}
                }
                if (spoils || score <= 0) continue;
                Choice c; c.score = score; c.slot = static_cast<int>(slot); c.mode = 1; c.tile = center;
                c.exclusive = exclusive;
                consider(c);
            }
            continue;
        }

        if (ability.can_target_enemies()) {
            for (auto tile : (enemy_tiles & range).positions()) {
                try {
                    const auto enemy = cambc::vision::champion_at(tile);
                    if (enemy.health() <= 0) continue;
                    const bool channelling = enemy.is_submitting_gems() || enemy.is_recalling();
                    if (amount <= 0 && !(channelling || distance(tile, here) <= 3)) continue;
                    float score = 100 + 2.0f * static_cast<float>(enemy.gem_count()) + amount / 10;
                    if (amount > 0 && enemy.health() <= amount) score += 250;
                    if (stuns && channelling) score += 200;
                    Choice c; c.score = score; c.slot = static_cast<int>(slot); c.mode = 2; c.tile = tile;
                    c.champion = enemy; c.exclusive = exclusive;
                    consider(c);
                } catch (const cambc::error&) {}
            }
        }
        if (ability.can_target_camps()) {
            for (auto tile : (camp_tiles & range).positions()) {
                try {
                    const auto camp = cambc::vision::camp_at(tile);
                    if (camp.health() <= 0) continue;
                    if (!camp_fair_game(camp, tile, awake, me.strength())) continue;
                    float score;
                    if (amount > 0) {
                        score = 40 + amount / 20;
                        if (camp.health() <= amount) score += 20;
                    } else {
                        // A pure stun only pays against an awake camp that hits hard.
                        if (!awake.is_set(tile) || camp_cost(camp.kind()).strength < 60) continue;
                        score = 25;
                    }
                    Choice c; c.score = score; c.slot = static_cast<int>(slot); c.mode = 3; c.tile = tile;
                    c.camp = camp; c.exclusive = exclusive;
                    consider(c);
                } catch (const cambc::error&) {}
            }
        }
    }

    if (best.slot < 0) return false;
    const auto& ability = abilities[best.slot];
    bool ok = false;
    switch (best.mode) {
    case 0: ok = try_action([&] { return ability.use(); }); break;
    case 1: ok = try_action([&] { return ability.use(best.tile); }); break;
    case 2: ok = best.champion && try_action([&] { return ability.use(*best.champion); }); break;
    case 3: ok = best.camp && try_action([&] { return ability.use(*best.camp); }); break;
    }
    if (!ok) return false;
    cast_done = true;
    if (best.exclusive) cast_exclusive = true;
    return true;
}

template<class Target>
void Bot::fight(const Target& target) {
    auto& me = cambc::self();
    if (!target.is_visible()) return;

    const vec2 tile = target.position();
    if (!attacked && me.can_attack() && me.attack_tiles().is_set(tile) && still_there(target, tile))
        attacked = try_action([&] { me.attack(target); return true; });

    // Spells first; an attack reset only uses the cast when nothing better was available.
    cast_spells();
    if (!attacked || cast_done || cast_exclusive) return;
    for (const auto& ability : me.abilities()) {
        if (!ability.info() || !ability.is_attack_reset() || !ability.is_ready()) continue;
        if (!ability.can_target_self()) continue;
        if (!still_there(target, tile)) return;
        if (!try_action([&] { return ability.use(); })) continue;
        cast_done = true;
        if (still_there(target, tile) && me.can_attack() && me.attack_tiles().is_set(tile))
            try_action([&] { me.attack(target); return true; });
        return;
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

// Tribute at altars free of enemy locks; clear an enemy lock if every altar is locked.
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
        if (!altar_enemy_locked(altar)) nearest = std::min(nearest, distance(me.position(), altar));

    const bool late = cambc::round_number() > 1800;
    if (gems >= kBankGems || late || (nearest <= 4 && gems >= 5)
        || cambc::global_submitted_gems(team::ally) + gems >= 1000)
        banking = true;
    if (!banking) return false;

    const int range = cambc::constants::submit_gems_range;
    cambc::bitboard goals, enemy_locked;
    goals.clear();
    enemy_locked.clear();

    // Already beside an enemy-locked altar: break the lock here rather than walk to another one.
    cambc::bitboard near_lock;
    near_lock.clear();
    for (auto altar : altars)
        if (distance(me.position(), altar) <= 2 && altar_enemy_locked(altar)) near_lock.set(altar, true);
    if (!near_lock.is_empty()) {
        if (clear_enemy_lock(near_lock)) return true;
        cambc::bitboard beside;
        beside.clear();
        for (auto altar : near_lock.positions()) beside |= tiles_around(altar, std::max(1, me.attack_range()));
        if (move_towards_closest(beside)) return true;
        if (beside.is_set(me.position()) && moved) return true; // in reach: strike next turn
    }

    for (auto altar : altars) {
        if (altar_enemy_locked(altar)) { enemy_locked.set(altar, true); continue; }
        if (distance(me.position(), altar) <= range && cambc::vision::in_vision(altar)) {
            if (try_action([&] { me.submit_gems(altar); return true; })) return true;
        }
        goals |= tiles_around(altar, range);
    }
    if (move_towards_closest(goals)) {
        dash_towards(goals);
        return true;
    }

    if (enemy_locked.is_empty()) return false;
    if (clear_enemy_lock(enemy_locked)) return true;
    cambc::bitboard near_locked;
    near_locked.clear();
    for (auto altar : enemy_locked.positions()) near_locked |= tiles_around(altar, 1);
    return move_towards_closest(near_locked);
}

// Score camp groups by reward per turn of travel and clearing; the pack's group counts double.
int Bot::choose_group() {
    auto& me = cambc::self();
    const auto& list = groups();
    const int round = cambc::round_number();
    if (skip_until.size() != list.size()) skip_until.assign(list.size(), 0);

    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(terrain_blocked(), tiles_around(me.position(), 0), visited);

    const int pack = read_pack_group();
    int helpers = 1;
    for (auto tile : cambc::vision::champion_tiles(team::ally).positions())
        if (tile != me.position() && distance(tile, me.position()) <= 8) ++helpers;
    const float dps = std::max(10.0f, me.strength() + me.focus() * 0.25f);

    const auto visible = cambc::vision::visible_tiles();
    int best = -1;
    float best_score = 0;
    bool pack_valid = false;
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
        if (static_cast<int>(i) == pack) { score *= kPackBonus; pack_valid = true; }
        if (static_cast<int>(i) == target_group) score *= kStickiness;
        if (best < 0 || score > best_score) { best = static_cast<int>(i); best_score = score; }
    }
    // Only replace the pack's group when it is gone for us (captured, respawning or unreachable).
    if (!pack_valid && best != pack) write_pack_group(best);
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
    if (spawns.is_set(me.position())) { stuck_turns = 0; cast_spells(); return; } // capturing

    int dist = std::numeric_limits<int>::max();
    for (auto tile : spawns.positions()) dist = std::min(dist, distance(me.position(), tile));
    if (dist < last_distance) stuck_turns = 0;
    else if (++stuck_turns > 12) {
        skip_until[group] = cambc::round_number() + 50;
        stuck_turns = 0;
    }
    last_distance = std::min(last_distance, dist);

    if (move_towards_closest(spawns)) {
        if (!fight_best_target()) {
            if (dist > 4) dash_towards(spawns);
            cast_spells();
        }
    }
}

void Bot::play_turn() {
    moved = false;
    move_failed = false;
    attacked = false;
    cast_exclusive = false;
    cast_done = false;
    auto& me = cambc::self();

    const bool at_spawn = me.is_alive() && cambc::map::spawn_region(team::ally).is_set(me.position());
    if (!me.is_alive() || at_spawn) buy_items();

    if (!me.is_alive() || me.is_recalling() || me.is_submitting_gems() || me.is_teleporting())
        return;

    if (!me.can_move() && !me.can_attack()) return;

    if (at_spawn && me.health() < me.max_health() * 0.9f && threats().empty()) {
        cast_spells();
        return;
    }

    if (!at_spawn && me.health() < me.max_health() * kRecallHealth) {
        const auto danger = threats();
        if (danger.empty()) {
            me.recall();
            return;
        }
        // Heal, shield or stun a pursuer on the way out, then step away.
        cast_spells();
        if (move_away_from(danger)) return;
        fight_best_target();
        return;
    }

    if (!at_spawn && me.gold_count() >= kShopGold && me.gem_count() < 10 && threats().empty()
        && distance(me.position(), cambc::map::spawn_region(team::ally).positions()[0]) > 6) {
        me.recall();
        return;
    }

    // Spend the cast before walking: exclusive spells need a turn without a step.
    cast_spells();

    // Outnumbered and hurt: disengage toward the pack instead of trading.
    if (!at_spawn && me.health() < me.max_health() * kDisengageHealth) {
        int enemies = 0, allies = 1;
        std::vector<vec2> hunters;
        for (auto tile : cambc::vision::champion_tiles(team::enemy).positions())
            if (distance(tile, me.position()) <= 5) { ++enemies; hunters.push_back(tile); }
        for (auto tile : cambc::vision::champion_tiles(team::ally).positions())
            if (tile != me.position() && distance(tile, me.position()) <= 5) ++allies;
        if (enemies > allies) {
            if (move_away_from(hunters)) return;
        }
    }

    if (!(cambc::vision::champion_tiles(team::enemy) & me.attack_tiles()).is_empty()) {
        fight_best_target();
        return;
    }

    if (bank_gems()) return;
    farm_camps();
}
