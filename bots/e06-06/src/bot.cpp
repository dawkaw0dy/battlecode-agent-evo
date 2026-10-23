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
    std::vector<vec2> tiles;                    // spawn tiles, for cheap per-turn scans
    std::vector<std::pair<float, float>> table; // (health, strength) per camp from the patch-100 table
    float health = 0, strength = 0;
    bool has_monolith = false;
    bool has_golem = false;
};

const std::vector<GroupInfo>& groups() {
    static const auto result = [] {
        std::vector<GroupInfo> list;
        for (const auto& group : cambc::map::camp_groups()) {
            GroupInfo info{group, group.spawn_tiles()};
            info.tiles = info.spawns.positions();
            for (const auto& camp : group.camps()) {
                const auto cost = camp_cost(camp.kind());
                info.health += cost.health;
                info.strength += cost.strength;
                info.table.emplace_back(cost.health, cost.strength);
                if (camp.kind() == cambc::camp_type::monolith) info.has_monolith = true;
                if (camp.kind() == cambc::camp_type::golem) info.has_golem = true;
            }
            std::sort(info.table.begin(), info.table.end());
            list.push_back(std::move(info));
        }
        return list;
    }();
    return result;
}

// Opening plan written by the draft (draft.cpp): side margin per group and the opening group.
struct Plan {
    std::vector<int> margin; // enemy distance minus ours, per group (0 when unknown)
    int opening = -1;
};

const Plan& plan() {
    static const Plan result = [] {
        Plan value;
        try {
            const cambc::draft_memory_buffer memory{};
            if (memory.size() >= 3 && static_cast<std::uint8_t>(memory[0]) == kPlanMagic) {
                const std::size_t count = static_cast<std::uint8_t>(memory[1]);
                if (count == cambc::map::camp_groups().size() && memory.size() >= 3 + count) {
                    value.opening = static_cast<int>(static_cast<std::uint8_t>(memory[2])) - 1;
                    for (std::size_t i = 0; i < count; ++i)
                        value.margin.push_back(static_cast<std::int8_t>(static_cast<std::uint8_t>(memory[3 + i])));
                }
            }
        } catch (const cambc::error&) {
        }
        return value;
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
    std::uint16_t value = static_cast<std::uint16_t>(std::max(0, group + 1));
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

// ---- Defense: altars, tributes and staying alive -----------------------------------------------
// Gems only count once tributed, so this subsystem keeps altars usable and the bag alive:
// - altars are chosen by path length plus the turns needed to break an enemy lock (5 hits) and
//   a penalty for enemy champions standing near them; locks seen by any ally are shared in memory;
// - enemy locks are attacked from range instead of skipping the altar forever;
// - we carry an altar lock and drop it on altars we pass (blocking enemy tributes), first of all on
//   an altar an enemy champion is tributing at, since placing a lock cancels that channel;
// - a tribute starts only when the enemies in reach cannot kill us during its ceil(gems/10) turns
//   (damage does not cancel it, death does); small bags always go in;
// - the bag is banked earlier when enemies are around or we are hurt, and a hurt champion next to
//   an altar tributes instead of recalling (recall is cancelled by damage, tribute is not).

namespace {

constexpr std::size_t kLockMaskOffset = 6; // u16: altars last seen with an enemy lock (bit = altar index)
constexpr int kLockReach = 2;              // altar locks reach altars within two tiles
constexpr int kEnemyMemory = 25;           // rounds an enemy sighting keeps the bank threshold low
constexpr unsigned kWaryBankGems = 15;     // carried gems that justify banking with enemies about

const std::vector<vec2>& altar_list() {
    static const auto list = cambc::map::altars().positions();
    return list;
}

std::uint16_t read_lock_mask() {
    std::uint16_t value = 0;
    cambc::shared_memory.read(kLockMaskOffset, std::span{&value, 1});
    return value;
}

void write_lock_mask(std::uint16_t value) {
    cambc::shared_memory.write(kLockMaskOffset, std::span{&value, 1});
}

std::uint16_t altar_bit(std::size_t index) {
    return index < 16 ? static_cast<std::uint16_t>(1u << index) : 0;
}

// Refresh the shared mask from what this champion sees; returns the up-to-date mask.
std::uint16_t observe_locks() {
    const auto& altars = altar_list();
    const auto enemy_locks = cambc::vision::altar_lock_tiles(team::enemy);
    std::uint16_t mask = read_lock_mask();
    const std::uint16_t before = mask;
    for (std::size_t i = 0; i < altars.size() && i < 16; ++i) {
        if (!cambc::vision::in_vision(altars[i])) continue;
        if (enemy_locks.is_set(altars[i])) mask |= altar_bit(i);
        else mask &= static_cast<std::uint16_t>(~altar_bit(i));
    }
    if (mask != before) write_lock_mask(mask);
    return mask;
}

bool enemy_locked(std::size_t index, vec2 altar, std::uint16_t mask) {
    if (cambc::vision::in_vision(altar)) return cambc::vision::altar_lock_tiles(team::enemy).is_set(altar);
    return (mask & altar_bit(index)) != 0;
}

std::optional<cambc::altar_lock> visible_enemy_lock(vec2 altar) {
    for (const auto& lock : cambc::vision::altar_locks(team::enemy))
        if (lock.position() == altar) return lock;
    return std::nullopt;
}

int lock_hits(vec2 altar) {
    if (const auto lock = visible_enemy_lock(altar)) return static_cast<int>(lock->hits_remaining());
    return static_cast<int>(cambc::constants::altar_lock_hits);
}

bool carries_lock() {
    return cambc::self().consumables()[cambc::consumable_type::altar_lock].count() > 0;
}

// Damage the visible enemies (and awake camps) can deal to a champion standing at `at` over `turns`
// of our turns: one burst spell each plus basic attacks once they are in reach.
float incoming_damage(vec2 at, int turns) {
    float total = 0;
    for (auto tile : cambc::vision::champion_tiles(team::enemy).positions()) {
        if (!cambc::vision::has_champion(tile)) continue;
        const auto enemy = cambc::vision::champion_at(tile);
        if (!enemy.is_visible() || enemy.health() <= 0) continue;
        const int reach = std::max(1, static_cast<int>(enemy.attack_range()));
        const int approach = std::max(0, distance(tile, at) - reach);
        const int active = turns + 1 - approach;
        if (active <= 0) continue;
        total += enemy.strength() * static_cast<float>(active) + enemy.focus() * 2.0f;
    }
    for (auto tile : cambc::vision::awake_camp_tiles().positions()) {
        if (distance(tile, at) > 5 || !cambc::vision::has_camp(tile)) continue;
        total += cambc::vision::camp_at(tile).strength() * static_cast<float>(turns + 1);
    }
    return total;
}

int tribute_turns(unsigned gems) {
    return std::max(1, static_cast<int>((gems + 9) / 10));
}

} // namespace

// Buy a lock before shards so one is always on hand (10 gold).
void Bot::buy_lock() {
    auto& me = cambc::self();
    const auto lock = cambc::consumable_type::altar_lock;
    if (!carries_lock() && me.can_buy(lock)) try_action([&] { me.buy(lock); return true; });
}

// Drop our lock on an altar in reach that has none: first one an enemy is tributing at (cancels the
// channel), then any other, preferring the altar nearest the enemy spawn.
bool Bot::place_lock() {
    auto& me = cambc::self();
    if (!carries_lock()) return false;
    const vec2 here = me.position();
    const auto enemy_spawn = cambc::map::spawn_region(team::enemy).positions();
    const vec2 enemy_home = enemy_spawn.empty() ? here : enemy_spawn[0];
    std::optional<vec2> best;
    float best_score = -1;
    for (auto altar : altar_list()) {
        if (distance(here, altar) > kLockReach || !cambc::vision::in_vision(altar)) continue;
        if (cambc::vision::has_altar_lock(altar)) continue;
        float score = 100.0f - static_cast<float>(distance(altar, enemy_home));
        for (auto tile : cambc::vision::champion_tiles(team::enemy).positions()) {
            if (distance(tile, altar) > 1 || !cambc::vision::has_champion(tile)) continue;
            const auto enemy = cambc::vision::champion_at(tile);
            if (enemy.is_visible()) score += 500.0f + 5.0f * enemy.gem_count();
        }
        if (score > best_score) { best_score = score; best = altar; }
    }
    if (!best) return false;
    const auto lock = me.consumables()[cambc::consumable_type::altar_lock];
    return try_action([&] { return lock.use(*best); });
}

// Basic-attack a visible enemy lock on `altar` when it is in range.
bool Bot::hit_lock(vec2 altar) {
    auto& me = cambc::self();
    if (attacked || !me.can_attack() || !me.attack_tiles().is_set(altar)) return false;
    const auto lock = visible_enemy_lock(altar);
    if (!lock) return false;
    if (!try_action([&] { me.attack(*lock); return true; })) return false;
    attacked = true;
    return true;
}

// Survival before anything else: recall or tribute when badly hurt, lock altars in passing.
bool Bot::guard_life() {
    auto& me = cambc::self();
    const vec2 here = me.position();
    observe_locks();
    for (auto tile : cambc::vision::champion_tiles(team::enemy).positions())
        if (distance(tile, here) <= 8) last_enemy_seen = cambc::round_number();
    place_lock();

    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(here);
    if (at_spawn || me.health() >= me.max_health() * kRecallHealth) return false;

    // Next to a usable altar with gems: the tribute survives damage, a recall would not.
    if (me.gem_count() > 0) {
        const auto mask = read_lock_mask();
        const auto& altars = altar_list();
        for (std::size_t i = 0; i < altars.size(); ++i) {
            if (distance(here, altars[i]) > cambc::constants::submit_gems_range) continue;
            if (enemy_locked(i, altars[i], mask)) continue;
            if (incoming_damage(here, tribute_turns(me.gem_count())) >= me.health()) continue;
            if (try_action([&] { me.submit_gems(altars[i]); return true; })) return true;
        }
    }
    const auto danger = threats();
    if (danger.empty()) {
        if (try_action([&] { me.recall(); return true; })) return true;
    }
    if (move_away_from(danger)) return true;
    fight_best_target();
    return true;
}

bool Bot::bank_gems() {
    auto& me = cambc::self();
    const unsigned gems = me.gem_count();
    if (gems == 0) {
        banking = false;
        return false;
    }
    const vec2 here = me.position();
    const int round = cambc::round_number();
    const auto mask = observe_locks();
    const auto& altars = altar_list();
    if (altars.empty()) return false;

    // Path lengths from here to the tiles next to each altar.
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(terrain_blocked(), tiles_around(here, 0), visited);
    const auto enemy_tiles = cambc::vision::champion_tiles(team::enemy);

    int best = -1;
    float best_cost = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < altars.size(); ++i) {
        const vec2 altar = altars[i];
        int dist = std::numeric_limits<int>::max();
        if (distance(here, altar) <= cambc::constants::submit_gems_range) dist = 0;
        else
            for (auto tile : tiles_around(altar, cambc::constants::submit_gems_range).positions())
                if (visited.is_set(tile)) dist = std::min<int>(dist, field.get(tile));
        if (dist == std::numeric_limits<int>::max()) continue;
        float cost = static_cast<float>(dist);
        if (enemy_locked(i, altar, mask)) cost += static_cast<float>(lock_hits(altar)) + 2.0f;
        for (auto tile : enemy_tiles.positions())
            if (distance(tile, altar) <= 3) cost += 8.0f;
        if (cost < best_cost) { best_cost = cost; best = static_cast<int>(i); }
    }
    if (best < 0) return false;
    const vec2 altar = altars[best];

    const bool wary = round - last_enemy_seen <= kEnemyMemory;
    const bool hurt = me.health() < me.max_health() * 0.5f;
    const bool late = round > 1800;
    if (gems >= kBankGems || (wary && gems >= kWaryBankGems) || late || (best_cost <= 4 && gems >= 5)
        || (hurt && gems >= 10) || cambc::global_submitted_gems(team::ally) + gems >= 1000)
        banking = true;
    if (!banking) return false;

    const bool locked = enemy_locked(best, altar, mask);
    if (distance(here, altar) <= cambc::constants::submit_gems_range) {
        if (locked) {
            hit_lock(altar);
            return true; // stay and keep hitting it
        }
        const float danger = incoming_damage(here, tribute_turns(gems));
        if (gems <= 10 || danger < me.health() * 0.9f) {
            if (!cambc::vision::has_altar_lock(altar)) place_lock();
            if (try_action([&] { me.submit_gems(altar); return true; })) return true;
        }
        // Too dangerous to channel here: fall back from the enemies and let the fight logic decide.
        std::vector<vec2> enemies;
        for (auto tile : enemy_tiles.positions())
            if (distance(tile, here) <= 6) enemies.push_back(tile);
        if (move_away_from(enemies)) return true;
        return fight_best_target();
    }
    // Ranged champions break the lock from range on the way in.
    if (locked && hit_lock(altar)) return true;
    return move_towards_closest(tiles_around(altar, cambc::constants::submit_gems_range));
}

// ---- Camps: which group to farm, in what order, with how many champions ----------------------
// Every group is valued as (gems x weight + gold) per turn of travel, clearing and the five capture
// phases; gems weigh more after round 1200. Clearing is simulated per camp, lowest health first,
// with the allies within 8 sharing the damage: each camp hits until it dies, so the bill is
// sum(strength_i x kill_time_i) / pack. A clear costing more than kDangerShare of our health is a
// last resort. Live camp health is read whenever spawns are in vision (half-cleared groups are
// cheap), a cleared group whose reward is waiting costs only the walk and the capture, and
// respawning groups are skipped until their remembered timer runs out. The whole team is pulled
// toward the last written team target (soft packing: lone farmers get picked off by invaders).
// The scan is kept cheap (precomputed tile lists, one vision fetch per turn): an earlier version
// ran out of fuel and lost most games.

namespace {

constexpr int kCaptureTurns = 5;     // capture phases on a spawn tile
constexpr float kSlack = 3.0f;       // extra turns per trip (approach, settling on a spawn tile)
constexpr float kDangerShare = 0.75f; // share of our health a clear may cost before it is a last resort
constexpr float kFocusShare = 0.2f;  // spell damage per turn as a share of Focus
constexpr float kTeamBonus = 2.5f;   // pull toward the team target: stuns chain only together
constexpr float kStickiness = 1.3f;  // keep the current group unless another is clearly better
constexpr int kOpeningRounds = 150;  // the draft's opening group is preferred until taken or this round
constexpr float kOpeningBonus = 3.0f;

// Damage the pack soaks while dealing `team_dps` per round and killing the camps lowest health
// first: each camp hits until it dies, so the bill is sum(strength_i x kill_time_i).
float clear_damage(const std::vector<std::pair<float, float>>& camps, float team_dps) {
    float elapsed = 0, damage = 0;
    for (const auto& [health, strength] : camps) {
        elapsed += health / team_dps;
        damage += strength * elapsed;
    }
    return damage;
}

} // namespace

int Bot::choose_group() {
    auto& me = cambc::self();
    const auto& list = groups();
    const int round = cambc::round_number();
    const vec2 here = me.position();
    if (skip_until.size() != list.size()) skip_until.assign(list.size(), 0);
    if (respawn_at.size() != list.size()) respawn_at.assign(list.size(), 0);

    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(terrain_blocked(), tiles_around(here, 0), visited);

    // Allies close enough to share the fight.
    int helpers = 1;
    for (auto tile : cambc::vision::champion_tiles(team::ally).positions())
        if (tile != here && distance(tile, here) <= 8) ++helpers;
    const float k = static_cast<float>(helpers);

    const float dps = std::max(10.0f, me.strength() + me.focus() * kFocusShare);
    const bool jungle = me.item_count()[is_spell_caster(me.champion_name())
        ? cambc::item_type::jungle_emblem_of_focus : cambc::item_type::jungle_emblem_of_health] > 0;
    const float gem_weight = round > 1200 ? 5.0f : 3.0f;
    const int team_target = read_team_target();
    const auto& opening_plan = plan();
    const auto visible = cambc::vision::visible_tiles();
    const auto camp_tiles = cambc::vision::camp_tiles();

    std::vector<std::pair<float, float>> live;
    int best = -1;
    float best_score = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& info = list[i];
        if (info.has_monolith || skip_until[i] > round) continue;

        int dist = std::numeric_limits<int>::max();
        bool seen = false;
        for (auto tile : info.tiles) {
            if (visited.is_set(tile)) dist = std::min<int>(dist, field.get(tile));
            if (visible.is_set(tile)) seen = true;
        }
        if (dist == std::numeric_limits<int>::max()) continue;

        // Group state: respawn timers are remembered so the goal does not flicker out of vision.
        bool reward_ready = false;
        if (seen) {
            const auto state = info.group.state();
            if (const auto* respawning = std::get_if<cambc::map::camp_group_respawning>(&state))
                respawn_at[i] = round + std::max(respawning->timer, 1);
            else {
                respawn_at[i] = 0;
                reward_ready = std::holds_alternative<cambc::map::camp_group_reward_available>(state);
            }
        }
        if (respawn_at[i] > round) continue;

        // Remaining camps: live health on visible spawn tiles, table values for the rest.
        const std::vector<std::pair<float, float>>* camps = &info.table;
        float health = info.health;
        if (reward_ready) {
            live.clear();
            camps = &live;
            health = 0;
        } else if (seen) {
            live.clear();
            health = 0;
            const float count = static_cast<float>(std::max<std::size_t>(1, info.tiles.size()));
            for (auto tile : info.tiles) {
                float camp_health = info.health / count, camp_strength = info.strength / count;
                if (visible.is_set(tile)) {
                    if (!camp_tiles.is_set(tile) || !cambc::vision::has_camp(tile)) continue; // dead
                    const auto camp = cambc::vision::camp_at(tile);
                    if (!camp.is_visible() || camp.health() <= 0) continue;
                    camp_health = camp.health();
                    camp_strength = camp.strength();
                }
                live.emplace_back(camp_health, camp_strength);
                health += camp_health;
            }
            std::sort(live.begin(), live.end());
            camps = &live;
        }

        const float clear_turns = health / (dps * k);
        float damage = clear_damage(*camps, dps * k) / k;
        if (jungle) damage *= 0.7f;
        const float value = info.group.gem_value() * gem_weight + info.group.gold_value();
        float score = value / (static_cast<float>(dist + kCaptureTurns) + clear_turns + kSlack);
        // A clear we would not survive stays possible, but only as a last resort over idling.
        if (damage > me.health() * kDangerShare) score *= 0.05f;
        if (static_cast<int>(i) == target_group) score *= kStickiness;
        if (static_cast<int>(i) == team_target) score *= kTeamBonus;
        if (round < kOpeningRounds && static_cast<int>(i) == opening_plan.opening && respawn_at[i] == 0)
            score *= kOpeningBonus;
        if (best < 0 || score > best_score) { best = static_cast<int>(i); best_score = score; }
    }
    return best;
}

// Attack a camp already in range, otherwise walk to the chosen camp group and capture on its spawns.
void Bot::farm_camps() {
    if (fight_best_target()) { stuck_turns = 0; return; }

    const int group = choose_group();
    if (group != target_group) { stuck_turns = 0; last_distance = 1 << 20; }
    target_group = group;
    auto& me = cambc::self();
    const int round = cambc::round_number();

    if (group < 0) {
        // Nothing worth doing: wait on spawn so shopping and healing keep up.
        move_towards_closest(cambc::map::spawn_region(team::ally));
        return;
    }
    write_team_target(group);

    const auto& spawns = groups()[group].spawns;
    if (spawns.is_set(me.position())) { stuck_turns = 0; return; } // capturing

    int dist = std::numeric_limits<int>::max();
    for (auto tile : spawns.positions()) dist = std::min(dist, distance(me.position(), tile));
    if (dist < last_distance) stuck_turns = 0;
    else if (++stuck_turns > 12) {
        // No progress: skip the group for a while.
        skip_until[group] = round + 50;
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

    const bool at_spawn = me.is_alive() && cambc::map::spawn_region(team::ally).is_set(me.position());
    if (!me.is_alive() || at_spawn) {
        buy_lock();
        buy_items();
    }

    if (!me.is_alive() || me.is_recalling() || me.is_submitting_gems() || me.is_teleporting())
        return;

    // Stunned or otherwise unable to act.
    if (!me.can_move() && !me.can_attack()) return;

    // Heal up on spawn before heading out again.
    if (at_spawn && me.health() < me.max_health() * 0.9f && threats().empty()) return;

    // Survival first: tribute or recall when badly hurt, drop locks on altars in passing.
    if (guard_life()) return;

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
    if (!engaged && !bank_gems()) farm_camps();

    // Last chance for the turn's cast: whatever is still legal after this turn's movement.
    try {
        if (me.is_alive() && !me.is_submitting_gems() && !me.is_recalling()) cast_spells(!moved);
    } catch (const cambc::error&) {
    }
}
