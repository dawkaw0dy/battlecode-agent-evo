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

constexpr unsigned kBankGems = 30;  // carried gems that justify a trip to an altar
constexpr unsigned kShopGold = 150; // gold that justifies a recall to shop
constexpr float kRecallHealth = 0.35f;

cambc::bitboard safe_tiles_around(vec2 position, int radius);

int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

cambc::bitboard tiles_around(vec2 position, int radius) {
    return safe_tiles_around(position, radius);
}

// Tiles within radius of a position, clipped to the map (draw_chebyshev can throw near the edge).
cambc::bitboard safe_tiles_around(vec2 position, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    const int w = static_cast<int>(cambc::map::width()), h = static_cast<int>(cambc::map::height());
    for (int dx = -radius; dx <= radius; ++dx)
        for (int dy = -radius; dy <= radius; ++dy) {
            const int x = position.x + dx, y = position.y + dy;
            if (x >= 0 && y >= 0 && x < w && y < h) tiles.set(vec2{x, y}, true);
        }
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
    float exposure = 0;  // sum over members of strength x health killed before it, in focus-fire order
    bool has_monolith = false;
};

// Damage a lone attacker takes while clearing a group with unit DPS, killing the highest strength per
// health first (the order pick_target uses): each member hits until the cumulative health up to it is gone.
float group_exposure(std::vector<CampCost> members) {
    std::sort(members.begin(), members.end(), [](const CampCost& a, const CampCost& b) {
        return a.strength * b.health > b.strength * a.health;
    });
    float elapsed = 0, total = 0;
    for (const auto& m : members) {
        elapsed += m.health;
        total += m.strength * elapsed;
    }
    return total;
}

const std::vector<GroupInfo>& groups() {
    static const auto result = [] {
        std::vector<GroupInfo> list;
        for (const auto& group : cambc::map::camp_groups()) {
            GroupInfo info{group, group.spawn_tiles()};
            std::vector<CampCost> members;
            for (const auto& camp : group.camps()) {
                const auto cost = camp_cost(camp.kind());
                members.push_back(cost);
                info.health += cost.health;
                info.strength += cost.strength;
                if (camp.kind() == cambc::camp_type::monolith) info.has_monolith = true;
            }
            info.exposure = group_exposure(std::move(members));
            list.push_back(std::move(info));
        }
        return list;
    }();
    return result;
}

int group_index(const cambc::map::camp_group& group) {
    const auto& list = groups();
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i].group == group) return static_cast<int>(i);
    return -1;
}

// Shared memory: byte 0 unused, bytes 2..7 hold one claim per champion slot (group index + 1, 0 = none).
constexpr int kClaimSlots = 6;
int claim_slot(cambc::champion_type type) {
    using T = cambc::champion_type;
    switch (type) { // the draft's first picks get distinct slots
    case T::vesper_vale: return 0;
    case T::marcella: return 1;
    case T::vendetta: return 2;
    case T::varg: return 3;
    case T::ryth: return 4;
    case T::tavi: return 5;
    default: return static_cast<int>(type) % kClaimSlots;
    }
}

std::array<int, kClaimSlots> read_claims() {
    std::array<std::uint8_t, kClaimSlots> raw{};
    cambc::shared_memory.read(2, std::span{raw.data(), raw.size()});
    std::array<int, kClaimSlots> claims{};
    for (int i = 0; i < kClaimSlots; ++i) claims[i] = static_cast<int>(raw[i]) - 1;
    return claims;
}

void write_claim(int slot, int group) {
    std::uint8_t value = group < 0 || group >= 255 ? 0 : static_cast<std::uint8_t>(group + 1);
    cambc::shared_memory.write(2 + slot, std::span{&value, 1});
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
        if (score > best_score) { best_score = score; best_enemy = enemy; }
    }
    if (best_enemy) return *best_enemy;

    // Camps: finish awake groups first, then the group we are working on; within a group kill the highest
    // strength per health first (clusters, which never hit back, last). Never wake a group we cannot clear.
    std::optional<cambc::camp> best_camp;
    float best_camp_score = -1;
    const auto awake = cambc::vision::awake_camp_tiles();
    for (auto tile : (cambc::vision::camp_tiles() & attack_tiles).positions()) {
        if (!cambc::vision::has_camp(tile)) continue;
        const auto camp = cambc::vision::camp_at(tile);
        const float health = camp.health();
        if (health <= 0) continue;
        const bool is_awake = awake.is_set(tile);
        const auto kind = camp.kind();
        const int index = group_index(camp.camp_group());
        if (!is_awake) {
            if (kind == cambc::camp_type::monolith) continue;
            if (index < 0 || index >= static_cast<int>(feasible.size())) {
                if (kind == cambc::camp_type::golem && me.strength() < 90) continue;
            } else if (!feasible[index] && index != target_group) {
                continue;
            }
        }
        float score = camp.strength() / std::max(1.0f, health) * 1000.0f + 1.0f / std::max(1.0f, health);
        if (health <= me.strength()) score += 50.0f;
        if (index == target_group) score += 100.0f;
        if (is_awake) score += 1000.0f;
        if (score > best_camp_score) { best_camp_score = score; best_camp = camp; }
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

    // One cast per turn: stun an unstunned champion, else the strongest damage spell, else reset and hit again.
    bool target_stunned = false;
    if constexpr (vs_champion) {
        if (still_there(target, tile)) target_stunned = cambc::vision::champion_at(tile).buffs().has_stun();
    }
    const cambc::ability* stun = nullptr;
    const cambc::ability* damage = nullptr;
    const cambc::ability* reset = nullptr;
    const cambc::ability* shield = nullptr;
    const auto abilities = me.abilities();
    for (const auto& ability : abilities) {
        if (!ability.info() || !ability.is_ready()) continue;
        if (exclusive_with_movement(ability) && moved) continue;
        if (ability.is_heal() || ability.is_dash()) continue;
        if (ability.is_attack_reset()) { reset = &ability; continue; }
        if (ability.is_shield()) { if (ability.can_target_self()) shield = &ability; continue; }
        if (!(ability.is_damage() || ability.is_cc() || ability.is_aoe_damage())) continue;
        if (!ability.valid_tiles().is_set(tile)) continue;
        if (ability.is_aoe_damage()) {
            if (!ability.can_target_ground()) continue;
        } else if (vs_champion ? !ability.can_target_enemies() : !ability.can_target_camps()) {
            continue;
        }
        if (vs_champion && ability.is_cc() && !target_stunned) stun = &ability;
        else if (!damage || ability.damage_amount() > damage->damage_amount()) damage = &ability;
    }

    auto cast = [&](const cambc::ability& ability) {
        if (!still_there(target, tile)) return false;
        bool ok;
        if (ability.is_aoe_damage()) ok = try_action([&] { return ability.use(tile); });
        else ok = try_action([&] { return ability.use(target); });
        if (ok && exclusive_with_movement(ability)) cast_exclusive = true;
        return ok;
    };

    if (stun && cast(*stun)) return;
    if (damage && damage->damage_amount() >= me.strength() && cast(*damage)) return;
    // A reset only right after our own basic attack this turn (otherwise the engine rejects it).
    if (reset && attacked && !me.can_attack() && still_there(target, tile)) {
        if (try_action([&] { return reset->use(); })) {
            basic_attack();
            return;
        }
    }
    if (damage && cast(*damage)) return;
    if (shield && me.health() < me.max_health() * 0.8f) try_action([&] { return shield->use(); });
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
    float weakest = std::numeric_limits<float>::max();
    vec2 weakest_tile = balance.enemies.front();
    int nearest = std::numeric_limits<int>::max();
    for (auto tile : balance.enemies) {
        const auto enemy = cambc::vision::champion_at(tile);
        enemy_gems += enemy.gem_count();
        nearest = std::min(nearest, distance(here, tile));
        if (enemy.health() < weakest) { weakest = enemy.health(); weakest_tile = tile; }
    }
    const float engage_bar = 1.15f - std::min(0.35f, enemy_gems / 120.0f) + std::min(0.4f, me.gem_count() / 100.0f);
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
            const auto ally = cambc::vision::champion_at(tile);
            if (ally.max_health() - ally.health() < 150) continue;
            if (!best || ally.health() / ally.max_health() < best->health() / best->max_health()) best = ally;
        }
        if (best && ability.use(*best)) {
            if (exclusive_with_movement(ability)) cast_exclusive = true;
        }
        return;
    }
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

// ---- Camps -----------------------------------------------------------------------------------
// Each group is scored by this champion's share of its reward per round: travel, waiting for a respawn,
// clearing with the allies that claimed it, and the five capture rounds. Rewards split among the allies
// present, so cheap groups are spread over the team while groups too dangerous to solo gather claimants.
// Groups whose clearing damage would push us under the recall threshold are a last resort only.

namespace {
constexpr int kCaptureRounds = 5;
constexpr float kSafeHealth = 0.35f; // keep this share of max health after a clear
} // namespace

int Bot::choose_group() {
    auto& me = cambc::self();
    const auto& list = groups();
    const int round = cambc::round_number();
    if (skip_until.size() != list.size()) skip_until.assign(list.size(), 0);
    if (respawn_at.size() != list.size()) respawn_at.assign(list.size(), 0);
    feasible.assign(list.size(), 0);

    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(terrain_blocked(), tiles_around(me.position(), 0), visited);

    const int slot = claim_slot(me.champion_name());
    const auto claims = read_claims();
    int team = 1;
    for (int i = 0; i < kClaimSlots; ++i) if (i != slot && claims[i] >= 0) ++team;
    team = std::max(team, 3);

    const float dps = std::max(10.0f, me.strength() + me.focus() * 0.25f);
    const bool jungle = me.item_count()[is_spell_caster(me.champion_name())
        ? cambc::item_type::jungle_emblem_of_focus : cambc::item_type::jungle_emblem_of_health] > 0;
    const float budget = me.health() - me.max_health() * kSafeHealth;
    const float gem_weight = 3.0f + round / 1000.0f;
    const float gold_weight = me.champion_name() == cambc::champion_type::vesper_vale ? 1.33f : 1.0f;

    const auto visible = cambc::vision::visible_tiles();
    const auto ally_tiles = cambc::vision::champion_tiles(team::ally);
    const auto enemy_tiles = cambc::vision::champion_tiles(team::enemy);
    int best = -1;
    float best_score = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& info = list[i];
        if (info.has_monolith || skip_until[i] > round) continue;

        int dist = std::numeric_limits<int>::max();
        for (auto tile : info.spawns.positions())
            if (visited.is_set(tile)) dist = std::min<int>(dist, field.get(tile));
        if (dist == std::numeric_limits<int>::max()) continue;

        // What we can see of the group: lifecycle, remaining health, who stands on it.
        float health = info.health;
        bool reward_ready = false;
        if (!(info.spawns & visible).is_empty()) {
            try {
                const auto state = info.group.state();
                if (const auto* respawning = std::get_if<cambc::map::camp_group_respawning>(&state)) {
                    respawn_at[i] = round + std::max(respawning->timer, 1);
                } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(state)) {
                    reward_ready = true;
                    respawn_at[i] = 0;
                } else {
                    respawn_at[i] = 0;
                }
                if (!(info.spawns & cambc::vision::camp_tiles() & visible).is_empty() || !reward_ready) {
                    float seen = 0;
                    for (auto tile : (info.spawns & cambc::vision::camp_tiles()).positions())
                        if (cambc::vision::has_camp(tile)) seen += std::max(0.0f, (float)cambc::vision::camp_at(tile).health());
                    if ((info.spawns & visible) == info.spawns && respawn_at[i] == 0) health = seen;
                }
            } catch (const cambc::error&) {
            }
        }
        if (reward_ready) {
            // Another ally already captures it: the reward would only be split, go elsewhere.
            bool taken = false;
            for (auto tile : (ally_tiles & info.spawns).positions()) {
                if (tile == me.position() || !cambc::vision::has_champion(tile)) continue;
                const int other = claim_slot(cambc::vision::champion_at(tile).champion_name());
                if (!info.spawns.is_set(me.position()) || other < slot) taken = true;
            }
            if (taken) continue;
            health = 0;
        }
        const int wait = respawn_at[i] > round ? std::max(0, respawn_at[i] - round - dist) : 0;
        if (respawn_at[i] > round + 120) continue;

        int claimants = 0;
        for (int c = 0; c < kClaimSlots; ++c)
            if (c != slot && claims[c] == static_cast<int>(i)) ++claimants;

        // Clearing time and damage taken with n attackers sharing the hits.
        const float scale = health / std::max(1.0f, info.health);
        auto clear_cost = [&](int n, float& turns) {
            turns = health / (dps * n);
            float damage = info.exposure * scale * scale / (dps * n) / n;
            if (jungle) damage *= 0.7f;
            return damage;
        };
        int n = 1 + claimants;
        float clear_turns = 0;
        bool safe = clear_cost(n, clear_turns) < budget;
        float factor = 1.0f;
        if (!safe) {
            // Too much for those already on it: worth calling the team only if the team can do it.
            float team_turns = 0;
            if (clear_cost(std::max(n, team), team_turns) < budget) {
                n = std::max(n, team);
                clear_turns = team_turns;
                factor = 0.45f;
                safe = true;
            } else {
                factor = 0.03f;
            }
        }
        feasible[i] = safe ? 1 : 0;

        const float value = info.group.gem_value() * gem_weight + info.group.gold_value() * gold_weight;
        const float rounds = dist + wait + clear_turns + kCaptureRounds + 4.0f;
        float score = value / n / rounds * factor;
        // Contested groups: enemy champions next to it.
        for (auto tile : enemy_tiles.positions()) {
            bool near = false;
            for (auto spawn : info.spawns.positions()) if (distance(tile, spawn) <= 3) { near = true; break; }
            if (near) { score *= 0.6f; break; }
        }
        if (static_cast<int>(i) == target_group) score *= 1.25f;
        if (best < 0 || score > best_score) { best = static_cast<int>(i); best_score = score; }
    }
    if (best >= 0) feasible[best] = 1;
    return best;
}

// Attack a camp already in range, otherwise walk to the chosen group (next to it while it respawns, so its
// spawn tiles stay free), and stand on a spawn tile to capture.
void Bot::farm_camps() {
    // An engine rejection while walking (engine error 5 seen once on 00lanes) only ends this turn's walk.
    try {
        walk_to_group();
    } catch (const cambc::error&) {
    }
}

void Bot::walk_to_group() {
    auto& me = cambc::self();
    const int slot = claim_slot(me.champion_name());
    if (fight_best_target()) { stuck_turns = 0; return; }

    const int group = choose_group();
    if (group != target_group) { stuck_turns = 0; last_distance = 1 << 20; }
    target_group = group;
    write_claim(slot, group);
    if (group < 0) {
        // Nothing worth doing: wait on spawn so shopping and healing keep up.
        try_action([&] { return move_towards_closest(cambc::map::spawn_region(team::ally)); });
        return;
    }

    const auto& spawns = groups()[group].spawns;
    const int round = cambc::round_number();
    const bool respawning = respawn_at[group] > round;
    if (spawns.is_set(me.position())) {
        stuck_turns = 0;
        // Standing on a respawning group's tile delays it: step off. Otherwise we are capturing.
        if (respawning) {
            cambc::bitboard goals = safe_tiles_around(me.position(), 2) & ~spawns;
            try_action([&] { return move_towards_closest(goals); });
        }
        return;
    }

    int dist = std::numeric_limits<int>::max();
    for (auto tile : spawns.positions()) dist = std::min(dist, distance(me.position(), tile));
    if (dist < last_distance || respawning) stuck_turns = 0;
    else if (++stuck_turns > 12) {
        // No progress: skip the group for a while.
        skip_until[group] = round + 50;
        stuck_turns = 0;
    }
    last_distance = std::min(last_distance, dist);

    if (respawning) {
        cambc::bitboard goals;
        goals.clear();
        const int reach = std::max(1, static_cast<int>(me.attack_range()));
        for (auto tile : spawns.positions()) goals |= safe_tiles_around(tile, reach);
        goals &= ~spawns;
        if (goals.is_set(me.position())) return; // wait in reach
        // The engine rejects some search inputs near map features (engine error 5): then just wait.
        try_action([&] { return move_towards_closest(goals); });
        return;
    }
    if (try_action([&] { return move_towards_closest(spawns); }))
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

    try {
        heal_allies();
    } catch (const cambc::error&) {
    }

    // Enemy champions nearby: fight, chase or back off before banking or farming.
    bool engaged = false;
    try {
        engaged = engage();
    } catch (const cambc::error&) {
        engaged = true; // stale unit read after a kill: the turn's actions are already spent
    }
    if (engaged) return;

    if (bank_gems()) return;
    farm_camps();
}

