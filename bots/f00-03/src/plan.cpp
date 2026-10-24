#include "plan.h"
#include <algorithm>
#include <cmath>
#include <numeric>

using cambc::vec2;
using cambc::team;

namespace {

struct CampDesign { float hp, str; int range; };

CampDesign design(cambc::camp_type type) {
    switch (type) {
    case cambc::camp_type::wolf: return {1350, 20, 3};
    case cambc::camp_type::elk: return {2400, 60, 4};
    case cambc::camp_type::gem_cluster: return {120, 0, 0};
    case cambc::camp_type::gold_cluster: return {120, 0, 0};
    case cambc::camp_type::monolith: return {20000, 200, 5};
    case cambc::camp_type::golem: return {5000, 120, 4};
    }
    return {1000, 50, 3};
}

vec2 region_center(const cambc::bitboard& board) {
    long sx = 0, sy = 0, n = 0;
    for (auto p : board.positions()) { sx += p.x; sy += p.y; ++n; }
    if (!n) return {0, 0};
    return {static_cast<int>(sx / n), static_cast<int>(sy / n)};
}

int min_over(const cambc::distance_field& field, const cambc::bitboard& visited, const std::vector<vec2>& tiles) {
    int best = 9999;
    for (auto t : tiles)
        if (visited.is_set(t)) best = std::min<int>(best, field.get(t));
    return best;
}

} // namespace

const cambc::bitboard& static_walls_ally() {
    static const cambc::bitboard walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    return walls;
}

const cambc::bitboard& dash_walls_ally() {
    static const cambc::bitboard walls = [] {
        const auto walkable = ~static_walls_ally();
        const auto& holes = cambc::map::holes();
        cambc::bitboard thin;
        thin.clear();
        const int d[2][2] = {{1, 0}, {0, 1}};  // straight jumps only: diagonal ones can be corner-blocked
        for (const auto& v : d) {
            const auto a = walkable.shift_right(v[0]).shift_up(v[1]);
            const auto b = walkable.shift_right(-v[0]).shift_up(-v[1]);
            thin |= holes & a & b;
        }
        return static_walls_ally() & ~thin;
    }();
    return walls;
}

std::vector<GroupData> analyse_groups() {
    std::vector<GroupData> out;
    const auto groups = cambc::map::camp_groups();
    auto walls_a = static_walls_ally();
    auto walls_e = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::ally);
    cambc::distance_field fa, fe, fd;
    cambc::bitboard va, ve, vd;
    fa.compute(walls_a, cambc::map::spawn_region(team::ally), va);
    fd.compute(dash_walls_ally(), cambc::map::spawn_region(team::ally), vd);
    fe.compute(walls_e, cambc::map::spawn_region(team::enemy), ve);
    for (const auto& g : groups) {
        GroupData d;
        long sx = 0, sy = 0;
        for (const auto& c : g.camps()) {
            auto p = c.spawn_position();
            d.tiles.push_back(p);
            sx += p.x; sy += p.y;
            auto des = design(c.kind());
            d.hp += des.hp;
            d.dps += des.str;
            d.range = std::max(d.range, des.range);
            if (c.kind() == cambc::camp_type::monolith) d.has_monolith = true;
        }
        if (!d.tiles.empty())
            d.center = {static_cast<int>(sx / static_cast<long>(d.tiles.size())), static_cast<int>(sy / static_cast<long>(d.tiles.size()))};
        d.gems = static_cast<int>(g.gem_value());
        d.gold = static_cast<int>(g.gold_value());
        d.respawn = g.max_respawn_timer();
        d.dist_ally = min_over(fa, va, d.tiles);
        d.dist_enemy = min_over(fe, ve, d.tiles);
        d.dist_ally_dash = min_over(fd, vd, d.tiles);
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<std::uint8_t> split_regions(const std::vector<GroupData>& groups, int slots, bool dash_map) {
    std::vector<std::uint8_t> owner(groups.size(), 255);
    if (slots <= 0 || groups.empty()) return owner;
    const vec2 home = region_center(cambc::map::spawn_region(team::ally));
    const vec2 away = region_center(cambc::map::spawn_region(team::enemy));
    const double fx = away.x - home.x, fy = away.y - home.y;

    // Our half of the map plus contested ground; solo-impossible groups stay unassigned.
    std::vector<int> eligible;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        const auto& g = groups[i];
        if (g.has_monolith || g.dps >= 120) continue;
        const int da = dash_map ? g.dist_ally_dash : g.dist_ally;
        if (da >= 9999) continue;
        if (da <= g.dist_enemy + 2 || (dash_map && g.dist_enemy >= 9999)) eligible.push_back(i);
    }
    if (eligible.empty()) return owner;

    // Sort by bearing from our spawn relative to the enemy direction, then cut into equal-value sectors.
    std::vector<std::pair<double, int>> order;
    for (int i : eligible) {
        const double dx = groups[i].center.x - home.x, dy = groups[i].center.y - home.y;
        const double angle = std::atan2(fx * dy - fy * dx, fx * dx + fy * dy);
        order.push_back({angle, i});
    }
    std::sort(order.begin(), order.end());
    std::vector<double> weight;
    double total = 0;
    for (auto& [a, i] : order) {
        const auto& g = groups[i];
        const double w = g.gems + 0.3 * g.gold + 4.0 + g.hp / 400.0;
        weight.push_back(w);
        total += w;
    }
    const int regions = std::min<int>(slots, static_cast<int>(order.size()));
    std::vector<int> chunk(order.size());
    double acc = 0;
    for (size_t k = 0; k < order.size(); ++k) {
        const double mid = acc + weight[k] / 2;
        chunk[k] = std::min(regions - 1, static_cast<int>(mid / total * regions));
        acc += weight[k];
    }
    // Hardest sector to the strongest slot.
    std::vector<double> difficulty(regions, 0);
    for (size_t k = 0; k < order.size(); ++k) difficulty[chunk[k]] += groups[order[k].second].hp * (1 + groups[order[k].second].dps / 20.0);
    std::vector<int> rank(regions);
    std::iota(rank.begin(), rank.end(), 0);
    std::stable_sort(rank.begin(), rank.end(), [&](int a, int b) { return difficulty[a] > difficulty[b]; });
    std::vector<int> slot_of(regions);
    for (int s = 0; s < regions; ++s) slot_of[rank[s]] = s;
    for (size_t k = 0; k < order.size(); ++k) owner[order[k].second] = static_cast<std::uint8_t>(slot_of[chunk[k]]);
    return owner;
}

bool is_dash_map(const std::vector<GroupData>& groups) {
    float walk = 0, dash = 0;
    for (const auto& g : groups) {
        const float v = g.gems + 0.3f * g.gold;
        if (g.dist_ally < 9999) walk += v;
        if (g.dist_ally_dash < 9999) dash += v;
    }
    return dash > 0 && walk < 0.6f * dash;
}

std::vector<cambc::champion_draft> preferred_picks(int team_size, bool dash_map) {
    using cd = cambc::champion_draft;
    std::vector<cd> all{cd::vendetta, cd::marcella, cd::vesper_vale, cd::tavi, cd::ryth, cd::varg};
    // Most camps behind one-tile holes: take the champions that can dash over them.
    if (dash_map) all = {cd::vendetta, cd::vesper_vale, cd::heart_of_ruin, cd::ren, cd::marcella, cd::tavi};
    all.resize(std::min<size_t>(all.size(), std::max(team_size, 1)));
    return all;
}
