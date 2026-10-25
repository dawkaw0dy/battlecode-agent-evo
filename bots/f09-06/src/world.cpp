// Map knowledge and the per-turn snapshot.
#include "world.h"
#include <variant>
#include <algorithm>
#include <cmath>

World W;

namespace {

struct Spec { float hp, str; int range; };
Spec camp_spec(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return {1350, 20, 3};
    case cambc::camp_type::elk: return {2400, 60, 4};
    case cambc::camp_type::gem_cluster: return {120, 0, 0};
    case cambc::camp_type::gold_cluster: return {120, 0, 0};
    case cambc::camp_type::monolith: return {20000, 200, 5};
    case cambc::camp_type::golem: return {5000, 120, 4};
    }
    return {1500, 50, 3};
}

vec2 centroid(const cambc::bitboard& b) {
    long sx = 0, sy = 0, n = 0;
    for (auto p : b.positions()) { sx += p.x; sy += p.y; ++n; }
    if (!n) return {0, 0};
    return {static_cast<int>(sx / n), static_cast<int>(sy / n)};
}

}  // namespace

void World::init() {
    init_dims();
    blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(team::enemy);
    our_spawn = cambc::map::spawn_region(team::ally);
    their_spawn = cambc::map::spawn_region(team::enemy);
    our_center = centroid(our_spawn);
    their_center = centroid(their_spawn);
    tile_group.assign(static_cast<std::size_t>(g_w) * g_h, -1);

    from_spawn.compute(blocked, our_spawn);
    sym = cambc::map::symmetry();

    const auto cg = cambc::map::camp_groups();
    groups.reserve(cg.size());
    for (const auto& g : cg) {
        Group G;
        G.id = static_cast<int>(groups.size());
        G.gems = static_cast<int>(g.gem_value());
        G.gold = static_cast<int>(g.gold_value());
        G.respawn_max = g.max_respawn_timer();
        long sx = 0, sy = 0;
        for (const auto& c : g.camps()) {
            const auto s = camp_spec(c.kind());
            Member m{c.spawn_position(), c.kind(), s.hp, s.hp, s.str, s.range};
            G.m.push_back(m);
            G.tiles.push_back(m.pos);
            G.spawn.set(m.pos);
            sx += m.pos.x; sy += m.pos.y;
            if (inside(m.pos)) tile_group[m.pos.y * g_w + m.pos.x] = static_cast<std::int16_t>(G.id);
            if (c.kind() == cambc::camp_type::golem || c.kind() == cambc::camp_type::monolith) G.boss = true;
        }
        if (G.m.empty()) continue;
        handles.push_back(g);
        G.center = {static_cast<int>(sx / static_cast<long>(G.m.size())), static_cast<int>(sy / static_cast<long>(G.m.size()))};
        for (auto t : G.tiles) { G.radius = std::max(G.radius, cheb(t, G.center)); alive_camps.set(t); }
        int a = kFar, b = kFar;
        for (auto t : G.tiles) {
            for (auto d : kDirs) {
                const vec2 q = t + d;
                if (!passable(q)) continue;
                a = std::min(a, from_spawn.at(q));
                b = std::min(b, from_spawn.at(mirror(q)));
            }
        }
        G.ours = a; G.theirs = b;
        G.island = true;
        for (auto t : G.tiles) if (from_spawn.at(t) < kFar) G.island = false;
        groups.push_back(std::move(G));
    }

    const int ar = cambc::constants::submit_gems_range;
    for (auto p : cambc::map::altars().positions()) {
        Altar A;
        A.pos = p;
        for (int dy = -ar; dy <= ar; ++dy)
            for (int dx = -ar; dx <= ar; ++dx) {
                const vec2 q{p.x + dx, p.y + dy};
                if (!passable(q)) continue;
                A.approach.push_back(q);
                A.ours = std::min(A.ours, from_spawn.at(q));
                A.theirs = std::min(A.theirs, from_spawn.at(mirror(q)));
            }
        altars.push_back(std::move(A));
    }
    ready = true;
}

void World::refresh() {
    round = static_cast<int>(cambc::round_number());
    const vec2 me = cambc::self().position();
    cambc::vision::visible_tiles_into(visible);
    cambc::vision::champion_tiles_into(occupied);
    cambc::vision::camp_tiles_into(campb);
    occupied |= campb;
    occupied |= cambc::vision::teleport_tiles();
    if (inside(me)) occupied.set(me, false);

    enemies.clear();
    allies.clear();
    for (int side = 0; side < 2; ++side) {
        const team t = side ? team::enemy : team::ally;
        for (auto p : cambc::vision::champion_tiles(t).positions()) {
            if (p == me) continue;
            Unit u;
            u.pos = p;
            u.enemy = side == 1;
            try {
                auto c = cambc::vision::champion_at(p);
                u.type = c.champion_name();
                u.hp = c.health();
                u.max_hp = c.max_health();
                u.str = c.strength();
                u.focus = c.focus();
                u.range = c.attack_range();
                u.tribute = c.is_submitting_gems();
                u.channel = u.tribute || c.is_recalling();
                u.gems = static_cast<int>(c.gem_count());
                u.stunned = c.buffs().has_stun();
            } catch (const std::exception&) {
                if (u.hp <= 0) continue;
            }
            if (u.hp <= 0) continue;
            (u.enemy ? enemies : allies).push_back(u);
        }
    }

    camps.clear();
    for (auto p : campb.positions()) {
        CampSeen c{};
        c.pos = p;
        c.group = group_at(p);
        try {
            auto k = cambc::vision::camp_at(p);
            c.kind = k.kind();
            c.hp = k.health();
            c.str = k.strength();
            c.range = k.attack_range();
            c.awake = k.is_awake();
        } catch (const std::exception&) {
            continue;
        }
        if (c.hp <= 0) continue;
        camps.push_back(c);
        if (c.group >= 0)
            for (auto& m : groups[c.group].m)
                if (m.pos == p) {
                    if (m.hp <= 0) mark(m, true);
                    m.hp = c.hp; m.str = c.str; m.range = c.range; m.max_hp = std::max(m.max_hp, c.hp);
                }
    }

    // Group states for groups with a spawn tile in vision; beliefs for the rest.
    const auto* vis = visible.ptr();
    const auto* cb = campb.ptr();
    for (auto& G : groups) {
        bool seen = false;
        if (cheb(G.center, me) <= G.radius + 9)
            for (auto t : G.tiles) if (raw_bit(vis, t)) { seen = true; break; }
        if (!seen) {
            if (G.state == GroupState::respawning && round >= G.respawn_at) {
                G.state = GroupState::alive;
                for (auto& m : G.m) { m.hp = m.max_hp; mark(m, true); }
            }
            continue;
        }
        cambc::map::camp_group_state st;
        try {
            st = handles[G.id].state();
        } catch (const std::exception&) {
            continue;
        }
        G.seen = round;
        if (std::holds_alternative<cambc::map::camp_group_respawning>(st)) {
            const int t = std::get<cambc::map::camp_group_respawning>(st).timer;
            if (G.state != GroupState::respawning) for (auto& m : G.m) mark(m, false);
            G.state = GroupState::respawning;
            G.respawn_at = round + std::max(t, 1);
            for (auto& m : G.m) m.hp = m.max_hp;
        } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
            G.state = GroupState::reward;
            for (auto& m : G.m) { if (m.hp > 0) mark(m, false); m.hp = 0; }
        } else {
            if (G.state != GroupState::alive) for (auto& m : G.m) { m.hp = m.max_hp; mark(m, true); }
            G.state = GroupState::alive;
            // A visible spawn tile without a camp on it means that member is dead.
            for (auto& m : G.m)
                if (m.hp > 0 && raw_bit(vis, m.pos) && !raw_bit(cb, m.pos)) { m.hp = 0; mark(m, false); }
        }
    }
}

const Field* World::group_field(int gi, bool compute) {
    auto& G = groups[static_cast<std::size_t>(gi)];
    if (G.field.ready()) return &G.field;
    if (!compute || cambc::fuel_remaining() < 70000) return nullptr;
    cambc::bitboard roots;
    for (auto t : G.tiles)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const vec2 q{t.x + dx, t.y + dy};
                if (passable(q)) roots.set(q);
            }
    if (roots.is_empty()) return nullptr;
    G.field.compute(blocked, roots);
    return &G.field;
}

void World::precompute_some() {
    // Nearest groups first.
    int best = -1;
    for (const auto& G : groups)
        if (!G.island && !G.field.ready() && (best < 0 || G.ours < groups[static_cast<std::size_t>(best)].ours)) best = G.id;
    if (best >= 0) group_field(best);
}

void World::partition(int packs) {
    struct C { float angle, w; int id; };
    std::vector<C> cs;
    float total = 0;
    for (auto& G : groups) {
        G.sector = -1;
        if (G.island || G.boss || G.ours >= kFar || G.ours > G.theirs + 6) continue;
        const float a = std::atan2(static_cast<float>(G.center.y - our_center.y), static_cast<float>(G.center.x - our_center.x));
        const float w = std::min(60.0f, static_cast<float>(G.gems) + 0.15f * static_cast<float>(G.gold)) + 10.0f;
        cs.push_back({a, w, G.id});
        total += w;
    }
    if (cs.empty() || packs <= 1) {
        for (auto& c : cs) groups[static_cast<std::size_t>(c.id)].sector = 0;
        return;
    }
    // Start the sweep at the widest angular gap so no sector wraps around.
    std::sort(cs.begin(), cs.end(), [](const C& a, const C& b) { return a.angle < b.angle || (a.angle == b.angle && a.id < b.id); });
    std::size_t start = 0;
    float gap = -1;
    for (std::size_t i = 0; i < cs.size(); ++i) {
        const float next = i + 1 < cs.size() ? cs[i + 1].angle : cs[0].angle + 6.2831853f;
        if (next - cs[i].angle > gap) { gap = next - cs[i].angle; start = (i + 1) % cs.size(); }
    }
    float acc = 0;
    for (std::size_t k = 0; k < cs.size(); ++k) {
        const auto& c = cs[(start + k) % cs.size()];
        const int sec = std::min(packs - 1, static_cast<int>((acc + c.w * 0.5f) / total * static_cast<float>(packs)));
        groups[static_cast<std::size_t>(c.id)].sector = sec;
        acc += c.w;
    }
}

vec2 World::mirror(vec2 p) const {
    switch (sym) {
    case cambc::symmetry::horizontal: return cambc::map::reflect_horizontal(p);
    case cambc::symmetry::vertical: return cambc::map::reflect_vertical(p);
    case cambc::symmetry::rotational180: return cambc::map::rotate_180(p);
    }
    return p;
}

void World::mark(Member& m, bool alive) {
    if (!inside(m.pos)) return;
    if (raw_bit(alive_camps.ptr(), m.pos) == alive) return;
    alive_camps.set(m.pos, alive);
    ++camps_version;
}
