#include "plan.h"
#include <algorithm>
#include <cmath>

using cambc::vec2;
using cambc::team;

namespace {

struct Design { float hp, str; int range; };

Design design(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return {1350, 20, 3};
    case cambc::camp_type::elk: return {2400, 60, 4};
    case cambc::camp_type::gem_cluster: return {120, 0, 0};
    case cambc::camp_type::gold_cluster: return {120, 0, 0};
    case cambc::camp_type::monolith: return {20000, 200, 5};
    case cambc::camp_type::golem: return {5000, 120, 4};
    }
    return {1500, 50, 4};
}

vec2 center_of(const cambc::bitboard& b) {
    long sx = 0, sy = 0, n = 0;
    for (auto p : b.positions()) { sx += p.x; sy += p.y; ++n; }
    if (!n) return {0, 0};
    return {static_cast<int>(sx / n), static_cast<int>(sy / n)};
}

int min_dist(const cambc::distance_field& f, const cambc::bitboard& reached, const std::vector<vec2>& tiles) {
    int best = 9999;
    for (auto t : tiles)
        if (reached.is_set(t)) best = std::min<int>(best, f.get(t));
    return best;
}

} // namespace

const cambc::bitboard& walk_walls() {
    static const cambc::bitboard w = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    return w;
}

const cambc::bitboard& dash_walls() {
    static const cambc::bitboard w = [] {
        const auto open = ~walk_walls();
        cambc::bitboard thin;
        thin.clear();
        // Straight jumps only: diagonal dashes can be refused at corners.
        thin |= cambc::map::holes() & open.shift_right(1) & open.shift_right(-1);
        thin |= cambc::map::holes() & open.shift_up(1) & open.shift_up(-1);
        return walk_walls() & ~thin;
    }();
    return w;
}

float group_value(const GroupInfo& g) { return g.gems + 0.3f * g.gold; }

std::vector<GroupInfo> analyse_groups() {
    std::vector<GroupInfo> out;
    const auto groups = cambc::map::camp_groups();
    const auto ally_walls = walk_walls();
    const auto enemy_walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::ally);
    cambc::distance_field fa, fe, fd;
    cambc::bitboard ra, re, rd;
    fa.compute(ally_walls, cambc::map::spawn_region(team::ally), ra);
    fe.compute(enemy_walls, cambc::map::spawn_region(team::enemy), re);
    fd.compute(dash_walls(), cambc::map::spawn_region(team::ally), rd);
    for (const auto& g : groups) {
        GroupInfo d;
        long sx = 0, sy = 0;
        for (const auto& c : g.camps()) {
            const vec2 p = c.spawn_position();
            d.tiles.push_back(p);
            sx += p.x;
            sy += p.y;
            const auto des = design(c.kind());
            if (des.str > 0) d.fight_hp += des.hp;
            else d.soft_hp += des.hp;
            d.str += des.str;
            d.range = std::max(d.range, des.range);
            if (c.kind() == cambc::camp_type::monolith || c.kind() == cambc::camp_type::golem) d.boss = true;
        }
        if (!d.tiles.empty()) {
            const long n = static_cast<long>(d.tiles.size());
            d.center = {static_cast<int>(sx / n), static_cast<int>(sy / n)};
        }
        d.gems = static_cast<int>(g.gem_value());
        d.gold = static_cast<int>(g.gold_value());
        d.respawn = g.max_respawn_timer();
        d.d_ally = min_dist(fa, ra, d.tiles);
        d.d_enemy = min_dist(fe, re, d.tiles);
        d.d_dash = min_dist(fd, rd, d.tiles);
        out.push_back(std::move(d));
    }
    return out;
}

bool is_dash_map(const std::vector<GroupInfo>& groups) {
    float walk = 0, dash = 0;
    for (const auto& g : groups) {
        if (g.d_ally < 9999) walk += group_value(g);
        if (g.d_dash < 9999) dash += group_value(g);
    }
    return dash > 0 && walk < 0.6f * dash;
}

std::vector<cambc::champion_draft> preferred_picks(int team_size, bool dash_map) {
    using cd = cambc::champion_draft;
    // Fast clearers: a strong basic attack with a reset, a tank, and a healer to keep the squad fighting.
    std::vector<cd> all{cd::vendetta, cd::ryth, cd::willow, cd::vesper_vale, cd::marcella, cd::tavi, cd::varg};
    if (dash_map) all = {cd::vendetta, cd::vesper_vale, cd::heart_of_ruin, cd::ren, cd::marcella, cd::tavi};
    all.resize(std::min<size_t>(all.size(), static_cast<size_t>(std::max(team_size, 1))));
    return all;
}

std::vector<std::uint8_t> split_groups(const std::vector<GroupInfo>& groups, int team_size, bool dash_map) {
    std::vector<std::uint8_t> owner(groups.size(), 255);
    const int squads = std::max(1, team_size);
    std::vector<int> eligible;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        const auto& g = groups[i];
        if (g.boss || group_value(g) <= 0) continue;
        const int da = dash_map ? g.d_dash : g.d_ally;
        if (da >= 9999) continue;
        if (da <= g.d_enemy + 4 || g.d_enemy >= 9999) eligible.push_back(i);
    }
    if (eligible.empty()) return owner;
    if (squads == 1) {
        for (int i : eligible) owner[i] = 0;
        return owner;
    }
    // One sector per champion by bearing from our spawn, relative to the enemy direction, of equal value.
    const vec2 home = center_of(cambc::map::spawn_region(team::ally));
    const vec2 away = center_of(cambc::map::spawn_region(team::enemy));
    const double fx = away.x - home.x, fy = away.y - home.y;
    std::vector<std::pair<double, int>> order;
    double total = 0;
    for (int i : eligible) {
        const double dx = groups[i].center.x - home.x, dy = groups[i].center.y - home.y;
        order.push_back({std::atan2(fx * dy - fy * dx, fx * dx + fy * dy), i});
        total += group_value(groups[i]);
    }
    std::sort(order.begin(), order.end());
    double acc = 0;
    for (auto& [a, i] : order) {
        const double v = group_value(groups[i]) + 4.0;
        const double mid = acc + v / 2;
        owner[i] = static_cast<std::uint8_t>(std::min(squads - 1, static_cast<int>(mid / (total + 4.0 * order.size()) * squads)));
        acc += v;
    }
    return owner;
}
