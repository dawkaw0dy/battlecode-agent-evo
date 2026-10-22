#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <vector>

namespace {

using cambc::vec2;
using cambc::team;

int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

vec2 centroid(const cambc::bitboard& tiles) {
    long sx = 0, sy = 0, n = 0;
    for (auto p : tiles.positions()) { sx += p.x; sy += p.y; ++n; }
    if (n == 0) return vec2{0, 0};
    return vec2{static_cast<int>(sx / n), static_cast<int>(sy / n)};
}

// Split the camp groups into one region per champion: seeds are spread by farthest-point sampling over
// groups on our half of the map, every group joins its nearest seed, and each region gets its nearest altar.
std::array<std::byte, plan::kSize> make_plan(int regions, const std::vector<cambc::champion_draft>& slots) {
    std::array<std::byte, plan::kSize> memory{};
    memory.fill(std::byte{0xFF});
    memory[plan::kMagic] = std::byte{'S'};
    memory[plan::kRegions] = static_cast<std::byte>(regions);
    for (std::size_t k = 0; k < slots.size() && static_cast<int>(k) < regions; ++k) {
        const int index = static_cast<int>(slots[k]);
        if (index >= 0 && index < plan::kDraftCount) memory[plan::kSlotOf + index] = static_cast<std::byte>(k);
    }

    const vec2 own = centroid(cambc::map::spawn_region(team::ally));
    const vec2 enemy = centroid(cambc::map::spawn_region(team::enemy));
    std::vector<vec2> centers;
    std::vector<bool> monolith;
    for (const auto& group : cambc::map::camp_groups()) {
        centers.push_back(centroid(group.spawn_tiles()));
        bool big = false;
        for (const auto& camp : group.camps()) big |= camp.kind() == cambc::camp_type::monolith;
        monolith.push_back(big);
    }
    const int count = static_cast<int>(centers.size());
    memory[plan::kGroupCount] = static_cast<std::byte>(count & 0xFF);

    std::vector<int> candidates;
    for (int i = 0; i < count; ++i)
        if (!monolith[i] && distance(own, centers[i]) <= distance(enemy, centers[i])) candidates.push_back(i);
    if (static_cast<int>(candidates.size()) < regions) {
        candidates.clear();
        for (int i = 0; i < count; ++i) if (!monolith[i]) candidates.push_back(i);
    }
    if (candidates.empty()) for (int i = 0; i < count; ++i) candidates.push_back(i);
    if (candidates.empty()) return memory;

    std::vector<vec2> seeds;
    int first = candidates[0];
    for (int i : candidates) if (distance(own, centers[i]) < distance(own, centers[first])) first = i;
    seeds.push_back(centers[first]);
    while (static_cast<int>(seeds.size()) < regions) {
        int best = -1, best_gap = -1;
        for (int i : candidates) {
            int gap = std::numeric_limits<int>::max();
            for (auto s : seeds) gap = std::min(gap, distance(s, centers[i]));
            if (gap > best_gap) { best_gap = gap; best = i; }
        }
        if (best < 0 || best_gap <= 0) break;
        seeds.push_back(centers[best]);
    }
    // Fewer distinct seeds than champions: reuse them so every slot still has a region.
    const int distinct = static_cast<int>(seeds.size());
    for (int k = distinct; k < regions; ++k) seeds.push_back(seeds[k % distinct]);

    for (int i = 0; i < count && plan::kGroupRegion + i < plan::kSize; ++i) {
        int best = 0;
        for (int k = 1; k < distinct; ++k)
            if (distance(seeds[k], centers[i]) < distance(seeds[best], centers[i])) best = k;
        memory[plan::kGroupRegion + i] = static_cast<std::byte>(best);
    }

    const auto altars = cambc::map::altars().positions();
    for (int k = 0; k < regions && k < plan::kMaxRegions; ++k) {
        if (altars.empty()) break;
        vec2 best = altars[0];
        for (auto a : altars) if (distance(a, seeds[k]) < distance(best, seeds[k])) best = a;
        memory[plan::kAltar + 2 * k] = static_cast<std::byte>(best.x);
        memory[plan::kAltar + 2 * k + 1] = static_cast<std::byte>(best.y);
    }
    // Duplicate seeds share a region index so their champions farm the same groups.
    for (int k = distinct; k < regions && k < plan::kMaxRegions; ++k) {
        memory[plan::kAltar + 2 * k] = memory[plan::kAltar + 2 * (k % distinct)];
        memory[plan::kAltar + 2 * k + 1] = memory[plan::kAltar + 2 * (k % distinct) + 1];
    }
    return memory;
}

} // namespace

void play_draft() {
    // Self-sufficient farmers first: strong basic attacks, health and sustain.
    using D = cambc::champion_draft;
    constexpr std::array preference{D::vendetta, D::vesper_vale, D::ryth, D::marcella, D::varg,
        D::tavi, D::heart_of_ruin, D::ren, D::willow, D::cassi_and_issac, D::stellar, D::prism};
    auto chosen = cambc::draft::current_draft(team::ally);
    D pick = preference[0];
    for (auto option : preference) {
        if (std::find(chosen.begin(), chosen.end(), option) == chosen.end()) { pick = option; break; }
    }

    try {
        int regions = static_cast<int>(cambc::draft::team_size());
        regions = std::clamp(regions, 1, plan::kMaxRegions);
        chosen.push_back(pick);
        cambc::draft_memory = make_plan(regions, chosen);
    } catch (const cambc::error&) {
        // The plan is optional: champions fall back to farming the whole map.
    }
    cambc::draft::pick(pick);
}
