// Map-aware draft: score every champion for this map's camps and travel, pick the best one still free,
// and write an opening plan (per-group path distances from both spawns) into the draft memory.
#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using D = cambc::champion_draft;
constexpr int kChampions = 12;
constexpr int kOpeningRounds = 150;
constexpr float kContestedBias = 0.6f;
constexpr float kOwnSideBias = 1.25f;

// Fallback order when map analysis fails.
constexpr std::array kFallback{D::vesper_vale, D::marcella, D::vendetta, D::varg, D::ryth,
    D::tavi, D::willow, D::ren, D::cassi_and_issac, D::stellar, D::prism, D::heart_of_ruin};

struct MapFeatures {
    float mean_group_size = 1;  // monsters per group (gem/gold clusters included)
    float heavy_share = 0;      // share of camp health in elk and golems
    float mean_distance = 20;   // mean path distance from our spawn to a farmable group
    int groups = 0;
};

float camp_health(cambc::camp_type kind) {
    using C = cambc::camp_type;
    switch (kind) {
    case C::wolf: return 1350;
    case C::elk: return 2400;
    case C::gem_cluster: case C::gold_cluster: return 120;
    case C::monolith: return 20000;
    case C::golem: return 5000;
    }
    return 0;
}

cambc::bitboard terrain_blocked() {
    return cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
}

// Path distance from a spawn region to every group (255 when unreachable).
std::vector<std::uint8_t> group_distances(const std::vector<cambc::map::camp_group>& list, cambc::team side) {
    std::vector<std::uint8_t> result(list.size(), 255);
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(terrain_blocked(), cambc::map::spawn_region(side), visited);
    for (std::size_t i = 0; i < list.size(); ++i) {
        int best = 255;
        for (auto tile : list[i].spawn_tiles().positions())
            if (visited.is_set(tile)) best = std::min<int>(best, field.get(tile));
        result[i] = static_cast<std::uint8_t>(std::min(best, 254));
    }
    return result;
}

MapFeatures analyse(const std::vector<cambc::map::camp_group>& list, const std::vector<std::uint8_t>& ours) {
    MapFeatures f;
    float monsters = 0, health = 0, heavy = 0, dist = 0;
    int farmable = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        bool monolith = false;
        int count = 0;
        for (const auto& camp : list[i].camps()) {
            const auto kind = camp.kind();
            if (kind == cambc::camp_type::monolith) monolith = true;
            ++count;
            health += camp_health(kind);
            if (kind == cambc::camp_type::elk || kind == cambc::camp_type::golem) heavy += camp_health(kind);
        }
        if (monolith || ours[i] >= 254) continue;
        monsters += static_cast<float>(count);
        dist += ours[i];
        ++farmable;
    }
    f.groups = farmable;
    if (farmable > 0) {
        f.mean_group_size = monsters / static_cast<float>(farmable);
        f.mean_distance = dist / static_cast<float>(farmable);
    }
    if (health > 0) f.heavy_share = heavy / health;
    return f;
}

// Farming value of each champion on this map, after patches 100-102.
std::array<float, kChampions> champion_scores(const MapFeatures& f) {
    std::array<float, kChampions> s{};
    auto at = [&](D d) -> float& { return s[static_cast<int>(d)]; };
    // Base: early clear speed and survivability with this bot's kit usage (basic attacks + damage spells).
    at(D::vesper_vale) = 10.0f;    // 55 Strength ranged, 33% more gold: fastest scaling
    at(D::marcella) = 9.5f;        // 1200 HP, gold per tributed gem, 2x Focus spell
    at(D::vendetta) = 9.0f;        // 70 Strength melee, walks and hits, 1000 HP
    at(D::varg) = 8.5f;            // splash basic attacks
    at(D::ryth) = 8.0f;            // 1200 HP, area stun damage
    at(D::tavi) = 7.0f;            // 45 Strength, 900 HP, stun spell
    at(D::willow) = 6.0f;          // 55 Strength, heals allies
    at(D::ren) = 5.5f;
    at(D::cassi_and_issac) = 5.0f; // two bodies but linked death and weak scaling
    at(D::stellar) = 4.0f;
    at(D::prism) = 3.5f;
    at(D::heart_of_ruin) = 3.0f;

    // Large cardinally connected groups reward splash and area damage.
    const float big = std::clamp((f.mean_group_size - 2.0f) / 3.0f, 0.0f, 1.0f);
    at(D::varg) += 1.0f * big;
    at(D::ryth) += 0.8f * big;
    // Elk and golems hit hard: high-health champions last longer.
    const float heavy = std::clamp(f.heavy_share * 2.0f, 0.0f, 1.0f);
    at(D::ryth) += 0.5f * heavy;
    at(D::marcella) += 0.3f * heavy;
    // Long walks: Vendetta's dash and melee walk-and-hit keep tempo.
    const float far = std::clamp((f.mean_distance - 15.0f) / 25.0f, 0.0f, 1.0f);
    at(D::vendetta) += 0.6f * far;
    return s;
}

} // namespace

void play_draft() {
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    auto taken = [&](D d) { return std::find(chosen.begin(), chosen.end(), d) != chosen.end(); };

    std::array<float, kChampions> scores{};
    bool analysed = false;
    try {
        const auto list = cambc::map::camp_groups();
        const auto ours = group_distances(list, cambc::team::ally);
        const auto theirs = group_distances(list, cambc::team::enemy);
        scores = champion_scores(analyse(list, ours));
        analysed = true;

        // Opening plan: byte 0 = group count, then (ours, theirs) path distance per group.
        if (chosen.empty()) {
            std::array<std::uint8_t, 1024> plan{};
            const std::size_t n = std::min<std::size_t>(list.size(), 255);
            plan[0] = static_cast<std::uint8_t>(n);
            for (std::size_t i = 0; i < n && 2 * i + 2 < plan.size(); ++i) {
                plan[1 + 2 * i] = ours[i];
                plan[2 + 2 * i] = theirs[i];
            }
            cambc::draft_memory = plan;
        }
    } catch (...) {} // any failure falls back to the static order

    if (analysed) {
        int best = -1;
        for (int i = 0; i < kChampions; ++i) {
            if (taken(static_cast<D>(i))) continue;
            if (best < 0 || scores[i] > scores[best]) best = i;
        }
        if (best >= 0) {
            cambc::draft::pick(static_cast<D>(best));
            return;
        }
    }
    for (auto pick : kFallback) {
        if (!taken(pick)) {
            cambc::draft::pick(pick);
            return;
        }
    }
}

float opening_bias(int group, int round) {
    if (round > kOpeningRounds || group < 0) return 1.0f;
    static const std::vector<std::uint8_t> plan = [] {
        std::vector<std::uint8_t> bytes;
        try {
            const auto& memory = cambc::draft_memory;
            for (std::size_t i = 0; i < memory.size(); ++i) bytes.push_back(static_cast<std::uint8_t>(memory.data()[i]));
        } catch (...) {}
        return bytes;
    }();
    if (plan.empty() || group >= plan[0] || static_cast<std::size_t>(2 + 2 * group) >= plan.size()) return 1.0f;
    const int ours = plan[1 + 2 * group], theirs = plan[2 + 2 * group];
    if (ours >= 254) return 1.0f;
    // Groups the enemy reaches first are likely contested early; groups on our side are safe.
    if (theirs + 2 < ours) return kContestedBias;
    if (ours + 4 < theirs) return kOwnSideBias;
    return 1.0f;
}
