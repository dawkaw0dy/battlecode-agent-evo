// Endgame: when to bank carried gems, which altar to use (path distance, ally locks are fine,
// enemy locks cost their hits), and denying enemy tributes with altar locks.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <limits>
#include <vector>

using cambc::vec2;
using cambc::team;

namespace {

constexpr unsigned kBankGems = 30;      // carried gems that justify a trip to an altar
constexpr int kLockRange = 2;           // altar lock placement range
constexpr int kTimeMargin = 25;         // spare rounds kept before the round limit

int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

template<class Action>
bool guarded(Action&& action) {
    try {
        return action();
    } catch (const cambc::error&) {
        return false;
    }
}

cambc::bitboard around(vec2 p, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    const int w = static_cast<int>(cambc::map::width()), h = static_cast<int>(cambc::map::height());
    for (int dx = -radius; dx <= radius; ++dx)
        for (int dy = -radius; dy <= radius; ++dy) {
            const int x = p.x + dx, y = p.y + dy;
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            tiles.set(vec2{x, y}, true);
        }
    return tiles;
}

bool enemy_lock(vec2 altar) {
    return (cambc::vision::altar_lock_tiles(team::enemy)).is_set(altar);
}

cambc::bitboard terrain_blocked_tiles() {
    return cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(team::enemy);
}

bool ally_lock(vec2 altar) {
    return (cambc::vision::altar_lock_tiles(team::ally)).is_set(altar);
}

int enemies_near(vec2 p, int radius) {
    int n = 0;
    for (auto tile : cambc::vision::champion_tiles(team::enemy).positions())
        if (distance(p, tile) <= radius) ++n;
    return n;
}

} // namespace

// Buy one altar lock on spawn (cheap denial tool); called before the shard shopping.
void Bot::stock_lock() {
    auto& me = cambc::self();
    if (cambc::round_number() < 150) return;
    const auto lock = cambc::consumable_type::altar_lock;
    guarded([&] {
        if (!me.can_buy(lock)) return false;
        me.buy(lock);
        return true;
    });
}

// Place a lock on an altar where a visible enemy is tributing (cancels the channel) or is about to.
bool Bot::deny_tributes() {
    auto& me = cambc::self();
    const auto lock = cambc::consumable_type::altar_lock;
    if (me.consumables()[lock].count() == 0) return false;
    const auto enemies = cambc::vision::champion_tiles(team::enemy);
    if (enemies.is_empty()) return false;
    const unsigned enemy_score = cambc::global_submitted_gems(team::enemy);

    vec2 best{};
    int best_value = 0;
    for (auto altar : cambc::map::altars().positions()) {
        if (distance(me.position(), altar) > kLockRange) continue;
        if (cambc::vision::has_altar_lock(altar)) continue;
        int value = 0;
        for (auto tile : enemies.positions()) {
            if (distance(tile, altar) > cambc::constants::submit_gems_range) continue;
            guarded([&] {
                if (!cambc::vision::has_champion(tile)) return false;
                const auto enemy = cambc::vision::champion_at(tile);
                const int gems = static_cast<int>(enemy.gem_count());
                if (enemy.is_submitting_gems()) value += 100 + gems;
                else if (gems >= 15 || (gems > 0 && enemy_score + gems >= cambc::constants::winning_gems))
                    value += gems;
                return true;
            });
        }
        if (value > best_value) { best_value = value; best = altar; }
    }
    if (best_value <= 0) return false;
    return guarded([&] { return me.consumables()[lock].use(best); });
}

bool Bot::bank_gems() {
    auto& me = cambc::self();
    const unsigned gems = me.gem_count();
    if (gems == 0) {
        banking = false;
        return false;
    }

    // Path distance from here to each altar's tribute ring; enemy-locked altars pay for clearing.
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(terrain_blocked_tiles(), around(me.position(), 0), visited);
    const int range = cambc::constants::submit_gems_range;
    int best_cost = std::numeric_limits<int>::max(), best_path = best_cost;
    vec2 best_altar{};
    bool found = false;
    for (auto altar : cambc::map::altars().positions()) {
        int path = std::numeric_limits<int>::max();
        if (distance(me.position(), altar) <= range) path = 0;
        else
            for (auto tile : around(altar, range).positions())
                if (visited.is_set(tile)) path = std::min<int>(path, field.get(tile));
        if (path == std::numeric_limits<int>::max()) continue;
        int cost = path;
        if (enemy_lock(altar) && !ally_lock(altar)) {
            int hits = 5;
            guarded([&] {
                if (!cambc::vision::in_vision(altar)) return false;
                hits = static_cast<int>(cambc::vision::altar_lock_at(altar).hits_remaining());
                return true;
            });
            cost += 2 * hits;
        }
        cost += 6 * enemies_near(altar, 4);
        if (!found || cost < best_cost) { best_cost = cost; best_path = path; best_altar = altar; found = true; }
    }
    if (!found) return false;

    const int round = static_cast<int>(cambc::round_number());
    const int rounds_left = static_cast<int>(cambc::constants::max_rounds) - round;
    const int channel = static_cast<int>((gems + 9) / 10);
    const unsigned ally_score = cambc::global_submitted_gems(team::ally);

    if (gems >= kBankGems
        || ally_score + gems >= cambc::constants::winning_gems                // winning tribute
        || rounds_left <= best_cost + channel + kTimeMargin                  // bank before the limit
        || (best_path <= 4 && gems >= 5))
        banking = true;
    if (!banking) return false;

    if (distance(me.position(), best_altar) <= range) {
        if (enemy_lock(best_altar) && !ally_lock(best_altar)) {
            // Clear the lock with basic attacks.
            if (me.can_attack()) {
                guarded([&] {
                    if (!cambc::vision::in_vision(best_altar)) return false;
                    const auto lock = cambc::vision::altar_lock_at(best_altar);
                    if (!me.attack_tiles().is_set(best_altar)) return false;
                    me.attack(lock);
                    attacked = true;
                    return true;
                });
            }
            return true; // wait next to the altar for the attack to come back
        }
        if (guarded([&] { me.submit_gems(best_altar); return true; })) return true;
    }

    cambc::bitboard goals;
    goals.clear();
    goals |= around(best_altar, range);
    if (move_towards_closest(goals)) return true;
    // Fallback: any altar ring.
    goals.clear();
    for (auto altar : cambc::map::altars().positions()) goals |= around(altar, range);
    return move_towards_closest(goals);
}
