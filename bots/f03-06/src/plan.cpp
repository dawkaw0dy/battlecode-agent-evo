#include "plan.h"
#include <algorithm>

cambc::bitboard static_walls() {
    return cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
}

cambc::bitboard static_walls_with_camps() {
    auto b = static_walls();
    for (const auto& g : cambc::map::camp_groups())
        for (const auto& c : g.camps()) b.set(c.spawn_position());
    return b;
}

std::vector<GroupInfo> analyse_groups() {
    std::vector<GroupInfo> out;
    for (const auto& grp : cambc::map::camp_groups()) {
        GroupInfo g;
        g.gems = static_cast<int>(grp.gem_value());
        g.gold = static_cast<int>(grp.gold_value());
        g.respawn = grp.max_respawn_timer();
        long sx = 0, sy = 0;
        for (const auto& c : grp.camps()) {
            const vec2 p = c.spawn_position();
            const auto k = c.kind();
            g.tiles.push_back(p);
            g.kinds.push_back(k);
            const auto s = camp_spec(k);
            g.hp += s.hp;
            if (!is_cluster(k)) g.str += s.str;
            g.range = std::max(g.range, s.range);
            if (k == cambc::camp_type::monolith || k == cambc::camp_type::golem) g.boss = true;
            if (k == cambc::camp_type::monolith) g.monolith = true;
            sx += p.x;
            sy += p.y;
        }
        if (g.tiles.empty()) { out.push_back(g); continue; }
        const int n = static_cast<int>(g.tiles.size());
        const vec2 c{static_cast<int>(sx / n), static_cast<int>(sy / n)};
        g.center = g.tiles[0];
        for (auto t : g.tiles)
            if (cheb(t, c) < cheb(g.center, c)) g.center = t;
        out.push_back(std::move(g));
    }
    return out;
}

void measure_spawn_distances(std::vector<GroupInfo>& groups) {
    const auto walls = static_walls_with_camps();
    for (int side = 0; side < 2; ++side) {
        const team t = side == 0 ? team::ally : team::enemy;
        auto w = walls;
        // Our own walls exclude the enemy spawn; theirs exclude ours.
        if (t == team::enemy) w = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
            | cambc::map::spawn_region(team::ally);
        for (const auto& g : groups)
            for (auto p : g.tiles) w.set(p, true);
        cambc::distance_field f;
        cambc::bitboard visited;
        f.compute(w & ~cambc::map::spawn_region(t), cambc::map::spawn_region(t), visited);
        Field lf;
        lf.load(f);
        for (auto& g : groups) {
            int best = kFar;
            for (auto p : g.tiles)
                for (auto d : kDirs) {
                    const vec2 q{p.x + d.x, p.y + d.y};
                    const int v = lf.at(q);
                    if (v != kFar && v + 1 < best) best = v + 1;
                }
            (side == 0 ? g.d_ally : g.d_enemy) = best;
        }
    }
}
