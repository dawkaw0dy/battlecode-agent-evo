#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using cambc::champion_draft;

// Measure the map and choose one of three simple plans.
static Plan compute_plan() {
    Plan plan;
    const int w = cambc::map::width(), h = cambc::map::height();
    const int side = std::max(w, h);
    const auto spawns = cambc::map::spawn_region(cambc::team::ally).positions();
    int sx = 0, sy = 0;
    for (auto p : spawns) { sx += p.x; sy += p.y; }
    if (!spawns.empty()) { sx /= static_cast<int>(spawns.size()); sy /= static_cast<int>(spawns.size()); }
    const cambc::vec2 home{sx, sy};

    int clusters = 0, fighters = 0, near_big = 0;
    int nearest_altar = 1000;
    for (auto a : cambc::map::altars().positions())
        nearest_altar = std::min(nearest_altar, static_cast<int>(home.chebyshev_distance(a)));
    for (const auto& group : cambc::map::camp_groups()) {
        for (const auto& c : group.camps()) {
            const auto t = c.kind();
            const int d = static_cast<int>(home.chebyshev_distance(c.spawn_position()));
            if (t == cambc::camp_type::gem_cluster || t == cambc::camp_type::gold_cluster) ++clusters;
            else ++fighters;
            if (t == cambc::camp_type::golem && d <= side / 2) ++near_big;
        }
    }

    if (near_big > 0) plan.kind = PlanKind::big_camp;
    else if (side <= 35) plan.kind = PlanKind::aggro;
    else plan.kind = PlanKind::farm;

    // Bank more rarely when altars are far away; cluster-rich maps yield many small bags.
    plan.bank_threshold = static_cast<std::uint8_t>(std::clamp(nearest_altar / 2 + (clusters > fighters ? 14 : 8), 7, 40));
    // How much of our health a solo fight may cost.
    plan.danger_pct = plan.kind == PlanKind::aggro ? 55 : 45;
    plan.chase_range = plan.kind == PlanKind::aggro ? 5 : 2;
    return plan;
}

void play_draft() {
    static const Plan plan = compute_plan();
    static const std::array<champion_draft, 6> farm{champion_draft::vendetta, champion_draft::vesper_vale,
        champion_draft::ryth, champion_draft::varg, champion_draft::marcella, champion_draft::willow};
    static const std::array<champion_draft, 6> big{champion_draft::varg, champion_draft::vendetta,
        champion_draft::ryth, champion_draft::willow, champion_draft::marcella, champion_draft::vesper_vale};
    static const std::array<champion_draft, 6> aggro{champion_draft::vendetta, champion_draft::ryth,
        champion_draft::marcella, champion_draft::vesper_vale, champion_draft::varg, champion_draft::willow};
    const auto& order = plan.kind == PlanKind::big_camp ? big : plan.kind == PlanKind::aggro ? aggro : farm;

    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    champion_draft pick = order[0];
    bool found = false;
    for (auto c : order)
        if (std::find(chosen.begin(), chosen.end(), c) == chosen.end()) { pick = c; found = true; break; }
    if (!found)
        for (int i = 0; i < 11 && !found; ++i) {
            auto c = static_cast<champion_draft>(i);
            if (c != champion_draft::cassi_and_issac && std::find(chosen.begin(), chosen.end(), c) == chosen.end()) {
                pick = c;
                found = true;
            }
        }

    const std::array<std::uint8_t, 6> bytes{'R', static_cast<std::uint8_t>(plan.kind), plan.bank_threshold,
        plan.danger_pct, plan.chase_range, 0};
    cambc::draft_memory = bytes;
    cambc::draft::pick(pick);
}
