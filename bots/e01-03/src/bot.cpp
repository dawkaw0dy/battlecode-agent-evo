// Champion decisions for a split farm: each champion farms the camp region assigned in the draft, tributes
// at that region's altar, and only regroups when shared memory reports an enemy invading a region.
#include <cambc.h>
#include "bot.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <cstddef>
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

constexpr unsigned kBankGems = 25;   // carried gems that justify a trip to the altar
constexpr unsigned kShopGold = 150;  // gold that justifies a recall to shop
constexpr float kRecallHealth = 0.4f;
constexpr float kForeignRegion = 0.3f; // score factor for groups outside our region
constexpr int kReportAge = 12;       // rounds an invasion report stays fresh
constexpr int kRegroupRange = 30;    // farthest an invasion report pulls us

int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

// Bounds-checked square around a tile (draw_chebyshev can fail at the map edge).
cambc::bitboard tiles_around(vec2 position, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    const int w = cambc::map::width(), h = cambc::map::height();
    for (int x = position.x - radius; x <= position.x + radius; ++x)
        for (int y = position.y - radius; y <= position.y + radius; ++y)
            if (x >= 0 && y >= 0 && x < w && y < h) tiles.set(vec2{x, y}, true);
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

bool is_spell_caster(cambc::champion_type type) {
    using T = cambc::champion_type;
    return type == T::stellar || type == T::prism || type == T::cassi;
}

int draft_index(cambc::champion_type type) {
    const int t = static_cast<int>(type);
    if (t <= 2) return t;
    if (t == 3 || t == 4) return 3; // Cassi and Issac share one pick
    return t - 1;
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

// Invasion report in shared memory: u16 round, x, y, region, enemy count.
struct Report { int round = -1000; vec2 position{0, 0}; int region = -1; int enemies = 0; };

Report read_report() {
    std::array<std::byte, 6> bytes{};
    cambc::shared_memory.read(0, std::span{bytes});
    Report report;
    const int round = static_cast<int>(bytes[0]) | (static_cast<int>(bytes[1]) << 8);
    if (round == 0) return report;
    report.round = round;
    report.position = vec2{static_cast<int>(bytes[2]), static_cast<int>(bytes[3])};
    report.region = static_cast<int>(bytes[4]);
    report.enemies = static_cast<int>(bytes[5]);
    return report;
}

void write_report(vec2 position, int region, int enemies) {
    const int round = cambc::round_number();
    std::array<std::byte, 6> bytes{static_cast<std::byte>(round & 0xFF), static_cast<std::byte>((round >> 8) & 0xFF),
        static_cast<std::byte>(position.x), static_cast<std::byte>(position.y),
        static_cast<std::byte>(region & 0xFF), static_cast<std::byte>(std::min(enemies, 255))};
    cambc::shared_memory.write(0, std::span<const std::byte>{bytes});
}

// Groups closer to the enemy spawn than to ours: lone champions meet the grouped enemy there.
bool enemy_side(const cambc::bitboard& spawns) {
    static const vec2 own = cambc::map::spawn_region(team::ally).positions()[0];
    static const vec2 enemy = cambc::map::spawn_region(team::enemy).positions()[0];
    const auto tiles = spawns.positions();
    if (tiles.empty()) return false;
    return distance(tiles[0], enemy) + 4 < distance(tiles[0], own);
}

template<class Action>
bool try_action(Action&& action) {
    try {
        return action();
    } catch (const cambc::error&) {
        return false;
    }
}

} // namespace

void Bot::init() {
    initialised = true;
    const auto& list = groups();
    group_region.assign(list.size(), -1);
    skip_until.assign(list.size(), 0);
    const auto& memory = cambc::draft_memory;
    if (memory.size() < static_cast<std::size_t>(plan::kGroupRegion) || memory[plan::kMagic] != std::byte{'S'}) return;
    if (static_cast<int>(memory[plan::kGroupCount]) != static_cast<int>(list.size() & 0xFF)) return;
    const int regions = static_cast<int>(memory[plan::kRegions]);
    const int slot = static_cast<int>(memory[plan::kSlotOf + draft_index(cambc::self().champion_name())]);
    if (slot >= regions || slot >= plan::kMaxRegions) return;
    region = slot;
    for (std::size_t i = 0; i < list.size() && plan::kGroupRegion + i < memory.size(); ++i)
        group_region[i] = static_cast<int>(memory[plan::kGroupRegion + i]);
    const int ax = static_cast<int>(memory[plan::kAltar + 2 * region]);
    const int ay = static_cast<int>(memory[plan::kAltar + 2 * region + 1]);
    if (ax < cambc::map::width() && ay < cambc::map::height() && cambc::map::altars().is_set(vec2{ax, ay}))
        home_altar = vec2{ax, ay};
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

bool Bot::move_away_from(const std::vector<vec2>& threats) {
    auto& me = cambc::self();
    if (threats.empty() || cast_exclusive || move_failed || !me.can_move()) return false;
    const auto blocked = blocked_tiles();
    const auto home = cambc::map::spawn_region(team::ally);
    auto score = [&](vec2 p) {
        int best = std::numeric_limits<int>::max();
        for (auto t : threats) best = std::min(best, distance(p, t));
        return best * 4 + (home.is_set(p) ? 8 : 0);
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

// Lone farmers want sustain against camps first (Jungle), then raw damage (Power shards).
void Bot::buy_items() {
    auto& me = cambc::self();
    using I = cambc::item_type;
    const bool focus = is_spell_caster(me.champion_name());
    const auto jungle_shard = focus ? I::jungle_shard_of_focus : I::jungle_shard_of_health;
    const auto jungle_emblem = focus ? I::jungle_emblem_of_focus : I::jungle_emblem_of_health;
    const auto power = focus ? I::power_shard_of_focus : I::power_shard_of_strength;

    for (int guard = 0; guard < 64; ++guard) {
        if (me.gold_count() < power.cost()) return;
        const auto counts = me.item_count();
        auto item = counts[jungle_emblem] == 0 ? jungle_shard : power;
        if (!me.can_buy(item, 1)) return;
        me.buy(item, 1);
    }
}

std::optional<std::variant<cambc::champion, cambc::camp>> Bot::pick_target() {
    auto& me = cambc::self();
    const auto attack_tiles = me.attack_tiles();
    std::optional<cambc::champion> best_enemy;
    for (auto tile : (cambc::vision::champion_tiles(team::enemy) & attack_tiles).positions()) {
        if (!cambc::vision::has_champion(tile)) continue;
        const auto enemy = cambc::vision::champion_at(tile);
        if (enemy.health() <= 0) continue;
        if (!best_enemy || enemy.health() < best_enemy->health()) best_enemy = enemy;
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
        // Alone we never wake a camp we cannot finish.
        if (!is_awake && (kind == cambc::camp_type::monolith
            || (kind == cambc::camp_type::golem && me.strength() < 100))) continue;
        if (!best_camp || (is_awake && !best_awake)
            || (is_awake == best_awake && camp.health() < best_camp->health())) {
            best_camp = camp;
            best_awake = is_awake;
        }
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
    if (me.can_attack() && me.attack_tiles().is_set(tile))
        attacked = try_action([&] { me.attack(target); return true; });

    for (const auto& ability : me.abilities()) {
        if (!ability.info() || !ability.is_ready()) continue;
        if (!still_there(target, tile)) return;
        const bool exclusive = exclusive_with_movement(ability);
        if (exclusive && moved) continue;
        if (ability.is_attack_reset()) {
            // Only right after our own attack; then swing again.
            if (!attacked || me.can_attack() || !ability.can_target_self()) continue;
            if (try_action([&] { return ability.use(); })) {
                if (exclusive) cast_exclusive = true;
                if (still_there(target, tile) && me.can_attack() && me.attack_tiles().is_set(tile))
                    try_action([&] { me.attack(target); return true; });
                return;
            }
        } else if (ability.is_dash() || ability.is_heal()) {
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
            if (me.health() < me.max_health() * 0.8f && ability.can_target_self()) {
                if (try_action([&] { return ability.use(); })) { if (exclusive) cast_exclusive = true; return; }
            }
        }
    }
}

bool Bot::fight_best_target() {
    try {
        const auto target = pick_target();
        if (!target) return false;
        std::visit([this](const auto& t) { fight(t); }, *target);
        return true;
    } catch (const cambc::error&) {
        // A unit left vision mid-turn; skip it this turn.
        return true;
    }
}

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
        if (best && try_action([&] { return ability.use(*best); })) {
            if (exclusive_with_movement(ability)) cast_exclusive = true;
        }
        return;
    }
}

std::vector<vec2> Bot::threats(int enemy_range) {
    auto& me = cambc::self();
    std::vector<vec2> result;
    for (auto tile : cambc::vision::awake_camp_tiles().positions())
        if (distance(me.position(), tile) <= 6) result.push_back(tile);
    for (auto tile : cambc::vision::champion_tiles(team::enemy).positions())
        if (distance(me.position(), tile) <= enemy_range) result.push_back(tile);
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
    if (gems >= kBankGems || late || (nearest <= 4 && gems >= 5)
        || cambc::global_submitted_gems(team::ally) + gems >= 1000)
        banking = true;
    if (!banking) return false;

    for (auto altar : cambc::map::altars().positions()) {
        if (cambc::vision::has_altar_lock(altar)) continue;
        if (distance(me.position(), altar) <= cambc::constants::submit_gems_range) {
            if (try_action([&] { me.submit_gems(altar); return true; })) return true;
        }
    }

    // Our region's altar, unless another one is much closer or it is locked.
    cambc::bitboard goals;
    goals.clear();
    if (home_altar && !cambc::vision::has_altar_lock(*home_altar)
        && distance(me.position(), *home_altar) <= nearest + 8) {
        goals = tiles_around(*home_altar, cambc::constants::submit_gems_range);
    } else {
        for (auto altar : cambc::map::altars().positions())
            if (!cambc::vision::has_altar_lock(altar))
                goals |= tiles_around(altar, cambc::constants::submit_gems_range);
    }
    return move_towards_closest(goals);
}

// Score camp groups by reward per turn of travel and clearing; groups in our region are strongly preferred.
int Bot::choose_group() {
    auto& me = cambc::self();
    const auto& list = groups();
    const int round = cambc::round_number();

    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(terrain_blocked(), tiles_around(me.position(), 0), visited);

    const float dps = std::max(10.0f, me.strength() + me.focus() * 0.2f);
    const bool jungle = me.item_count()[is_spell_caster(me.champion_name())
        ? cambc::item_type::jungle_emblem_of_focus : cambc::item_type::jungle_emblem_of_health] > 0;

    const auto visible = cambc::vision::visible_tiles();
    int best = -1;
    float best_score = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& info = list[i];
        if (info.has_monolith) continue;
        if (!(info.spawns & visible).is_empty()) {
            const auto state = info.group.state();
            if (const auto* respawning = std::get_if<cambc::map::camp_group_respawning>(&state))
                skip_until[i] = std::max(skip_until[i], round + std::max(respawning->timer, 1));
        }
        if (skip_until[i] > round) continue;

        int dist = std::numeric_limits<int>::max();
        for (auto tile : info.spawns.positions())
            if (visited.is_set(tile)) dist = std::min<int>(dist, field.get(tile));
        if (dist == std::numeric_limits<int>::max()) { skip_until[i] = 1 << 20; continue; }

        const float clear_turns = info.health / dps;
        float damage = info.strength * clear_turns;
        if (jungle) damage *= 0.7f;
        const float value = info.group.gem_value() * 3.0f + info.group.gold_value();
        float score = value / (dist + clear_turns + 8.0f);
        if (damage > me.health() * 0.6f) score *= 0.01f;
        if (region >= 0 && group_region[i] != region) score *= kForeignRegion;
        if (enemy_side(info.spawns)) score *= 0.5f;
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
        skip_until[group] = cambc::round_number() + 50 + dist;
        stuck_turns = 0;
    }
    last_distance = std::min(last_distance, dist);

    if (move_towards_closest(spawns))
        fight_best_target();
}

// Join an ally whose region is being invaded, if we are healthy and close enough.
bool Bot::answer_invasion() {
    auto& me = cambc::self();
    const Report report = read_report();
    const int round = cambc::round_number() & 0xFFFF;
    if (report.round < 0 || round - report.round > kReportAge || round < report.round) return false;
    if (report.region == region && region >= 0) return false; // our own region: we are already there
    if (me.health() < me.max_health() * 0.6f || me.gem_count() >= kBankGems) return false;
    const int dist = distance(me.position(), report.position);
    if (dist > kRegroupRange || dist <= 2) return false;
    return move_towards_closest(tiles_around(report.position, 2));
}

void Bot::play_turn() {
    moved = false;
    move_failed = false;
    attacked = false;
    cast_exclusive = false;
    auto& me = cambc::self();
    if (!initialised) init();

    const bool at_spawn = me.is_alive() && cambc::map::spawn_region(team::ally).is_set(me.position());
    if (!me.is_alive() || at_spawn) buy_items();

    if (!me.is_alive() || me.is_recalling() || me.is_submitting_gems() || me.is_teleporting())
        return;
    if (!me.can_move() && !me.can_attack()) return;

    // Count nearby enemies and allies; report invasions of the region we are farming.
    int enemies = 0, allies = 1;
    for (auto tile : cambc::vision::champion_tiles(team::enemy).positions())
        if (distance(tile, me.position()) <= 7) ++enemies;
    for (auto tile : cambc::vision::champion_tiles(team::ally).positions())
        if (tile != me.position() && distance(tile, me.position()) <= 7) ++allies;
    if (enemies > 0 && !at_spawn) write_report(me.position(), region, enemies);

    if (at_spawn && me.health() < me.max_health() * 0.9f && threats(5).empty()) return;

    // Outnumbered or hurt near enemies: step back and recall rather than fight alone.
    const bool outnumbered = enemies > allies || (enemies >= 2 && enemies >= allies)
        || (enemies > 0 && me.health() < me.max_health() * 0.5f);
    if (!at_spawn && (outnumbered || me.health() < me.max_health() * kRecallHealth)) {
        const auto danger = threats(5);
        if (danger.empty()) {
            if (me.health() < me.max_health() * 0.6f || me.gem_count() > 0 || enemies > allies) {
                try_action([&] { me.recall(); return true; });
                return;
            }
        } else {
            // Head home while the threats are not yet on us; otherwise step straight away from them.
            int closest = std::numeric_limits<int>::max();
            for (auto t : danger) closest = std::min(closest, distance(me.position(), t));
            if (closest >= 3 && move_towards_closest(cambc::map::spawn_region(team::ally))) return;
            if (move_away_from(danger)) return;
            fight_best_target();
            return;
        }
    }

    if (!at_spawn && me.gold_count() >= kShopGold && me.gem_count() < 10 && threats(6).empty()
        && distance(me.position(), cambc::map::spawn_region(team::ally).positions()[0]) > 6) {
        try_action([&] { me.recall(); return true; });
        return;
    }

    heal_allies();

    if (!(cambc::vision::champion_tiles(team::enemy) & me.attack_tiles()).is_empty()) {
        fight_best_target();
        return;
    }

    if (bank_gems()) return;
    if (answer_invasion()) return;
    farm_camps();
}
