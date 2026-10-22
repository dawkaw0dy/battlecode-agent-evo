// Deathball: one leader picks camp groups and altars, the others stay on it, and the group fights together.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;

const char* g_action = "";

namespace {

// Shared memory bytes: leader slot + 1, leader x, leader y, heartbeat round, target group + 1, flags.
constexpr int kFlagEngage = 1;
constexpr int kFlagBank = 2;
constexpr int kFlagRecall = 4;

constexpr unsigned kBankGems = 24;
constexpr unsigned kShopGold = 180;
constexpr float kLowHealth = 0.3f;
constexpr int kFollowDist = 2;
constexpr int kNearDist = 3;

int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

// Build the square by hand so tiles near the map edge never reach the engine out of bounds.
cambc::bitboard tiles_around(vec2 position, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    for (int dx = -radius; dx <= radius; ++dx)
        for (int dy = -radius; dy <= radius; ++dy) {
            const vec2 tile{position.x + dx, position.y + dy};
            if (cambc::map::is_in_bounds(tile)) tiles.set(tile, true);
        }
    return tiles;
}

// Combine terrain and visible occupants using native bitboard operations.
cambc::bitboard blocked_tiles() {
    auto blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy) | cambc::vision::champion_tiles()
        | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles() | cambc::vision::ward_tiles();
    blocked.set(cambc::self().position(), false);
    return blocked;
}

bool occupied(vec2 tile) {
    return (cambc::vision::champion_tiles() | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles()
        | cambc::vision::ward_tiles()).is_set(tile);
}

// Our occupancy view can lag behind the engine's; a rejected step simply means no move this time.
bool try_move(vec2 step) {
    g_action = "move";
    try {
        return cambc::self().move_absolute(step);
    } catch (const cambc::error&) {
        return false;
    }
}

bool exclusive_with_movement(const cambc::ability& ability) {
    const auto& info = ability.info();
    if (!info) return false;
    return std::visit([](const auto& value) { return value.exclusive_with_movement; }, *info);
}

bool is_spell_caster(cambc::champion_type type) {
    using T = cambc::champion_type;
    return type == T::stellar || type == T::prism || type == T::cassi || type == T::willow;
}

const std::vector<std::pair<cambc::map::camp_group, cambc::bitboard>>& groups() {
    static const auto all = [] {
        std::vector<std::pair<cambc::map::camp_group, cambc::bitboard>> result;
        for (const auto& group : cambc::map::camp_groups()) result.emplace_back(group, group.spawn_tiles());
        return result;
    }();
    return all;
}

vec2 group_center(int index) {
    const auto tiles = groups()[index].second.positions();
    return tiles.empty() ? cambc::self().position() : tiles[tiles.size() / 2];
}

std::array<std::uint8_t, 8> read_memory() {
    std::array<std::uint8_t, 8> bytes{};
    cambc::shared_memory.read(0, std::span{bytes});
    return bytes;
}

void write_memory(const std::array<std::uint8_t, 8>& bytes) {
    cambc::shared_memory.write(0, std::span{bytes});
}

bool at_spawn() {
    const auto& me = cambc::self();
    return cambc::map::spawn_region(team::ally).is_set(me.position());
}

// Enumerate units from the occupied visible tiles so every handle is live this turn.
std::vector<cambc::champion> champions_on_tiles(team side) {
    std::vector<cambc::champion> result;
    for (auto tile : cambc::vision::champion_tiles(side).positions()) {
        if (!cambc::vision::has_champion(tile)) continue;
        // A unit seen at turn start can drop out of sight after our own step; skip it then.
        try {
            auto unit = cambc::vision::champion_at(tile);
            if (unit.health() > 0 && unit.position() == tile) result.push_back(unit);
        } catch (const cambc::error&) {}
    }
    return result;
}

std::vector<cambc::camp> camps_on_tiles() {
    std::vector<cambc::camp> result;
    for (auto tile : cambc::vision::camp_tiles().positions()) {
        if (!cambc::vision::has_camp(tile)) continue;
        try {
            auto unit = cambc::vision::camp_at(tile);
            if (unit.health() > 0 && unit.position() == tile) result.push_back(unit);
        } catch (const cambc::error&) {}
    }
    return result;
}

std::vector<cambc::champion> visible_allies() {
    std::vector<cambc::champion> result;
    const vec2 mine = cambc::self().position();
    for (const auto& ally : champions_on_tiles(team::ally))
        if (ally.position() != mine) result.push_back(ally);
    return result;
}

std::vector<cambc::champion> visible_enemies() {
    std::vector<cambc::champion> result;
    for (const auto& enemy : champions_on_tiles(team::enemy))
        if (enemy.health() > 0) result.push_back(enemy);
    return result;
}

} // namespace

bool Bot::move_towards_closest(cambc::bitboard goals) {
    g_action = "mtc";
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

    if (step == me.position() || occupied(step)) return false;
    if (try_move(step)) {
        moved = true;
        return true;
    }
    return false;
}

// Step to the free neighbouring tile farthest from the given threats.
bool Bot::move_away_from(const std::vector<vec2>& threats) {
    g_action = "away";
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
    if (step == me.position() || occupied(step)) return false;
    if (try_move(step)) {
        moved = true;
        return true;
    }
    return false;
}

std::vector<vec2> Bot::threats() {
    // Out-of-sight lookups mean the view went stale mid-turn: act as if nothing was found.
    try {
        g_action = "threats";
        auto& me = cambc::self();
        std::vector<vec2> result;
        for (const auto& camp : camps_on_tiles())
            if (camp.is_awake() && distance(me.position(), camp.position()) <= 6)
                result.push_back(camp.position());
        for (const auto& enemy : visible_enemies())
            if (distance(me.position(), enemy.position()) <= 5) result.push_back(enemy.position());
        return result;
    } catch (const cambc::error&) {
        return std::vector<vec2>{};
    }
}

void Bot::buy_items() {
    auto& me = cambc::self();
    using I = cambc::item_type;
    const bool focus = is_spell_caster(me.champion_name());
    // One Jungle emblem for cheaper camp fights, then Power shards (six for the price of five).
    const auto jungle_shard = focus ? I::jungle_shard_of_focus : I::jungle_shard_of_strength;
    const auto jungle_emblem = focus ? I::jungle_emblem_of_focus : I::jungle_emblem_of_strength;
    const auto power_shard = focus ? I::power_shard_of_focus : I::power_shard_of_strength;
    for (int guard = 0; guard < 64; ++guard) {
        if (me.gold_count() < power_shard.cost()) return;
        const auto counts = me.item_count();
        const auto item = counts[jungle_emblem] == 0 ? jungle_shard : power_shard;
        if (!me.can_buy(item, 1)) return;
        me.buy(item, 1);
    }
}

bool Bot::camp_allowed(cambc::camp_type kind) const {
    if (kind == cambc::camp_type::monolith) return false;
    if (kind == cambc::camp_type::golem) return cambc::round_number() > 350;
    return true;
}

// Best visible target in basic attack range: weakest enemy first, then camps of our fight.
std::optional<std::variant<cambc::champion, cambc::camp>> Bot::pick_target(int group, bool engaged) {
    // Out-of-sight lookups mean the view went stale mid-turn: act as if nothing was found.
    try {
        g_action = "pick";
        auto& me = cambc::self();
        const auto attack_tiles = me.attack_tiles();
        std::optional<cambc::champion> best_enemy;
        for (const auto& enemy : visible_enemies()) {
            if (!attack_tiles.is_set(enemy.position())) continue;
            if (!best_enemy || enemy.health() < best_enemy->health()) best_enemy = enemy;
        }
        if (best_enemy) return *best_enemy;
    
        std::optional<cambc::camp> best_camp;
        for (const auto& camp : camps_on_tiles()) {
            if (camp.health() <= 0) continue;
            if (!attack_tiles.is_set(camp.position())) continue;
            // Never wake camps on the way: only hit awake ones or the group we engage together.
            const bool ours = engaged && group >= 0 && groups()[group].second.is_set(camp.position());
            if (!camp.is_awake() && !ours) continue;
            if (!camp_allowed(camp.kind()) && !camp.is_awake()) continue;
            if (!best_camp || camp.health() < best_camp->health()) best_camp = camp;
        }
        if (best_camp) return *best_camp;
        return std::nullopt;
    } catch (const cambc::error&) {
        return std::nullopt;
    }
}

// A handle may outlive its unit within the turn: confirm the tile still holds a visible target.
template<class Target>
bool still_there(const Target& target, vec2 where) {
    if constexpr (std::is_same_v<Target, cambc::camp>) {
        if (!cambc::vision::has_camp(where)) return false;
    } else {
        if (!cambc::vision::has_champion(where)) return false;
    }
    return target.health() > 0;
}

template<class Target>
void Bot::fight(const Target& target) {
    // Out-of-sight lookups mean the view went stale mid-turn: act as if nothing was found.
    try {
        auto& me = cambc::self();
        const vec2 where = target.position();
        bool attacked = false;
        if (me.can_attack() && me.attack_tiles().is_set(where)) {
            (g_action = "attack", me.attack(target));
            attacked = true;
        }
    
        for (const auto& ability : me.abilities()) {
            if (!me.can_cast()) return;
            if (!ability.info() || !ability.is_ready()) continue;
            if (!still_there(target, where)) return;
            const bool exclusive = exclusive_with_movement(ability);
            if (exclusive && moved) continue;
            if (ability.is_attack_reset()) {
                // Only a spent basic attack can be reset.
                if (attacked && !me.can_attack() && me.attack_tiles().is_set(where)) {
                    if ((g_action = "use0", ability.use())) {
                        if (exclusive) cast_exclusive = true;
                        if (me.can_attack() && still_there(target, where) && me.attack_tiles().is_set(where))
                            (g_action = "attack", me.attack(target));
                    }
                    return;
                }
            } else if (ability.is_damage() || ability.is_cc() || ability.is_aoe_damage()) {
                if (!ability.valid_tiles().is_set(where)) continue;
                if (ability.can_target_ground() && !ability.can_target_enemies() && !ability.can_target_camps()) {
                    if ((g_action = "useG", ability.use(where))) { if (exclusive) cast_exclusive = true; return; }
                } else {
                    if constexpr (std::is_same_v<Target, cambc::camp>) {
                        if (!ability.can_target_camps()) continue;
                    } else {
                        if (!ability.can_target_enemies()) continue;
                    }
                    if ((g_action = "useT", ability.use(target))) { if (exclusive) cast_exclusive = true; return; }
                }
            }
        }
    } catch (const cambc::error&) {
        return;
    }
}

bool Bot::fight_best(int group, bool engaged) {
    const auto target = pick_target(group, engaged);
    if (!target) return false;
    std::visit([this](const auto& t) { fight(t); }, *target);
    return true;
}

// Shields absorb camp and champion hits while the group is in a fight.
void Bot::use_shield() {
    g_action = "shield";
    auto& me = cambc::self();
    if (!me.can_cast() || threats().empty() || me.health() > me.max_health() * 0.85f) return;
    for (const auto& ability : me.abilities()) {
        if (!ability.info() || !ability.is_shield() || !ability.is_ready() || !ability.can_target_self()) continue;
        if (exclusive_with_movement(ability) && moved) continue;
        if ((g_action = "use0", ability.use()) && exclusive_with_movement(ability)) cast_exclusive = true;
        return;
    }
}

// Heals keep the front line standing: patch the most injured visible ally in range.
void Bot::heal_allies() {
    g_action = "healq";
    auto& me = cambc::self();
    if (!me.can_cast()) return;
    for (const auto& ability : me.abilities()) {
        if (!ability.info() || !ability.is_heal() || !ability.is_ready() || !ability.can_target_allies()) continue;
        if (exclusive_with_movement(ability) && moved) continue;
        const auto tiles = ability.valid_tiles();
        std::optional<cambc::champion> best;
        for (const auto& ally : visible_allies()) {
            if (!tiles.is_set(ally.position())) continue;
            if (ally.max_health() - ally.health() < 2.0f * me.focus()) continue;
            if (!best || ally.health() / ally.max_health() < best->health() / best->max_health()) best = ally;
        }
        if (best && (g_action = "heal", ability.use(*best)) && exclusive_with_movement(ability)) cast_exclusive = true;
        return;
    }
}

// Chase enemies only when the visible group outnumbers them, and never far from the anchor.
bool Bot::chase_enemies(std::optional<vec2> anchor) {
    // Out-of-sight lookups mean the view went stale mid-turn: act as if nothing was found.
    try {
        g_action = "chase";
        auto& me = cambc::self();
        const auto enemies = visible_enemies();
        if (enemies.empty()) return false;
        int friends = 1;
        for (const auto& ally : visible_allies())
            if (distance(ally.position(), me.position()) <= 4) ++friends;
        int foes = 0;
        std::optional<cambc::champion> prey;
        for (const auto& enemy : enemies) {
            if (distance(enemy.position(), me.position()) > 7) continue;
            ++foes;
            if (anchor && distance(enemy.position(), *anchor) > 6) continue;
            if (!prey || enemy.health() < prey->health()) prey = enemy;
        }
        if (!prey || foes == 0 || friends < foes + (friends == 1 ? 1 : 0)) return false;
        if (me.attack_tiles().is_set(prey->position())) return false;
        cambc::bitboard goal = tiles_around(prey->position(), 1);
        return move_towards_closest(goal);
    } catch (const cambc::error&) {
        return false;
    }
}

bool Bot::submit_if_adjacent() {
    auto& me = cambc::self();
    if (me.gem_count() == 0) return false;
    for (auto altar : cambc::map::altars().positions()) {
        if (cambc::vision::has_altar_lock(altar)) continue;
        if (distance(me.position(), altar) <= cambc::constants::submit_gems_range) {
            g_action = "submit"; me.submit_gems(altar);
            return true;
        }
    }
    return false;
}

bool Bot::go_bank() {
    if (submit_if_adjacent()) return true;
    cambc::bitboard goals;
    goals.clear();
    for (auto altar : cambc::map::altars().positions())
        if (!cambc::vision::has_altar_lock(altar)) goals |= tiles_around(altar, cambc::constants::submit_gems_range);
    return move_towards_closest(goals);
}

// Remember respawn timers of groups we can see so targets do not flicker out of vision.
void Bot::update_respawns() {
    g_action = "respawns";
    const auto& all = groups();
    const int round = cambc::round_number();
    if (skip_until.size() != all.size()) skip_until.assign(all.size(), 0);
    const auto visible = cambc::vision::visible_tiles();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto& [group, spawns] = all[i];
        if ((spawns & visible).is_empty()) continue;
        const auto state = group.state();
        if (const auto* respawning = std::get_if<cambc::map::camp_group_respawning>(&state))
            skip_until[i] = std::max(skip_until[i], round + std::max(respawning->timer, 1));
        else if (skip_until[i] <= round + 1000)
            skip_until[i] = std::min(skip_until[i], round);
    }
}

// Score groups by reward per travel-and-clear time from the leader's tile.
int Bot::choose_target() {
    // Out-of-sight lookups mean the view went stale mid-turn: act as if nothing was found.
    try {
        g_action = "choose";
        const auto& all = groups();
        auto& me = cambc::self();
        const int round = cambc::round_number();
        int best = -1;
        float best_score = 0.0f;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (skip_until[i] > round) continue;
            bool allowed = true;
            float health = 0.0f;
            for (const auto& camp : all[i].first.camps()) {
                if (!camp_allowed(camp.kind())) allowed = false;
                health += camp.base_stats().base_health;
            }
            if (!allowed) continue;
            const float value = all[i].first.gem_value() + 0.15f * all[i].first.gold_value();
            const float dist = static_cast<float>(distance(me.position(), group_center(static_cast<int>(i))));
            const float score = value / (dist + health / 400.0f + 8.0f);
            if (best < 0 || score > best_score) { best = static_cast<int>(i); best_score = score; }
        }
        return best;
    } catch (const cambc::error&) {
        return -1;
    }
}

void Bot::lead(int round) {
    g_action = "lead";
    auto& me = cambc::self();
    auto memory = read_memory();
    int flags = memory[5];
    update_respawns();

    // Team shop trip: everyone recalls together and leaves spawn healed.
    if (flags & kFlagRecall) {
        if (at_spawn() && me.health() >= me.max_health() * 0.95f) flags &= ~kFlagRecall;
        else if (!at_spawn() && threats().empty()) {
            memory[5] = static_cast<std::uint8_t>(flags);
            write_memory(memory);
            me.recall();
            return;
        }
    }
    if (at_spawn() && me.health() < me.max_health() * 0.9f && visible_enemies().empty()) {
        memory[5] = static_cast<std::uint8_t>(flags);
        write_memory(memory);
        return;
    }

    // Retarget when the current group is gone, respawning or out of reach for too long.
    if (target >= 0 && skip_until[target] > round) target = -1;
    if (target < 0) {
        target = choose_target();
        best_dist = 1 << 20;
        stall_rounds = 0;
        wait_rounds = 0;
    }

    const auto allies = visible_allies();
    int near = 0;
    for (const auto& ally : allies)
        if (distance(ally.position(), me.position()) <= kNearDist) ++near;
    const bool gathered = near >= static_cast<int>(allies.size());

    // Banking is the leader's call; followers carry an even share and come along.
    const unsigned gems = me.gem_count();
    if (gems >= kBankGems || (round > 1800 && gems > 0)
        || (gems > 0 && cambc::global_submitted_gems(team::ally) + gems * 3 >= 1000))
        banking = true;
    if (gems == 0) banking = false;

    flags &= ~(kFlagEngage | kFlagBank);
    bool acted = false;
    if (fight_best(target, true)) acted = true;
    heal_allies();
    use_shield();

    if (!acted && chase_enemies(std::nullopt)) acted = true;

    if (!acted && banking) {
        flags |= kFlagBank;
        if (gathered || wait_rounds > 4) { wait_rounds = 0; go_bank(); }
        else ++wait_rounds;
        acted = true;
    }

    if (!acted && !banking && me.gold_count() >= kShopGold && threats().empty()
        && distance(me.position(), cambc::map::spawn_region(team::ally).positions()[0]) > 8) {
        flags |= kFlagRecall;
        memory = {static_cast<std::uint8_t>(slot + 1), static_cast<std::uint8_t>(me.position().x),
            static_cast<std::uint8_t>(me.position().y), static_cast<std::uint8_t>(round & 0xFF),
            static_cast<std::uint8_t>(target + 1), static_cast<std::uint8_t>(flags), 0, 0};
        write_memory(memory);
        me.recall();
        return;
    }

    if (!acted && target >= 0) {
        const auto& spawns = groups()[target].second;
        const vec2 center = group_center(target);
        const int dist = distance(me.position(), center);
        if (dist < best_dist) { best_dist = dist; stall_rounds = 0; }
        else if (++stall_rounds > 30) {
            skip_until[target] = round + 80;
            target = -1;
        }
        if (target >= 0) {
            bool reward = false;
            const auto visible = cambc::vision::visible_tiles();
            if (!(spawns & visible).is_empty()) {
                try {
                    reward = std::holds_alternative<cambc::map::camp_group_reward_available>(groups()[target].first.state());
                } catch (const cambc::error&) {}
            }
            if (dist <= 6 || reward) {
                // Engage only with the group around us, unless stragglers keep us waiting too long.
                if (gathered || reward || wait_rounds > 8) {
                    flags |= kFlagEngage;
                    wait_rounds = 0;
                    if (!fight_best(target, true) && move_towards_closest(spawns)) fight_best(target, true);
                } else {
                    ++wait_rounds;
                }
            } else if (gathered || wait_rounds > 5) {
                wait_rounds = 0;
                if (move_towards_closest(spawns)) fight_best(target, true);
            } else {
                ++wait_rounds;
            }
        }
    }

    memory = {static_cast<std::uint8_t>(slot + 1), static_cast<std::uint8_t>(me.position().x),
        static_cast<std::uint8_t>(me.position().y), static_cast<std::uint8_t>(round & 0xFF),
        static_cast<std::uint8_t>(target + 1), static_cast<std::uint8_t>(flags), 0, 0};
    write_memory(memory);
}

void Bot::follow(vec2 leader, int group, int flags) {
    g_action = "follow";
    auto& me = cambc::self();

    if (flags & kFlagRecall) {
        if (!at_spawn() && threats().empty() && me.gem_count() == 0) { me.recall(); return; }
    }
    if (at_spawn() && me.health() < me.max_health() * 0.9f && visible_enemies().empty()) return;

    const bool engaged = (flags & kFlagEngage) != 0;
    if (!engaged && submit_if_adjacent()) return;

    bool acted = fight_best(group, engaged);
    heal_allies();
    use_shield();
    if (acted) return;
    if (chase_enemies(leader)) return;

    if ((flags & kFlagBank) && me.gem_count() > 0 && distance(me.position(), leader) <= 4) {
        go_bank();
        return;
    }

    if (engaged && group >= 0 && distance(me.position(), leader) <= 6) {
        if (move_towards_closest(groups()[group].second)) fight_best(group, engaged);
        return;
    }
    if (distance(me.position(), leader) > kFollowDist) {
        cambc::bitboard goal = tiles_around(leader, kFollowDist);
        if (cambc::map::is_in_bounds(leader)) goal.set(leader, false);
        if (move_towards_closest(goal)) fight_best(group, engaged);
    }
}

void Bot::play_turn() {
    g_action = "turn";
    moved = false;
    cast_exclusive = false;
    auto& me = cambc::self();
    const int round = cambc::round_number();

    if (slot < 0) {
        const auto allies = cambc::champions(team::ally);
        for (std::size_t i = 0; i < allies.size(); ++i)
            if (allies[i] == me) slot = static_cast<int>(i);
        if (slot < 0) slot = 0;
    }

    if (!me.is_alive() || at_spawn()) buy_items();
    if (!me.is_alive()) return;

    auto memory = read_memory();
    const int leader_slot = static_cast<int>(memory[0]) - 1;
    const bool stale = memory[0] == 0 || static_cast<std::uint8_t>(round - memory[3]) > 2;
    bool leading = leader_slot == slot;

    // Keep the heartbeat while channelling so a team recall does not hand leadership away.
    if (leading && (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting())) {
        memory[1] = static_cast<std::uint8_t>(me.position().x);
        memory[2] = static_cast<std::uint8_t>(me.position().y);
        memory[3] = static_cast<std::uint8_t>(round & 0xFF);
        write_memory(memory);
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;

    if (!leading && stale) {
        leading = true;
        target = -1;
        banking = false;
        memory[0] = static_cast<std::uint8_t>(slot + 1);
        memory[3] = static_cast<std::uint8_t>(round & 0xFF);
        write_memory(memory);
    }
    me.set_priority(leading ? 100 : 10);

    // Badly hurt: step out of danger and recall; a leader hands the group over first.
    if (!at_spawn() && me.health() < me.max_health() * kLowHealth) {
        const auto danger = threats();
        if (danger.empty()) {
            if (leading) {
                memory[0] = 0;
                write_memory(memory);
            }
            me.recall();
            return;
        }
        if (move_away_from(danger)) {
            if (leading) {
                memory[0] = 0;
                write_memory(memory);
            }
            return;
        }
    }

    if (leading) {
        lead(round);
        return;
    }
    const vec2 leader{memory[1], memory[2]};
    if (!cambc::map::is_in_bounds(leader)) {
        fight_best(-1, false);
        return;
    }
    follow(leader, static_cast<int>(memory[4]) - 1, memory[5]);
}
