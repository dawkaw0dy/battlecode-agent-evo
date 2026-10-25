// Map model: static terrain, camp groups and what we last saw of each group.
#include "world.h"
#include <algorithm>

namespace {
std::unique_ptr<World> g_world;
}

World& world() { return *g_world; }

int group_of_tile(vec2 p) {
    const auto& w = *g_world;
    for (int gi = 0; gi < static_cast<int>(w.groups.size()); ++gi) {
        const auto& G = w.groups[gi];
        if (cheb(G.center, p) > G.span) continue;
        for (const auto& m : G.m)
            if (m.pos == p) return gi;
    }
    return -1;
}

void init_world() {
    g_world = std::make_unique<World>();
    auto& w = *g_world;
    w.w = cambc::map::width();
    w.h = cambc::map::height();

    w.terrain = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(cambc::team::enemy);
    w.spawn_ally = cambc::map::spawn_region(cambc::team::ally);
    w.spawn_tiles = w.spawn_ally.positions();
    w.altars = cambc::map::altars().positions();
    w.altar_adj.clear();
    for (auto a : w.altars) w.altar_adj.draw_chebyshev(a, static_cast<float>(cambc::constants::submit_gems_range));
    w.altar_adj &= ~w.terrain;

    long sx = 0, sy = 0;
    for (auto p : w.spawn_tiles) { sx += p.x; sy += p.y; }
    if (!w.spawn_tiles.empty())
        w.home_center = {static_cast<int>(sx / static_cast<long>(w.spawn_tiles.size())),
                         static_cast<int>(sy / static_cast<long>(w.spawn_tiles.size()))};
    const auto enemy_tiles = cambc::map::spawn_region(cambc::team::enemy).positions();
    sx = sy = 0;
    for (auto p : enemy_tiles) { sx += p.x; sy += p.y; }
    if (!enemy_tiles.empty())
        w.enemy_center = {static_cast<int>(sx / static_cast<long>(enemy_tiles.size())),
                          static_cast<int>(sy / static_cast<long>(enemy_tiles.size()))};

    for (const auto& g : cambc::map::camp_groups()) {
        Group G;
        G.handle = static_cast<std::uint32_t>(static_cast<cambc::handle_camp_group_t>(g));
        G.gems = static_cast<int>(g.gem_value());
        G.gold = static_cast<int>(g.gold_value());
        G.respawn = g.max_respawn_timer();
        long cx = 0, cy = 0;
        for (const auto& c : g.camps()) {
            const auto d = camp_design(c.kind());
            Member m{c.kind(), c.spawn_position(), d.hp, d.str, d.range, d.hp};
            G.m.push_back(m);
            G.hp_max += d.hp;
            G.str_total += d.str;
            if (c.kind() == cambc::camp_type::monolith || c.kind() == cambc::camp_type::golem) G.boss = true;
            cx += m.pos.x;
            cy += m.pos.y;
        }
        if (G.m.empty()) continue;
        std::sort(G.m.begin(), G.m.end(), [](const Member& a, const Member& b) {
            return a.str != b.str ? a.str > b.str : a.hp_max < b.hp_max;
        });
        G.center = {static_cast<int>(cx / static_cast<long>(G.m.size())), static_cast<int>(cy / static_cast<long>(G.m.size()))};
        for (const auto& m : G.m) G.span = std::max(G.span, cheb(m.pos, G.center));
        w.groups.push_back(std::move(G));
    }

    cambc::bitboard visited;
    w.home.compute(w.terrain, w.spawn_ally, visited);
    if (!w.altar_adj.is_empty()) w.altar.compute(w.terrain, w.altar_adj, visited);
    w.gfield.resize(w.groups.size());
    for (int i = 0; i < static_cast<int>(w.groups.size()); ++i) w.field_order.push_back(i);
    for (auto& G : w.groups)
        for (const auto& m : G.m) G.d_home = std::min<int>(G.d_home, w.home.get(m.pos));
    std::sort(w.field_order.begin(), w.field_order.end(),
              [&](int a, int b) { return w.groups[a].d_home < w.groups[b].d_home; });
}

const cambc::distance_field* group_field(int gi) {
    auto& w = *g_world;
    if (gi < 0 || gi >= static_cast<int>(w.gfield.size())) return nullptr;
    return w.gfield[gi].get();
}

void fill_group_fields(std::uint64_t floor) {
    auto& w = *g_world;
    if (!w.enemy_done) {
        if (cambc::fuel_remaining() < floor) return;
        cambc::distance_field enemy;
        cambc::bitboard visited;
        const auto walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | w.spawn_ally;
        enemy.compute(walls, cambc::map::spawn_region(cambc::team::enemy), visited);
        for (auto& G : w.groups)
            for (const auto& m : G.m) G.d_enemy = std::min<int>(G.d_enemy, enemy.get(m.pos));
        w.enemy_done = true;
    }
    for (int gi : w.field_order) {
        if (w.gfield[gi]) continue;
        if (cambc::fuel_remaining() < floor) return;
        auto f = std::make_unique<cambc::distance_field>();
        cambc::bitboard roots;
        for (const auto& m : w.groups[gi].m) roots.set(m.pos, true);
        cambc::bitboard visited;
        f->compute(w.terrain, roots, visited);
        w.gfield[gi] = std::move(f);
    }
}

void update_groups(int round, vec2 me) {
    auto& w = *g_world;
    cambc::bitboard vis{cambc::vision::visible_tiles()};
    for (auto& G : w.groups) {
        bool any = false;
        if (cheb(G.center, me) <= 16)
            for (const auto& m : G.m)
                if (vis.is_set(m.pos)) { any = true; break; }
        if (!any) {
            if (G.state == kRespawning && round >= G.ready) G.state = kUnknown;
            else if (G.state == kReward && round - G.reward_seen > 12) {
                // Most likely captured out of sight; assume the respawn started then.
                G.state = kRespawning;
                G.ready = G.reward_seen + G.respawn;
                for (auto& m : G.m) m.hp = m.hp_max;
            }
            continue;
        }
        G.seen = round;
        std::uint32_t s = 0;
        if (cambc::detail::sdk_camp_group_state(G.handle, &s) != 0) continue;
        if (s == 1) {
            if (G.state == kRespawning || G.state == kReward)
                for (auto& m : G.m) m.hp = m.hp_max;
            G.state = kAlive;
            for (auto& m : G.m) {
                if (!vis.is_set(m.pos)) continue;
                if (cambc::vision::has_camp(m.pos)) {
                    try { m.hp = cambc::vision::camp_at(m.pos).health(); } catch (...) {}
                } else {
                    m.hp = 0;
                }
            }
        } else if (s == 2) {
            G.state = kReward;
            G.reward_seen = round;
            for (auto& m : G.m) m.hp = 0;
        } else if (s == 3) {
            std::int32_t timer = 0;
            cambc::detail::sdk_camp_group_cooldown(G.handle, &timer);
            G.state = kRespawning;
            G.ready = round + std::max(timer, 1);
            for (auto& m : G.m) m.hp = m.hp_max;
        }
    }
}
