#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using cambc::champion_draft;
using cambc::camp_type;

namespace {

struct MapReport {
    int side = 0, team = 3, clusters = 0, fighters = 0, golems_near = 0, monolith_near = 0;
    int altar_dist = 0, home_dist = 0, free_groups = 0;
};

cambc::vec2 centroid(cambc::team side) {
    const auto tiles = cambc::map::spawn_region(side).positions();
    int sx = 0, sy = 0;
    for (auto p : tiles) { sx += p.x; sy += p.y; }
    const int n = std::max<int>(1, static_cast<int>(tiles.size()));
    return {sx / n, sy / n};
}

MapReport measure() {
    MapReport r;
    r.side = std::max(cambc::map::width(), cambc::map::height());
    r.team = static_cast<int>(cambc::draft::team_size());
    const auto home = centroid(cambc::team::ally);
    const auto away = centroid(cambc::team::enemy);
    r.home_dist = static_cast<int>(home.chebyshev_distance(away));
    r.altar_dist = 1000;
    for (auto a : cambc::map::altars().positions())
        r.altar_dist = std::min(r.altar_dist, static_cast<int>(home.chebyshev_distance(a)));
    for (const auto& group : cambc::map::camp_groups()) {
        bool fighter = false;
        for (const auto& c : group.camps()) {
            const auto t = c.kind();
            const int d = static_cast<int>(home.chebyshev_distance(c.spawn_position()));
            if (t == camp_type::gem_cluster || t == camp_type::gold_cluster) { ++r.clusters; continue; }
            fighter = true;
            ++r.fighters;
            if (t == camp_type::golem && d <= r.side / 2) ++r.golems_near;
            if (t == camp_type::monolith && d <= r.side / 2) ++r.monolith_near;
        }
        if (!fighter) ++r.free_groups;
    }
    return r;
}

Plan choose_plan(const MapReport& r) {
    Plan p;
    if (r.home_dist <= 32 && r.side <= 45) p.kind = PlanKind::aggro;
    else if (r.golems_near > 0 && r.team >= 4) p.kind = PlanKind::hunt;
    else p.kind = PlanKind::farm;

    const bool cluster_rich = r.clusters > r.fighters;
    p.bank = static_cast<std::uint8_t>(std::clamp(r.altar_dist / 2 + (cluster_rich ? 22 : 16), 16, 45));
    switch (p.kind) {
    case PlanKind::aggro:
        p.bank = static_cast<std::uint8_t>(std::min<int>(p.bank, 20));
        p.danger = 50; p.chase = 5; p.helpers = 0; p.cap = 2; p.golem_round = 50; p.recall = 30;
        break;
    case PlanKind::hunt:
        p.danger = 55; p.chase = 2; p.helpers = 1; p.cap = 3; p.golem_round = 0; p.recall = 33;
        break;
    case PlanKind::farm:
        p.danger = 45; p.chase = 2; p.helpers = 0; p.cap = cluster_rich ? 1 : 2; p.golem_round = 60; p.recall = 33;
        break;
    }
    p.shop_gold = 15;
    return p;
}

} // namespace

void play_draft() {
    static const MapReport report = measure();
    static const Plan plan = choose_plan(report);

    using C = champion_draft;
    const bool cluster_rich = report.clusters > report.fighters;
    std::vector<C> order;
    switch (plan.kind) {
    case PlanKind::hunt:
        order = {C::varg, C::ryth, C::vendetta, C::willow, C::heart_of_ruin, C::marcella, C::vesper_vale, C::tavi};
        break;
    case PlanKind::aggro:
        order = {C::vendetta, C::heart_of_ruin, C::vesper_vale, C::ryth, C::tavi, C::marcella, C::varg, C::willow};
        break;
    case PlanKind::farm:
        if (cluster_rich)
            order = {C::varg, C::vendetta, C::vesper_vale, C::ryth, C::heart_of_ruin, C::tavi, C::marcella, C::willow};
        else
            order = {C::vendetta, C::vesper_vale, C::ryth, C::varg, C::heart_of_ruin, C::marcella, C::tavi, C::willow};
        break;
    }

    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    auto taken = [&](C c) { return std::find(chosen.begin(), chosen.end(), c) != chosen.end(); };
    C pick = order[0];
    bool found = false;
    for (auto c : order)
        if (!taken(c)) { pick = c; found = true; break; }
    for (int i = 0; i <= static_cast<int>(C::heart_of_ruin) && !found; ++i) {
        const auto c = static_cast<C>(i);
        if (c != C::cassi_and_issac && !taken(c)) { pick = c; found = true; }
    }

    const std::array<Plan, 1> bytes{plan};
    cambc::draft_memory = bytes;
    cambc::draft::pick(pick);
}
