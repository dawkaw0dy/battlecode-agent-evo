#include "plan.h"
#include <algorithm>
#include <cmath>

const cambc::bitboard& walk_walls() {
    static const cambc::bitboard w = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    return w;
}

namespace {
int nearest(const cambc::distance_field& f, const cambc::bitboard& seen, const std::vector<vec2>& tiles) {
    int best = kFar;
    for (auto t : tiles)
        if (seen.is_set(t)) best = std::min<int>(best, f.get(t));
    return best;
}

// Totals, range, boss flag and centre from the camps.
void derive(GroupInfo& d) {
    long sx = 0, sy = 0;
    for (size_t i = 0; i < d.tiles.size(); ++i) {
        const vec2 p = d.tiles[i];
        const auto k = d.kinds[i];
        sx += p.x;
        sy += p.y;
        const auto s = camp_spec(k);
        d.hp += s.hp;
        d.str += s.str;
        d.range = std::max(d.range, s.range);
        if (k == cambc::camp_type::monolith || k == cambc::camp_type::golem) d.boss = true;
    }
    if (!d.tiles.empty()) {
        const long n = static_cast<long>(d.tiles.size());
        d.center = {static_cast<int>(sx / n), static_cast<int>(sy / n)};
    }
}
}  // namespace

// Per group: camps, gems u16, gold u16, respawn, d_ally u16, d_enemy u16; per camp: x, y, kind.
size_t pack_groups(const std::vector<GroupInfo>& groups, std::byte* out, size_t cap) {
    size_t at = 0;
    auto put = [&](int v) { out[at++] = static_cast<std::byte>(v & 0xff); };
    auto put16 = [&](int v) { put(v); put(v >> 8); };
    for (const auto& g : groups) {
        if (at + 10 + 3 * g.tiles.size() > cap || g.tiles.size() > 255) return 0;
        put(static_cast<int>(g.tiles.size()));
        put16(std::clamp(g.gems, 0, 65535));
        put16(std::clamp(g.gold, 0, 65535));
        put(std::clamp(g.respawn, 0, 255));
        put16(std::clamp(g.d_ally, 0, kFar));
        put16(std::clamp(g.d_enemy, 0, kFar));
        for (size_t i = 0; i < g.tiles.size(); ++i) {
            put(g.tiles[i].x);
            put(g.tiles[i].y);
            put(static_cast<int>(g.kinds[i]));
        }
    }
    return at;
}

bool unpack_groups(const std::byte* in, size_t len, size_t n, std::vector<GroupInfo>& out) {
    // Allocations dominate the cost here: reserve everything once.
    out.clear();
    out.reserve(n);
    size_t at = 0;
    auto get = [&]() { return static_cast<int>(in[at++]); };
    auto get16 = [&]() { const int lo = get(); return lo | get() << 8; };
    for (size_t gi = 0; gi < n; ++gi) {
        if (at + 10 > len) return false;
        GroupInfo d;
        const int camps = get();
        d.gems = get16();
        d.gold = get16();
        d.respawn = get();
        d.d_ally = get16();
        d.d_enemy = get16();
        if (at + 3 * static_cast<size_t>(camps) > len) return false;
        d.tiles.reserve(static_cast<size_t>(camps));
        d.kinds.reserve(static_cast<size_t>(camps));
        for (int c = 0; c < camps; ++c) {
            const int x = get(), y = get(), k = get();
            if (k > static_cast<int>(cambc::camp_type::golem)) return false;
            d.tiles.push_back({x, y});
            d.kinds.push_back(static_cast<cambc::camp_type>(k));
        }
        derive(d);
        out.push_back(std::move(d));
    }
    return true;
}

std::vector<GroupInfo> analyse_map() {
    init_dims();
    std::vector<GroupInfo> out;
    const auto groups = cambc::map::camp_groups();
    const auto walls_e = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::ally);
    cambc::distance_field fa, fe;
    cambc::bitboard sa, se;
    fa.compute(walk_walls(), cambc::map::spawn_region(team::ally), sa);
    fe.compute(walls_e, cambc::map::spawn_region(team::enemy), se);
    for (const auto& g : groups) {
        GroupInfo d;
        for (const auto& c : g.camps()) {
            d.tiles.push_back(c.spawn_position());
            d.kinds.push_back(c.kind());
        }
        derive(d);
        d.gems = static_cast<int>(g.gem_value());
        d.gold = static_cast<int>(g.gold_value());
        d.respawn = g.max_respawn_timer();
        d.d_ally = nearest(fa, sa, d.tiles);
        d.d_enemy = nearest(fe, se, d.tiles);
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<cambc::champion_draft> pick_order(int size) {
    using cd = cambc::champion_draft;
    // Gold makers first: Vesper Vale (+33% gold), Marcella (gold per gem tributed), Tavi (shops anywhere).
    std::vector<cd> all{cd::vesper_vale, cd::vendetta, cd::marcella, cd::tavi, cd::varg};
    all.resize(static_cast<size_t>(std::clamp(size, 1, 5)));
    return all;
}

int slot_of(cambc::champion_type t, int size) {
    const auto order = pick_order(size);
    for (size_t i = 0; i < order.size(); ++i) {
        const int d = static_cast<int>(order[i]);
        // champion_type splits cassi_and_issac into two entries.
        const int v = static_cast<int>(t);
        const int as_draft = v <= 2 ? v : (v <= 4 ? 3 : v - 1);
        if (d == as_draft) return static_cast<int>(i);
    }
    return -1;
}

std::vector<std::uint8_t> assign_homes(const std::vector<GroupInfo>& groups, int slots) {
    std::vector<std::uint8_t> home(groups.size(), 255);
    if (slots <= 1 || groups.empty()) return home;
    std::vector<int> el;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        const auto& g = groups[i];
        if (g.boss || g.d_ally >= kFar) continue;
        if (g.d_ally <= g.d_enemy + 4) el.push_back(i);
    }
    if (static_cast<int>(el.size()) < slots) return home;
    auto value = [&](int i) { return groups[i].gems + 0.4 * groups[i].gold + 3.0 + groups[i].hp / 500.0; };
    auto d2 = [](vec2 a, double x, double y) { return std::hypot(a.x - x, a.y - y); };

    // Farthest-point seeds starting from the group farthest from our spawn.
    std::vector<std::pair<double, double>> centers;
    int first = el[0];
    for (int i : el) if (groups[i].d_ally > groups[first].d_ally) first = i;
    centers.push_back({static_cast<double>(groups[first].center.x), static_cast<double>(groups[first].center.y)});
    while (static_cast<int>(centers.size()) < slots) {
        int best = -1;
        double best_d = -1;
        for (int i : el) {
            double m = 1e18;
            for (auto& c : centers) m = std::min(m, d2(groups[i].center, c.first, c.second));
            if (m > best_d) { best_d = m; best = i; }
        }
        centers.push_back({static_cast<double>(groups[best].center.x), static_cast<double>(groups[best].center.y)});
    }

    // Balanced k-means: nearest centre with a penalty for an overloaded one.
    double total = 0;
    for (int i : el) total += value(i);
    const double fair = total / slots;
    std::vector<int> owner(groups.size(), -1);
    for (int iter = 0; iter < 6; ++iter) {
        std::vector<double> load(static_cast<size_t>(slots), 0);
        std::vector<int> order = el;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return value(a) > value(b); });
        for (int i : order) {
            int best = 0;
            double best_cost = 1e18;
            for (int k = 0; k < slots; ++k) {
                const double cost = d2(groups[i].center, centers[k].first, centers[k].second)
                    + 10.0 * std::max(0.0, load[k] / fair - 0.8);
                if (cost < best_cost) { best_cost = cost; best = k; }
            }
            owner[i] = best;
            load[best] += value(i);
        }
        for (int k = 0; k < slots; ++k) {
            double sx = 0, sy = 0, w = 0;
            for (int i : el)
                if (owner[i] == k) { sx += value(i) * groups[i].center.x; sy += value(i) * groups[i].center.y; w += value(i); }
            if (w > 0) centers[k] = {sx / w, sy / w};
        }
    }
    for (int i : el) home[i] = static_cast<std::uint8_t>(owner[i]);
    return home;
}
