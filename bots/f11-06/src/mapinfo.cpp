#include "mapinfo.h"

const bitboard& static_walls() {
    static const bitboard walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    return walls;
}

namespace {

vec2 region_center(const bitboard& region) {
    const auto tiles = region.positions();
    if (tiles.empty()) return {0, 0};
    long sx = 0, sy = 0;
    for (auto t : tiles) sx += t.x, sy += t.y;
    return {static_cast<int>(sx / static_cast<long>(tiles.size())), static_cast<int>(sy / static_cast<long>(tiles.size()))};
}

MapInfo analyse() {
    MapInfo m;
    m.w = cambc::map::width();
    m.h = cambc::map::height();
    const auto& ally_spawn = cambc::map::spawn_region(team::ally);
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    m.spawn_center = region_center(ally_spawn);
    m.enemy_center = region_center(enemy_spawn);

    const auto terrain = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    auto& fa = m.home;
    auto& va = m.home_reached;
    cambc::distance_field fe;
    bitboard ve;
    fa.compute(terrain | enemy_spawn, ally_spawn, va);
    fe.compute(terrain | ally_spawn, enemy_spawn, ve);
    auto da = [&](vec2 p) { return va.is_set(p) ? static_cast<int>(fa.get(p)) : kFar; };
    auto de = [&](vec2 p) { return ve.is_set(p) ? static_cast<int>(fe.get(p)) : kFar; };

    // Distance between spawns: the enemy spawn is a wall for our field, so use its neighbours.
    m.spawn_gap = kFar;
    for (auto t : enemy_spawn.positions()) {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const vec2 n{t.x + dx, t.y + dy};
                if (in_bounds(n) && !enemy_spawn.is_set(n)) m.spawn_gap = std::min(m.spawn_gap, da(n) + 1);
            }
    }

    m.raw = cambc::map::camp_groups();
    for (const auto& g : m.raw) {
        GroupInfo gi;
        gi.gems = static_cast<int>(g.gem_value());
        gi.gold = static_cast<int>(g.gold_value());
        gi.respawn = g.max_respawn_timer();
        long sx = 0, sy = 0;
        for (const auto& c : g.camps()) {
            const auto p = c.spawn_position();
            const auto k = c.kind();
            const auto spec = camp_spec(k);
            gi.tiles.push_back(p);
            m.camp_tiles.set(p);
            gi.kinds.push_back(k);
            gi.hp += spec.hp;
            gi.dps += spec.str;
            gi.range = std::max(gi.range, spec.range);
            if (spec.str > 0) ++gi.fighters;
            if (k == cambc::camp_type::golem || k == cambc::camp_type::monolith) gi.boss = true;
            if (k == cambc::camp_type::monolith) gi.mono = true;
            sx += p.x, sy += p.y;
            gi.dA = std::min(gi.dA, da(p));
            gi.dE = std::min(gi.dE, de(p));
        }
        if (!gi.tiles.empty())
            gi.center = {static_cast<int>(sx / static_cast<long>(gi.tiles.size())),
                         static_cast<int>(sy / static_cast<long>(gi.tiles.size()))};
        m.groups.push_back(std::move(gi));
    }

    for (auto a : cambc::map::altars().positions()) {
        int best_a = kFar, best_e = kFar;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const vec2 n{a.x + dx, a.y + dy};
                if (!in_bounds(n)) continue;
                best_a = std::min(best_a, da(n));
                best_e = std::min(best_e, de(n));
            }
        m.altars.push_back(a);
        m.altar_dA.push_back(best_a);
        m.altar_dE.push_back(best_e);
    }
    return m;
}

}  // namespace

const MapInfo& map_info() {
    static const MapInfo info = analyse();
    return info;
}
