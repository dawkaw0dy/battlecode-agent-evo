#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

// The draft sees the whole map with 10x fuel: read the camp layout to pick champions, and precompute
// the opening into draft memory (layout in draft.h): the order in which the team clears camp groups,
// and the order in which champions lock altars.
namespace {

using cambc::vec2;
using cambc::team;

float group_health(const cambc::map::camp_group& group, bool& unsafe) {
    float total = 0;
    for (const auto& camp : group.camps()) {
        switch (camp.kind()) {
        case cambc::camp_type::wolf: total += 1350; break;
        case cambc::camp_type::elk: total += 2400; break;
        case cambc::camp_type::gem_cluster: total += 120; break;
        case cambc::camp_type::gold_cluster: total += 120; break;
        case cambc::camp_type::golem: total += 5000; unsafe = true; break;
        case cambc::camp_type::monolith: total += 20000; unsafe = true; break;
        }
    }
    return total;
}

cambc::bitboard terrain() {
    return cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(team::enemy);
}

int field_distance(const cambc::distance_field& field, const cambc::bitboard& visited, const cambc::bitboard& tiles) {
    int best = std::numeric_limits<int>::max();
    for (auto tile : tiles.positions())
        if (visited.is_set(tile)) best = std::min<int>(best, field.get(tile));
    return best;
}

cambc::bitboard around(vec2 p, int r) {
    cambc::bitboard tiles;
    tiles.clear();
    const int w = cambc::map::width(), h = cambc::map::height();
    for (int x = std::max(0, p.x - r); x <= std::min(w - 1, p.x + r); ++x)
        for (int y = std::max(0, p.y - r); y <= std::min(h - 1, p.y + r); ++y)
            tiles.set(vec2{x, y}, true);
    return tiles;
}

void write_plan() {
    std::array<std::uint8_t, kPlanBytes> plan{};
    plan[kPlanMagic] = kMagic;
    const auto groups = cambc::map::camp_groups();
    const auto blocked = terrain();

    // Opening route: greedy chain of the best reward per turn, starting at our spawn.
    cambc::bitboard origin = cambc::map::spawn_region(team::ally);
    std::vector<bool> used(groups.size(), false);
    const float team_dps = 250.0f;
    int count = 0;
    for (; count < kRouteMax; ++count) {
        cambc::distance_field field;
        cambc::bitboard visited;
        field.compute(blocked, origin, visited);
        int best = -1;
        float best_score = 0;
        for (std::size_t i = 0; i < groups.size() && i < 255; ++i) {
            if (used[i]) continue;
            bool unsafe = false;
            const float health = group_health(groups[i], unsafe);
            if (unsafe) continue;
            const int dist = field_distance(field, visited, groups[i].spawn_tiles());
            if (dist == std::numeric_limits<int>::max()) continue;
            const float value = groups[i].gem_value() * 3.0f + groups[i].gold_value();
            const float score = value / (dist + health / team_dps + 8.0f);
            if (best < 0 || score > best_score) { best = static_cast<int>(i); best_score = score; }
        }
        if (best < 0) break;
        used[best] = true;
        plan[kPlanRoute + count] = static_cast<std::uint8_t>(best + 1);
        origin = groups[best].spawn_tiles();
    }
    plan[kPlanRouteLen] = static_cast<std::uint8_t>(count);

    // Lock order: altars by walking distance from our spawn (each is reached from a free neighbour).
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(blocked, cambc::map::spawn_region(team::ally), visited);
    const auto altars = cambc::map::altars().positions();
    std::vector<std::pair<int, int>> order;
    for (std::size_t i = 0; i < altars.size() && i < 255; ++i) {
        const int dist = field_distance(field, visited, around(altars[i], 1) & ~blocked);
        if (dist == std::numeric_limits<int>::max()) continue;
        order.push_back({dist, static_cast<int>(i)});
    }
    std::sort(order.begin(), order.end());
    const int altars_n = std::min<int>(kAltarMax, static_cast<int>(order.size()));
    for (int k = 0; k < altars_n; ++k) plan[kPlanAltars + k] = static_cast<std::uint8_t>(order[k].second + 1);
    plan[kPlanAltarLen] = static_cast<std::uint8_t>(altars_n);
    cambc::draft_memory = plan;
}

} // namespace

void play_draft() {
    // Marcella (2 gold per tributed gem, stun-immune) and Vesper Vale (33% more gold) fund early locks
    // and shards; then Varg when the map has large camp groups (splash), otherwise Vendetta; then stuns.
    using D = cambc::champion_draft;
    const auto groups = cambc::map::camp_groups();
    int camps = 0, big = 0;
    for (const auto& group : groups) {
        const int n = static_cast<int>(group.camps().size());
        camps += n;
        if (n >= 3) ++big;
    }
    const bool splash = !groups.empty() && (big * 3 >= static_cast<int>(groups.size()));
    std::vector<D> preference{D::marcella, D::vesper_vale};
    if (splash) { preference.push_back(D::varg); preference.push_back(D::vendetta); }
    else { preference.push_back(D::vendetta); preference.push_back(D::varg); }
    for (auto d : {D::ryth, D::tavi, D::willow, D::ren, D::cassi_and_issac, D::heart_of_ruin, D::stellar, D::prism})
        preference.push_back(d);

    try {
        write_plan();
    } catch (const cambc::error&) {
    }
    const auto chosen = cambc::draft::current_draft(team::ally);
    for (auto pick : preference) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
