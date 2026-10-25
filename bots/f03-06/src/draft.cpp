#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

using cd = cambc::champion_draft;

namespace {

bool g_written = false;
std::vector<cd> g_picks;

// Picks in slot order for the chosen plan.
std::vector<cd> choose_picks(Plan plan, int size) {
    std::vector<cd> order;
    if (plan == kBoss)
        order = {cd::vendetta, cd::marcella, cd::varg, cd::ryth, cd::vesper_vale};
    else
        order = {cd::vendetta, cd::willow, cd::varg, cd::marcella, cd::vesper_vale};
    order.resize(static_cast<size_t>(std::min<int>(size, 5)));
    return order;
}

int bodies(const std::vector<cd>& picks) {
    int n = 0;
    for (auto p : picks) n += p == cd::cassi_and_issac ? 2 : 1;
    return n;
}

// Our-side groups split into angular sectors of equal value around our spawn, one per body.
std::vector<std::uint8_t> assign_owners(const std::vector<GroupInfo>& g, int nb) {
    std::vector<std::uint8_t> owner(g.size(), 255);
    const auto spawn = cambc::map::spawn_region(team::ally).positions();
    if (spawn.empty() || nb <= 0) return owner;
    float cx = 0, cy = 0;
    for (auto p : spawn) { cx += p.x; cy += p.y; }
    cx /= spawn.size();
    cy /= spawn.size();
    std::vector<std::pair<float, int>> mine;
    float total = 0;
    for (size_t i = 0; i < g.size(); ++i) {
        if (g[i].tiles.empty() || g[i].d_ally >= kFar) continue;
        if (g[i].d_ally > g[i].d_enemy + 2) continue;
        if (g[i].monolith) continue;
        const float a = std::atan2(g[i].center.y - cy, g[i].center.x - cx);
        mine.push_back({a, static_cast<int>(i)});
        total += g[i].gems + 0.3f * g[i].gold + 5;
    }
    if (mine.empty()) return owner;
    std::sort(mine.begin(), mine.end());
    // Rotate so the largest angular gap is the seam between the first and last sector.
    size_t seam = 0;
    float gap = -1;
    for (size_t i = 0; i < mine.size(); ++i) {
        const float a = mine[i].first, b = i + 1 < mine.size() ? mine[i + 1].first : mine[0].first + 6.2831853f;
        if (b - a > gap) { gap = b - a; seam = (i + 1) % mine.size(); }
    }
    std::rotate(mine.begin(), mine.begin() + static_cast<long>(seam), mine.end());
    float acc = 0;
    for (auto& [a, i] : mine) {
        const float v = g[static_cast<size_t>(i)].gems + 0.3f * g[static_cast<size_t>(i)].gold + 5;
        int slot = static_cast<int>((acc + v * 0.5f) / total * nb);
        slot = std::clamp(slot, 0, nb - 1);
        owner[static_cast<size_t>(i)] = static_cast<std::uint8_t>(slot);
        acc += v;
    }
    return owner;
}

// Champions needed so that no one tanks more than ~600 health of a group's damage.
int needed(const GroupInfo& g, int nb) {
    if (g.str <= 0) return 1;
    const float x = g.str * g.hp * 0.7f / (70.0f * 600.0f);
    return std::clamp(static_cast<int>(std::ceil(std::sqrt(x))), 1, nb);
}

// Squad id per body slot: Cassi and Issac stay together; squads as even as possible.
std::vector<std::uint8_t> assign_squads(const std::vector<cd>& picks, int nsq) {
    std::vector<std::uint8_t> sq;
    std::vector<int> size(static_cast<size_t>(nsq), 0);
    for (auto p : picks) {
        const int w = p == cd::cassi_and_issac ? 2 : 1;
        int best = 0;
        for (int i = 1; i < nsq; ++i)
            if (size[static_cast<size_t>(i)] < size[static_cast<size_t>(best)]) best = i;
        size[static_cast<size_t>(best)] += w;
        for (int k = 0; k < w; ++k) sq.push_back(static_cast<std::uint8_t>(best));
    }
    return sq;
}

void write_plan() {
    const int size = static_cast<int>(cambc::draft::team_size());
    auto groups = analyse_groups();
    measure_spawn_distances(groups);

    // Read the map: value on our side, and whether a monolith is the main source of gems.
    float side_gem_rate = 0;  // gems per round from non-boss groups on our side, if farmed perfectly
    int boss = -1;
    int boss_d = kFar;
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        if (g.tiles.empty() || g.d_ally >= kFar) continue;
        if (g.monolith) {
            if (g.d_ally <= g.d_enemy + 4 && g.d_ally < boss_d) { boss = static_cast<int>(i); boss_d = g.d_ally; }
            continue;
        }
        if (g.d_ally <= g.d_enemy) side_gem_rate += static_cast<float>(g.gems) / std::max(40, g.respawn);
    }
    Plan plan = kFarm;
    if (boss >= 0 && side_gem_rate < 0.5f) plan = kBoss;

    g_picks = choose_picks(plan, size);
    const int nb = bodies(g_picks);
    // Squad size: the value-weighted median of what our groups need.
    std::vector<std::pair<int, float>> need;
    float wsum = 0;
    for (const auto& g : groups) {
        if (g.tiles.empty() || g.d_ally >= kFar || g.monolith || g.d_ally > g.d_enemy + 2) continue;
        const float w = g.gems + 0.3f * g.gold;
        need.push_back({needed(g, nb), w});
        wsum += w;
    }
    std::sort(need.begin(), need.end());
    int squad_size = 2;
    float acc = 0;
    for (auto& [k, w] : need) {
        acc += w;
        if (acc >= wsum * 0.5f) { squad_size = k; break; }
    }
    squad_size = std::clamp(squad_size, 2, std::max(2, nb));  // nobody farms alone: lone champions get picked off
    // Large maps: travel dominates, so cover more ground with pairs.
    if (cambc::map::width() * cambc::map::height() >= 55 * 55) squad_size = std::min(squad_size, 2);
    int nsq = std::clamp(nb / squad_size, 1, 4);
    // Small maps are fought over: split squads get caught alone, so move as one team.
    const int area = cambc::map::width() * cambc::map::height();
    if (area <= 45 * 45) nsq = 1;
    const auto squads = assign_squads(g_picks, nsq);
    const auto owner = assign_owners(groups, nsq);

    std::array<std::byte, 1024> mem{};
    mem[dm::magic] = std::byte{'M'};
    mem[dm::plan] = static_cast<std::byte>(plan);
    mem[dm::npicks] = static_cast<std::byte>(g_picks.size());
    for (size_t i = 0; i < g_picks.size() && i < 5; ++i)
        mem[static_cast<size_t>(dm::picks) + i] = static_cast<std::byte>(static_cast<int>(g_picks[i]));
    const size_t n = std::min<size_t>(groups.size(), 1024 - dm::owner);
    mem[dm::ngroups] = static_cast<std::byte>(std::min<size_t>(n, 255));
    mem[dm::boss] = static_cast<std::byte>(boss >= 0 && plan == kBoss ? boss : 255);
    mem[dm::raid_turns] = static_cast<std::byte>(6);
    mem[dm::nsquads] = static_cast<std::byte>(nsq);
    for (size_t i = 0; i < squads.size() && i < 6; ++i) mem[static_cast<size_t>(dm::squad) + i] = static_cast<std::byte>(squads[i]);
    for (size_t i = 0; i < n; ++i) mem[static_cast<size_t>(dm::owner) + i] = static_cast<std::byte>(owner[i]);
    cambc::draft_memory = std::span<const std::byte>{mem.data(), mem.size()};
}

}  // namespace

void play_draft() {
    init_dims();
    if (!g_written) {
        g_written = true;
        write_plan();
    }
    const auto chosen = cambc::draft::current_draft(team::ally);
    for (auto p : g_picks)
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end()) {
            cambc::draft::pick(p);
            return;
        }
    for (int i = 0; i <= static_cast<int>(cd::heart_of_ruin); ++i) {
        const auto p = static_cast<cd>(i);
        if (p == cd::cassi_and_issac) continue;
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end()) {
            cambc::draft::pick(p);
            return;
        }
    }
}
