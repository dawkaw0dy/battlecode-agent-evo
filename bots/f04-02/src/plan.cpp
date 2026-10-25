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
}  // namespace

// Centre, health, strength, range and boss flag from the camp list.
void finish_group(GroupInfo& d) {
    long sx = 0, sy = 0;
    for (size_t c = 0; c < d.tiles.size(); ++c) {
        sx += d.tiles[c].x;
        sy += d.tiles[c].y;
        const auto s = camp_spec(d.kinds[c]);
        d.hp += s.hp;
        d.str += s.str;
        d.range = std::max(d.range, s.range);
        if (d.kinds[c] == cambc::camp_type::monolith || d.kinds[c] == cambc::camp_type::golem) d.boss = true;
    }
    if (!d.tiles.empty()) {
        const long n = static_cast<long>(d.tiles.size());
        d.center = {static_cast<int>(sx / n), static_cast<int>(sy / n)};
    }
}

void altar_field(cambc::bitboard& spots, cambc::distance_field& f, cambc::bitboard& seen) {
    spots = empty_board();
    for (auto a : cambc::map::altars().positions()) {
        auto one = empty_board();
        one.set(a);
        spots |= grow(one, cambc::constants::submit_gems_range);
    }
    spots &= ~walk_walls();
    f.compute(walk_walls(), spots, seen);
}

namespace {
struct Reward { int gems, gold, respawn; };
Reward reward_of(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return {4, 40, 40};
    case cambc::camp_type::elk: return {15, 90, 60};
    case cambc::camp_type::gem_cluster: return {7, 0, 50};
    case cambc::camp_type::gold_cluster: return {0, 20, 50};
    case cambc::camp_type::monolith: return {200, 300, 150};
    case cambc::camp_type::golem: return {36, 160, 80};
    }
    return {0, 0, 0};
}
Reward derived(const std::vector<cambc::camp_type>& kinds) {
    Reward r{0, 0, 0};
    for (auto k : kinds) {
        const auto v = reward_of(k);
        r.gems += v.gems;
        r.gold += v.gold;
        r.respawn = std::max(r.respawn, v.respawn);
    }
    return r;
}
}  // namespace

bool write_table(const std::vector<GroupInfo>& groups, int home_altar, std::byte* mem, size_t off, size_t cap) {
    if (groups.size() > 250) return false;
    std::vector<std::uint8_t> exp(groups.size(), 0);
    size_t need = off + dm::kTableHead;
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        if (g.tiles.size() > 127) return false;
        const auto r = derived(g.kinds);
        exp[i] = r.gems != g.gems || r.gold != g.gold || r.respawn != g.respawn || g.gems > 65535 || g.gold > 65535 || g.respawn > 255;
        need += dm::kGroupBytes + (exp[i] ? dm::kExplicitBytes : 0) + dm::kCampBytes * g.tiles.size();
    }
    if (need > cap) return false;
    auto put = [&](size_t& at, int v, int bytes) {
        for (int b = 0; b < bytes; ++b) mem[at++] = static_cast<std::byte>((v >> (8 * b)) & 0xff);
    };
    size_t at = off;
    put(at, std::clamp(home_altar, 0, 65535), 2);
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        put(at, static_cast<int>(g.tiles.size()) | (exp[i] ? 128 : 0), 1);
        if (exp[i]) {
            put(at, std::clamp(g.gems, 0, 65535), 2);
            put(at, std::clamp(g.gold, 0, 65535), 2);
            put(at, std::clamp(g.respawn, 0, 255), 1);
        }
        put(at, std::clamp(g.d_ally, 0, 65535), 2);
        put(at, std::clamp(g.d_enemy, 0, 65535), 2);
        put(at, std::clamp(g.bank_d, 0, 65535), 2);
        for (size_t c = 0; c < g.tiles.size(); ++c) {
            put(at, g.tiles[c].x, 1);
            put(at, g.tiles[c].y, 1);
            put(at, static_cast<int>(g.kinds[c]), 1);
        }
    }
    return true;
}

bool read_table(const std::byte* mem, size_t off, size_t cap, size_t n, std::vector<GroupInfo>& out, int& home_altar) {
    out.clear();
    out.reserve(n);
    size_t at = off;
    auto get = [&](int bytes) {
        int v = 0;
        for (int b = 0; b < bytes; ++b) v |= static_cast<int>(mem[at++]) << (8 * b);
        return v;
    };
    if (at + dm::kTableHead > cap) return false;
    home_altar = get(2);
    for (size_t i = 0; i < n; ++i) {
        if (at + 1 > cap) return false;
        GroupInfo d;
        const int head = get(1);
        const int camps = head & 127;
        const bool explicit_values = head & 128;
        if (at + dm::kGroupBytes - 1 + (explicit_values ? dm::kExplicitBytes : 0) + static_cast<size_t>(dm::kCampBytes * camps) > cap) return false;
        if (explicit_values) {
            d.gems = get(2);
            d.gold = get(2);
            d.respawn = get(1);
        }
        d.d_ally = get(2);
        d.d_enemy = get(2);
        d.bank_d = get(2);
        d.tiles.reserve(static_cast<size_t>(camps));
        d.kinds.reserve(static_cast<size_t>(camps));
        for (int c = 0; c < camps; ++c) {
            const int x = get(1), y = get(1), k = get(1);
            if (k < 0 || k > static_cast<int>(cambc::camp_type::golem)) return false;
            d.tiles.push_back(vec2{x, y});
            d.kinds.push_back(static_cast<cambc::camp_type>(k));
        }
        if (!explicit_values) {
            const auto r = derived(d.kinds);
            d.gems = r.gems;
            d.gold = r.gold;
            d.respawn = r.respawn;
        }
        finish_group(d);
        out.push_back(std::move(d));
    }
    return true;
}

std::vector<GroupInfo> analyse_map(int* home_altar) {
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
        finish_group(d);
        d.gems = static_cast<int>(g.gem_value());
        d.gold = static_cast<int>(g.gold_value());
        d.respawn = g.max_respawn_timer();
        d.d_ally = nearest(fa, sa, d.tiles);
        d.d_enemy = nearest(fe, se, d.tiles);
        out.push_back(std::move(d));
    }
    if (home_altar) {
        cambc::bitboard spots, seen;
        cambc::distance_field fa2;
        altar_field(spots, fa2, seen);
        for (auto& d : out) d.bank_d = nearest(fa2, seen, d.tiles);
        *home_altar = nearest(fa2, seen, cambc::map::spawn_region(team::ally).positions());
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

std::vector<double> region_weights(int size, bool pairs) {
    std::vector<double> w(static_cast<size_t>(std::max(1, region_count(size, pairs))), 0.0);
    for (int s = 0; s < size; ++s) w[static_cast<size_t>(region_of(s, size, pairs))] += 1.0;
    return w;
}

std::vector<std::uint8_t> assign_homes(const std::vector<GroupInfo>& groups, int slots, const std::vector<double>& weight) {
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
    double wsum = 0;
    for (int k = 0; k < slots; ++k) wsum += static_cast<int>(weight.size()) == slots ? weight[static_cast<size_t>(k)] : 1.0;
    auto fair = [&](int k) { return total * (static_cast<int>(weight.size()) == slots ? weight[static_cast<size_t>(k)] : 1.0) / wsum; };
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
                    + 10.0 * std::max(0.0, load[k] / fair(k) - 0.8);
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
