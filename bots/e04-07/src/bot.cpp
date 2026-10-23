// Champion decisions for a scaling economy: farm the richest reachable camp groups as a team,
// turn gold into Treasure, Jungle and Power shards, and bank gems in batches.
#include <cambc.h>
#include "bot.h"
#include "draft.h"
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

constexpr unsigned kBankGems = 30;  // carried gems that justify a trip to an altar
constexpr unsigned kShopGold = 150; // gold that justifies a recall to shop
constexpr float kRecallHealth = 0.35f;

int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

cambc::bitboard tiles_around(vec2 position, int radius) {
    // Set tiles one by one: draw_chebyshev near the map edge can raise engine error 5.
    cambc::bitboard tiles;
    tiles.clear();
    const int w = cambc::map::width(), h = cambc::map::height();
    for (int x = std::max(0, position.x - radius); x <= std::min(w - 1, position.x + radius); ++x)
        for (int y = std::max(0, position.y - radius); y <= std::min(h - 1, position.y + radius); ++y)
            tiles.set(vec2{x, y}, true);
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

// ---- Opening plan and altars -----------------------------------------------------------------
// The draft wrote the clearing route and the lock order (draft.h). Shared byte 6 holds a mask of
// altars (index < 8) last seen without our lock, byte 7 the team's progress along the route.

struct Plan {
    std::vector<int> route;  // camp group indices
    std::vector<int> altars; // altar indices, nearest first
};

const Plan& plan() {
    static const Plan result = [] {
        Plan p;
        const auto& memory = cambc::draft_memory;
        if (memory.size() < kPlanBytes || static_cast<std::uint8_t>(memory[kPlanMagic]) != kMagic) return p;
        const int groups_n = static_cast<int>(cambc::map::camp_groups().size());
        const int altars_n = static_cast<int>(cambc::map::altars().positions().size());
        const int route_len = std::min<int>(kRouteMax, static_cast<std::uint8_t>(memory[kPlanRouteLen]));
        for (int k = 0; k < route_len; ++k) {
            const int g = static_cast<std::uint8_t>(memory[kPlanRoute + k]) - 1;
            if (g >= 0 && g < groups_n) p.route.push_back(g);
        }
        const int altar_len = std::min<int>(kAltarMax, static_cast<std::uint8_t>(memory[kPlanAltarLen]));
        for (int k = 0; k < altar_len; ++k) {
            const int a = static_cast<std::uint8_t>(memory[kPlanAltars + k]) - 1;
            if (a >= 0 && a < altars_n) p.altars.push_back(a);
        }
        return p;
    }();
    return result;
}

const std::vector<vec2>& altar_list() {
    static const auto result = cambc::map::altars().positions();
    return result;
}

std::uint8_t read_byte(std::size_t offset) {
    std::uint8_t value = 0;
    cambc::shared_memory.read(offset, std::span{&value, 1});
    return value;
}

void write_byte(std::size_t offset, std::uint8_t value) {
    cambc::shared_memory.write(offset, std::span{&value, 1});
}

constexpr std::size_t kOpenAltars = 6;
constexpr std::size_t kRouteProgress = 7;

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

// An altar lock first, then Treasure emblem (gold), Jungle (camp damage taken), then Power shards.
void Bot::buy_items() {
    auto& me = cambc::self();
    using I = cambc::item_type;
    const auto lock = cambc::consumable_type::altar_lock;
    if (!carries_lock() && me.can_buy(lock)) try_action([&] { me.buy(lock); return true; });
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

// ---- Combat ----------------------------------------------------------------------------------
// Engagements are judged by a Lanchester-style balance: (sum HP x sum DPS) of our side against theirs,
// counting allies within 4 tiles and enemies (plus awake camps) within 5. Kills pay 100 gold plus the
// victim's gems, so enemies carrying gems are worth a riskier fight. Targets in range are chosen by
// kill-ability first, then reward per remaining health. Stuns go first, then damage spells, then an attack
// reset right after our own basic attack.

namespace {

constexpr int kAllyRadius = 4;
constexpr int kEnemyRadius = 5;
constexpr int kChaseTurns = 6;   // consecutive turns a chase may last
constexpr int kChaseRest = 20;   // rounds without chasing after a chase expires

float champion_dps(const cambc::champion& unit) {
    // Two spells per 8 turns at roughly 1.5-2x Focus each.
    return unit.strength() + unit.focus() * 0.3f;
}

struct Balance {
    float ours = 0, theirs = 0;  // HP x DPS of each side
    std::vector<vec2> enemies;   // visible living enemy champions within kEnemyRadius
    float ratio() const { return ours / std::max(1.0f, theirs); }
};

Balance assess_balance() {
    auto& me = cambc::self();
    const vec2 here = me.position();
    float our_hp = me.health(), our_dps = std::max(10.0f, me.strength() + me.focus() * 0.3f);
    for (auto tile : cambc::vision::champion_tiles(team::ally).positions()) {
        if (tile == here || distance(tile, here) > kAllyRadius || !cambc::vision::has_champion(tile)) continue;
        const auto ally = cambc::vision::champion_at(tile);
        if (ally.health() <= 0) continue;
        our_hp += ally.health();
        our_dps += champion_dps(ally);
    }
    Balance result;
    float their_hp = 0, their_dps = 0;
    for (auto tile : cambc::vision::champion_tiles(team::enemy).positions()) {
        if (distance(tile, here) > kEnemyRadius || !cambc::vision::has_champion(tile)) continue;
        const auto enemy = cambc::vision::champion_at(tile);
        if (enemy.health() <= 0) continue;
        their_hp += enemy.health();
        their_dps += champion_dps(enemy);
        result.enemies.push_back(tile);
    }
    // Awake camps hit whoever is nearest; count them as extra enemy damage only.
    for (auto tile : cambc::vision::awake_camp_tiles().positions()) {
        if (distance(tile, here) > 4 || !cambc::vision::has_camp(tile)) continue;
        their_dps += cambc::vision::camp_at(tile).strength() * 0.5f;
    }
    result.ours = our_hp * our_dps;
    result.theirs = std::max(1.0f, their_hp) * their_dps;
    if (their_dps <= 0) result.theirs = 0;
    return result;
}

bool has_ready_reset() {
    for (const auto& ability : cambc::self().abilities())
        if (ability.info() && ability.is_ready() && ability.is_attack_reset()) return true;
    return false;
}

} // namespace

namespace {

// Team focus target: shared-memory bytes 2..5 hold x, y and the round (u16) it was last hit.
constexpr std::size_t kFocusOffset = 2;

void write_focus(vec2 tile) {
    const auto round = cambc::round_number();
    std::array<std::uint8_t, 4> data{static_cast<std::uint8_t>(tile.x), static_cast<std::uint8_t>(tile.y),
        static_cast<std::uint8_t>(round & 0xff), static_cast<std::uint8_t>((round >> 8) & 0xff)};
    cambc::shared_memory.write(kFocusOffset, std::span{data});
}

std::optional<vec2> read_focus() {
    std::array<std::uint8_t, 4> data{};
    cambc::shared_memory.read(kFocusOffset, std::span{data});
    const int round = data[2] | (data[3] << 8);
    if (round == 0 || static_cast<int>(cambc::round_number()) - round > 2) return std::nullopt;
    return vec2{data[0], data[1]};
}

bool near_focus(vec2 tile) {
    const auto focus = read_focus();
    return focus && distance(*focus, tile) <= 1;
}

int stun_duration(const cambc::ability& ability) {
    const auto& info = ability.info();
    if (!info) return 0;
    return std::visit([](const auto& value) -> int {
        if constexpr (requires { value.duration; }) return value.duration;
        else return 0;
    }, *info);
}

float effect_radius(const cambc::ability& ability) {
    const auto& info = ability.info();
    if (!info) return 0;
    return std::visit([](const auto& value) -> float {
        if constexpr (requires { value.radius; }) return value.radius;
        else return 0.0f;
    }, *info);
}

bool stun_immune(const cambc::champion& unit) {
    return unit.champion_name() == cambc::champion_type::marcella;
}

bool dangerous_sleeper(cambc::camp_type kind) {
    return kind == cambc::camp_type::monolith || kind == cambc::camp_type::golem;
}

} // namespace

// The spell planner. Every ready slot is scored against every legal target in its cast range and the
// single best (slot, target) pair is cast:
// - enemy champions: share of their health removed x kill reward, a kill bonus, and a stun bonus for an
//   unstunned, stun-vulnerable target; the team focus target counts 1.5x;
// - awake camps: a little for damage, more for stunning a strong camp that would hit us this round;
//   camp casts wait while enemy champions are near, so stuns are saved for them;
// - area spells are centred on the tile that hits the most value and never on sleeping Golems or Monoliths;
// - heals go to the ally missing the most health, shields when hurt under threat.
// "Excludes walking" slots are skipped after a step, and before a step unless `stay` (we are fighting
// in place) or the target is an enemy champion.
bool Bot::cast_spells(bool stay) {
    if (cast_done) return false;
    auto& me = cambc::self();
    const vec2 here = me.position();

    bool enemies_near = false;
    for (auto tile : cambc::vision::champion_tiles(team::enemy).positions())
        if (distance(tile, here) <= 7 && cambc::vision::has_champion(tile)) enemies_near = true;
    const auto awake = cambc::vision::awake_camp_tiles();

    enum class Mode { self, unit_champion, unit_camp, ground };
    struct Option {
        const cambc::ability* ability = nullptr;
        Mode mode = Mode::self;
        vec2 tile;
        float score = 0;
    };
    Option best;
    auto consider = [&](const Option& option) { if (option.score > best.score) best = option; };

    // Value of hitting an enemy champion with `damage` and optionally stunning it.
    auto champion_value = [&](vec2 tile, float damage, bool stuns) -> float {
        if (!cambc::vision::has_champion(tile)) return 0;
        const auto enemy = cambc::vision::champion_at(tile);
        if (!enemy.is_visible()) return 0;
        const float health = enemy.health();
        if (health <= 0) return 0;
        const float reward = 200.0f + 3.0f * enemy.gem_count();
        float value = std::min(damage, health) / std::max(1.0f, enemy.max_health()) * reward;
        if (damage >= health) value += 400.0f + 3.0f * enemy.gem_count();
        if (stuns && !stun_immune(enemy) && !enemy.buffs().has_stun()) value += 120.0f;
        if (near_focus(tile)) value *= 1.5f;
        return value;
    };
    auto camp_value = [&](vec2 tile, float damage, bool stuns) -> float {
        if (!awake.is_set(tile) || !cambc::vision::has_camp(tile)) return 0;
        const auto camp = cambc::vision::camp_at(tile);
        const float health = camp.health();
        if (health <= 0) return 0;
        float value = std::min(damage, health) * 0.1f;
        if (stuns && !camp.buffs().has_stun()) value += camp.strength() * 0.5f;
        return value;
    };

    const auto abilities = me.abilities();
    for (const auto& ability : abilities) {
        if (!ability.info() || !ability.is_ready()) continue;
        if (ability.is_attack_reset() || ability.is_dash()) continue;
        const bool exclusive = exclusive_with_movement(ability);
        if (exclusive && moved) continue;

        if (ability.is_shield()) {
            if (ability.can_target_self() && me.health() < me.max_health() * 0.75f && !threats().empty())
                consider({&ability, Mode::self, here, 50.0f});
            continue;
        }
        const auto tiles = ability.valid_tiles();
        if (ability.is_heal()) {
            if (!ability.can_target_allies()) continue;
            const float heal = ability.heal_amount();
            for (auto tile : (cambc::vision::champion_tiles(team::ally) & tiles).positions()) {
                if (tile == here || !cambc::vision::has_champion(tile)) continue;
                const auto ally = cambc::vision::champion_at(tile);
                const float missing = ally.max_health() - ally.health();
                if (ally.health() <= 0 || missing < std::min(150.0f, heal * 0.6f)) continue;
                float score = std::min(missing, heal) * 0.3f;
                if (ally.health() < ally.max_health() * 0.4f) score *= 2.0f;
                consider({&ability, Mode::unit_champion, tile, score});
            }
            continue;
        }
        const bool stuns = ability.is_cc() || stun_duration(ability) > 0;
        const bool area = ability.is_aoe_damage();
        if (!(ability.is_damage() || stuns || area)) continue;
        const float damage = ability.damage_amount();
        const bool camp_ok = (stay || !exclusive) && !enemies_near;

        if (area) {
            if (!ability.can_target_ground()) continue;
            const int radius = std::max(0, static_cast<int>(effect_radius(ability)));
            cambc::bitboard centres = (cambc::vision::champion_tiles(team::enemy) | awake) & tiles;
            for (auto centre : centres.positions()) {
                float score = 0;
                bool hits_champion = false, unsafe = false;
                for (auto tile : (cambc::vision::champion_tiles(team::enemy) & tiles_around(centre, radius)).positions()) {
                    const float v = champion_value(tile, damage, stuns);
                    if (v > 0) hits_champion = true;
                    score += v;
                }
                for (auto tile : (cambc::vision::camp_tiles() & tiles_around(centre, radius)).positions()) {
                    if (!cambc::vision::has_camp(tile)) continue;
                    if (!awake.is_set(tile) && dangerous_sleeper(cambc::vision::camp_at(tile).kind())) unsafe = true;
                    score += camp_value(tile, damage, stuns);
                }
                if (unsafe) continue;
                if (!hits_champion && (!camp_ok)) continue;
                if (exclusive && !stay && !hits_champion) continue;
                consider({&ability, Mode::ground, centre, score});
            }
            continue;
        }
        if (ability.can_target_enemies()) {
            for (auto tile : (cambc::vision::champion_tiles(team::enemy) & tiles).positions())
                consider({&ability, Mode::unit_champion, tile, champion_value(tile, damage, stuns)});
        }
        if (ability.can_target_camps() && camp_ok) {
            for (auto tile : (awake & tiles).positions())
                consider({&ability, Mode::unit_camp, tile, camp_value(tile, damage, stuns)});
        }
    }

    if (!best.ability || best.score < 5.0f) return false;
    const auto& ability = *best.ability;
    bool ok = false;
    switch (best.mode) {
    case Mode::self:
        ok = try_action([&] { return ability.use(); });
        break;
    case Mode::ground:
        ok = try_action([&] { return ability.use(best.tile); });
        break;
    case Mode::unit_champion:
        if (cambc::vision::has_champion(best.tile)) {
            const auto unit = cambc::vision::champion_at(best.tile);
            if (unit.is_visible()) ok = try_action([&] { return ability.use(unit); });
        }
        break;
    case Mode::unit_camp:
        if (cambc::vision::has_camp(best.tile)) {
            const auto unit = cambc::vision::camp_at(best.tile);
            if (unit.is_visible()) ok = try_action([&] { return ability.use(unit); });
        }
        break;
    }
    if (!ok) return false;
    cast_done = true;
    if (exclusive_with_movement(ability)) cast_exclusive = true;
    if (best.mode == Mode::unit_champion && !ability.is_heal()) write_focus(best.tile);
    return true;
}

// Pick the best visible target in basic attack range: enemies first, then camps.
std::optional<std::variant<cambc::champion, cambc::camp>> Bot::pick_target() {
    auto& me = cambc::self();
    const auto attack_tiles = me.attack_tiles();
    const float hit = me.strength() * (has_ready_reset() ? 2.0f : 1.0f);
    std::optional<cambc::champion> best_enemy;
    float best_score = -1;
    for (auto tile : (cambc::vision::champion_tiles(team::enemy) & attack_tiles).positions()) {
        if (!cambc::vision::has_champion(tile)) continue;
        const auto enemy = cambc::vision::champion_at(tile);
        const float health = enemy.health();
        if (health <= 0) continue;
        float score = (100.0f + 3.0f * enemy.gem_count()) / std::max(1.0f, health) * 100.0f;
        if (health <= hit) score += 1000.0f;
        if (enemy.buffs().has_stun()) score *= 1.2f;
        if (near_focus(tile)) score *= 1.5f;
        if (score > best_score) { best_score = score; best_enemy = enemy; }
    }
    if (best_enemy) return *best_enemy;

    std::optional<cambc::camp> best_camp;
    bool best_awake = false;
    const auto awake = cambc::vision::awake_camp_tiles();
    for (auto tile : (cambc::vision::camp_tiles() & attack_tiles).positions()) {
        if (!cambc::vision::has_camp(tile)) continue;
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
    }
    if (best_camp) return *best_camp;
    return std::nullopt;
}

// True while the target still stands, visible, on the tile it occupied when the fight started.
template<class Target>
bool still_there(const Target& target, vec2 tile) {
    if (!target.is_visible() || !cambc::vision::in_vision(tile)) return false;
    if constexpr (std::is_same_v<Target, cambc::camp>) {
        return cambc::vision::has_camp(tile) && cambc::vision::camp_at(tile) == target
            && cambc::vision::camp_at(tile).health() > 0;
    } else {
        return cambc::vision::has_champion(tile) && cambc::vision::champion_at(tile) == target
            && cambc::vision::champion_at(tile).health() > 0;
    }
}

template<class Target>
void Bot::fight(const Target& target) {
    constexpr bool vs_champion = std::is_same_v<Target, cambc::champion>;
    auto& me = cambc::self();
    if (!target.is_visible()) return;
    const vec2 tile = target.position();

    auto basic_attack = [&] {
        if (!still_there(target, tile) || !me.can_attack() || !me.attack_tiles().is_set(tile)) return false;
        return try_action([&] { me.attack(target); return true; });
    };
    if (basic_attack()) attacked = true;

    // The turn's one cast: the spell planner picks slot, target and moment.
    cast_spells(true);
    // A reset only right after our own basic attack this turn (otherwise the engine rejects it).
    if (!cast_done && attacked && !me.can_attack() && still_there(target, tile)) {
        for (const auto& ability : me.abilities()) {
            if (!ability.info() || !ability.is_ready() || !ability.is_attack_reset()) continue;
            if (try_action([&] { return ability.use(); })) {
                cast_done = true;
                basic_attack();
            }
            break;
        }
    }
    if constexpr (vs_champion) {
        if (still_there(target, tile)) write_focus(tile);
    }
}

bool Bot::fight_best_target() {
    // A unit killed mid-turn can leave stale handles behind; a rejected read ends this fight only.
    try {
        const auto target = pick_target();
        if (!target) return false;
        std::visit([this](const auto& t) { fight(t); }, *target);
    } catch (const cambc::error&) {
    }
    return true;
}

// Positions that can hurt us next turn: awake camps (range up to 5) and enemy champions.
std::vector<vec2> Bot::threats() {
    auto& me = cambc::self();
    std::vector<vec2> result;
    for (auto tile : cambc::vision::awake_camp_tiles().positions())
        if (distance(me.position(), tile) <= 6) result.push_back(tile);
    for (auto tile : cambc::vision::champion_tiles(team::enemy).positions())
        if (distance(me.position(), tile) <= 6) result.push_back(tile);
    return result;
}

// Fights against enemy champions: hit what is in range when the trade is acceptable, chase weak or
// gem-carrying enemies when we clearly win, and back off when we clearly lose. True when the turn was used.
bool Bot::engage() {
    auto& me = cambc::self();
    const vec2 here = me.position();
    const auto balance = assess_balance();
    if (balance.enemies.empty()) { chase_turns = 0; return false; }

    const float ratio = balance.ratio();
    const bool melee = me.attack_range() <= 1;
    const float hp_share = me.health() / std::max(1.0f, me.max_health());
    const auto in_range = cambc::vision::champion_tiles(team::enemy) & me.attack_tiles();

    // Kill-reward awareness: enemies carrying gems lower our bar for a fight.
    unsigned enemy_gems = 0;
    int stunned = 0;
    float weakest = std::numeric_limits<float>::max();
    vec2 weakest_tile = balance.enemies.front();
    int nearest = std::numeric_limits<int>::max();
    for (auto tile : balance.enemies) {
        const auto enemy = cambc::vision::champion_at(tile);
        enemy_gems += enemy.gem_count();
        nearest = std::min(nearest, distance(here, tile));
        if (enemy.health() < weakest) { weakest = enemy.health(); weakest_tile = tile; }
        if (enemy.buffs().has_stun()) ++stunned;
    }
    const float engage_bar = 1.15f - std::min(0.35f, enemy_gems / 120.0f) + std::min(0.4f, me.gem_count() / 100.0f)
        - 0.2f * static_cast<float>(stunned);
    const bool favourable = ratio >= engage_bar;
    const bool losing = ratio < 0.6f;
    const float burst = me.strength() * (has_ready_reset() ? 2.0f : 1.0f);

    if (!in_range.is_empty()) {
        chase_turns = 0;
        const bool kill_now = weakest <= burst && in_range.is_set(weakest_tile);
        if (favourable || kill_now || !losing) {
            fight_best_target();
            return true;
        }
        // Losing trade: melee hits on the way out, ranged kites away from adjacent enemies.
        if (melee) {
            fight_best_target();
            move_away_from(balance.enemies);
            return true;
        }
        if (nearest <= 2 && move_away_from(balance.enemies)) return true;
        fight_best_target();
        return true;
    }

    // Nobody in range. Back off from a clearly lost fight when there is something to lose.
    if (losing && nearest <= 4 && (me.gem_count() >= 10 || hp_share < 0.6f)) {
        chase_turns = 0;
        return move_away_from(balance.enemies);
    }

    // Chase a weak or rich enemy when we clearly win, briefly, and not into their spawn.
    const int round = cambc::round_number();
    if (round < chase_rest_until || hp_share < 0.5f) return false;
    const auto target = cambc::vision::champion_at(weakest_tile);
    const bool worth = target.health() < target.max_health() * 0.5f || target.gem_count() >= 20 || weakest <= burst * 3;
    if (!favourable || ratio < 1.5f || !worth || distance(here, weakest_tile) > 4) return false;
    const auto enemy_spawn = cambc::map::spawn_region(team::enemy);
    for (auto tile : enemy_spawn.positions())
        if (distance(tile, weakest_tile) <= 3) return false;
    if (++chase_turns > kChaseTurns) {
        chase_turns = 0;
        chase_rest_until = round + kChaseRest;
        return false;
    }
    const int reach = std::max(1, me.attack_range());
    cambc::bitboard goals;
    goals.clear();
    for (auto tile : tiles_around(weakest_tile, reach).positions())
        if (!enemy_spawn.is_set(tile)) goals.set(tile, true);
    if (!move_towards_closest(goals)) return false;
    if (melee) fight_best_target();
    return true;
}

bool Bot::carries_lock() {
    return cambc::self().consumables()[cambc::consumable_type::altar_lock].count() > 0;
}

// Track which altars hold our lock in the shared mask (altars 0..7).
void Bot::observe_altars() {
    const auto& altars = altar_list();
    std::uint8_t open = read_byte(kOpenAltars);
    const std::uint8_t before = open;
    for (std::size_t i = 0; i < altars.size() && i < 8; ++i) {
        const auto owner = altar_owner(altars[i]);
        if (owner == AltarOwner::unknown) continue;
        const auto bit = static_cast<std::uint8_t>(1u << i);
        if (owner == AltarOwner::ally) open &= static_cast<std::uint8_t>(~bit);
        else open |= bit;
    }
    if (open != before) write_byte(kOpenAltars, open);
}

// Lock an unlocked altar within reach, the assigned opening altar first.
bool Bot::place_lock() {
    auto& me = cambc::self();
    if (!carries_lock()) return false;
    const auto& altars = altar_list();
    const auto& order = plan().altars;
    const int duty = (rank >= 0 && rank < static_cast<int>(order.size())) ? order[rank] : -1;
    int best = -1;
    for (std::size_t i = 0; i < altars.size(); ++i) {
        if (distance(me.position(), altars[i]) > 2 || altar_owner(altars[i]) != AltarOwner::none) continue;
        if (best < 0 || static_cast<int>(i) == duty) best = static_cast<int>(i);
    }
    if (best < 0) return false;
    const auto lock = me.consumables()[cambc::consumable_type::altar_lock];
    if (!try_action([&] { return lock.use(altars[best]); })) return false;
    if (best == duty) duty_done = true;
    if (best < 8) write_byte(kOpenAltars, static_cast<std::uint8_t>(read_byte(kOpenAltars) & ~(1u << best)));
    return true;
}

// Basic-attack a visible enemy lock on one of the given altars in range.
bool Bot::clear_enemy_lock(const cambc::bitboard& altars) {
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

// Opening: each champion walks its assigned altar (draft lock order, by rank) and locks it.
// Later the rank-0 warden re-locks altars seen without our lock, fetching a lock when it can.
bool Bot::lock_duty() {
    auto& me = cambc::self();
    const auto& altars = altar_list();
    const auto& order = plan().altars;
    const int round = cambc::round_number();
    int target = -1;
    if (!duty_done && round < 200 && rank >= 0 && rank < static_cast<int>(order.size()) && carries_lock()) {
        if (altar_owner(altars[order[rank]]) == AltarOwner::ally) duty_done = true;
        else target = order[rank];
    }
    if (target < 0 && rank == 0 && (duty_done || round >= 200 || order.empty())) {
        const std::uint8_t open = read_byte(kOpenAltars);
        int best = std::numeric_limits<int>::max();
        for (std::size_t i = 0; i < altars.size() && i < 8; ++i) {
            if (!(open & (1u << i))) continue;
            const int d = distance(me.position(), altars[i]);
            if (d < best) { best = d; target = static_cast<int>(i); }
        }
        if (target >= 0 && !carries_lock()) {
            if (me.gold_count() >= cambc::consumable_type::altar_lock.cost() && me.gem_count() < 10
                && threats().empty() && !cambc::map::spawn_region(team::ally).is_set(me.position())) {
                me.recall();
                return true;
            }
            return false;
        }
    }
    if (target < 0) return false;
    const vec2 altar = altars[target];
    if (altar_owner(altar) == AltarOwner::enemy) {
        cambc::bitboard one;
        one.clear();
        one.set(altar, true);
        if (clear_enemy_lock(one)) return true;
        return move_towards_closest(tiles_around(altar, 1));
    }
    if (distance(me.position(), altar) <= 2) return place_lock();
    return move_towards_closest(tiles_around(altar, 2));
}

bool Bot::bank_gems() {
    auto& me = cambc::self();
    const unsigned gems = me.gem_count();
    if (gems == 0) {
        banking = false;
        return false;
    }

    int nearest = std::numeric_limits<int>::max();
    for (auto altar : altar_list())
        if (altar_owner(altar) != AltarOwner::enemy) nearest = std::min(nearest, distance(me.position(), altar));

    const bool late = cambc::round_number() > 1800;
    // Once committed, keep heading to the altar so the decision cannot flicker each step.
    if (gems >= kBankGems || late || (nearest <= 4 && gems >= 5)
        || cambc::global_submitted_gems(team::ally) + gems >= 1000)
        banking = true;
    if (!banking) return false;

    // Our own locks do not block us; enemy locks do, and are broken when no other altar is usable.
    const int range = cambc::constants::submit_gems_range;
    cambc::bitboard goals, enemy_locked;
    goals.clear();
    enemy_locked.clear();
    for (auto altar : altar_list()) {
        const auto owner = altar_owner(altar);
        if (owner == AltarOwner::enemy) { enemy_locked.set(altar, true); continue; }
        if (distance(me.position(), altar) <= range && owner != AltarOwner::unknown) {
            if (try_action([&] { me.submit_gems(altar); return true; })) return true;
        }
        goals |= tiles_around(altar, range);
    }
    if (move_towards_closest(goals)) return true;
    if (enemy_locked.is_empty()) return false;
    if (clear_enemy_lock(enemy_locked)) return true;
    cambc::bitboard near_locked;
    near_locked.clear();
    for (auto altar : enemy_locked.positions()) near_locked |= tiles_around(altar, 1);
    return move_towards_closest(near_locked);
}

// Score every camp group by reward per turn of travel and clearing; skip groups we cannot survive.
int Bot::choose_group() {
    
    auto& me = cambc::self();
    const auto& list = groups();
    const int round = cambc::round_number();
    if (skip_until.size() != list.size()) skip_until.assign(list.size(), 0);
    // Remember respawn timers so the goal does not flicker when the group leaves vision.
    const auto visible = cambc::vision::visible_tiles();
    for (std::size_t i = 0; i < list.size(); ++i) {
        if ((list[i].spawns & visible).is_empty()) continue;
        const auto state = list[i].group.state();
        if (const auto* respawning = std::get_if<cambc::map::camp_group_respawning>(&state))
            skip_until[i] = std::max(skip_until[i], round + std::max(respawning->timer, 1));
    }

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
    // Opening route from the draft: the team works through it in order while it lasts.
    const auto& route = plan().route;
    int route_target = -1;
    if (round < 400) {
        int progress = read_byte(kRouteProgress);
        while (progress < static_cast<int>(route.size()) && skip_until[route[progress]] > round) ++progress;
        if (progress != read_byte(kRouteProgress)) write_byte(kRouteProgress, static_cast<std::uint8_t>(progress));
        if (progress < static_cast<int>(route.size())) route_target = route[progress];
    }

    int best = -1;
    float best_score = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& info = list[i];
        if (info.has_monolith) continue;
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
        if (static_cast<int>(i) == team_target) score *= 2.5f; // pack up: stuns chain only together
        if (static_cast<int>(i) == route_target) score *= 4.0f;
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

// Engine rejections near the map edge (engine error 5) or of units that left vision mid-turn (97) end
// the current turn's plan only; the next turn starts fresh.
void Bot::play_turn() {
    try {
        plan_turn();
    } catch (const cambc::error&) {
    }
}

void Bot::plan_turn() {
    moved = false;
    move_failed = false;
    attacked = false;
    cast_exclusive = false;
    cast_done = false;
    auto& me = cambc::self();

    if (rank < 0 && me.is_alive()) {
        // Rank by champion type among the allies visible on the first turn (all start on spawn).
        rank = 0;
        const int mine = static_cast<int>(me.champion_name());
        for (auto tile : cambc::vision::champion_tiles(team::ally).positions()) {
            if (tile == me.position() || !cambc::vision::has_champion(tile)) continue;
            if (static_cast<int>(cambc::vision::champion_at(tile).champion_name()) < mine) ++rank;
        }
    }
    const bool at_spawn = me.is_alive() && cambc::map::spawn_region(team::ally).is_set(me.position());
    if (!me.is_alive() || at_spawn) buy_items();
    if (me.is_alive()) {
        try { observe_altars(); } catch (const cambc::error&) {}
    }

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

    // Spells before walking: anything castable while standing still is cast now when we fight in place
    // (a target in basic attack range) or it hits an enemy champion; the rest waits until after the step.
    try {
        const auto in_reach = (cambc::vision::champion_tiles(team::enemy) | cambc::vision::awake_camp_tiles())
            & me.attack_tiles();
        cast_spells(!in_reach.is_empty());
    } catch (const cambc::error&) {
    }

    // Enemy champions nearby: fight, chase or back off before banking or farming.
    bool engaged = false;
    try {
        engaged = engage();
    } catch (const cambc::error&) {
        engaged = true; // stale unit read after a kill: the turn's actions are already spent
    }
    if (!engaged) {
        bool busy = false;
        try {
            place_lock();
            busy = lock_duty();
        } catch (const cambc::error&) {
        }
        if (!busy && !bank_gems()) farm_camps();
        try { place_lock(); } catch (const cambc::error&) {}
    }

    // Last chance for the turn's cast: whatever is still legal after this turn's movement.
    try {
        if (me.is_alive() && !me.is_submitting_gems() && !me.is_recalling()) cast_spells(!moved);
    } catch (const cambc::error&) {
    }
}
