#include "plan.h"
#include <algorithm>
#include <cmath>

const cambc::bitboard& walk_walls() {
    static const cambc::bitboard w = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    return w;
}

namespace {
// Fields and visit masks read in place: a host get() per tile costs ~35 fuel.
int nearest(const cambc::distance_field& f, const cambc::bitboard& seen, const std::vector<vec2>& tiles) {
    int best = kFar;
    for (auto t : tiles)
        if (inside(t) && raw_bit(seen.ptr(), t)) best = std::min<int>(best, raw_dist(f.ptr(), t));
    return best;
}
}  // namespace

std::vector<GroupInfo> analyse_map() { return analyse_map(cambc::map::camp_groups()); }

std::vector<GroupInfo> analyse_map(const std::vector<cambc::map::camp_group>& groups) {
    init_dims();
    std::vector<GroupInfo> out;
    out.reserve(groups.size());
    const auto walls_e = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::ally);
    cambc::distance_field fa, fe;
    cambc::bitboard sa, se;
    fa.compute(walk_walls(), cambc::map::spawn_region(team::ally), sa);
    fe.compute(walls_e, cambc::map::spawn_region(team::enemy), se);
    for (const auto& g : groups) {
        GroupInfo d;
        long sx = 0, sy = 0;
        for (const auto& c : g.camps()) {
            const vec2 p = c.spawn_position();
            d.tiles.push_back(p);
            d.kinds.push_back(c.kind());
            sx += p.x;
            sy += p.y;
            const auto s = camp_spec(c.kind());
            d.hp += s.hp;
            d.str += s.str;
            d.range = std::max(d.range, s.range);
            if (c.kind() == cambc::camp_type::monolith || c.kind() == cambc::camp_type::golem) d.boss = true;
        }
        if (!d.tiles.empty()) {
            const long n = static_cast<long>(d.tiles.size());
            d.center = {static_cast<int>(sx / n), static_cast<int>(sy / n)};
        }
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

namespace {
// Base health, damage per turn on camps and basic range per pick (after the patches), as measure()
// estimates it in game: strength (x9/8 with an attack reset, x1.25 Vendetta) plus spell damage / 8.
struct Base { float hp, dps; int range; };
Base base_of(cambc::champion_draft c) {
    using cd = cambc::champion_draft;
    switch (c) {
    case cd::vesper_vale: return {1000, 62, 3};
    case cd::vendetta: return {1000, 98, 1};
    case cd::marcella: return {1200, 66, 1};
    case cd::tavi: return {900, 70, 3};
    case cd::varg: return {1250, 50, 1};  // shield 250; splash not counted
    default: return {900, 55, 1};
    }
}

// Turns for a team to clear the group from full health without a trip home (-1: it cannot). Camps die in
// threat-per-health order; a camp hurts while it reaches the one we hit.
float clear_turns(const GroupInfo& g, float dps, float pool, float full, int n, int range) {
    const size_t m = std::min<size_t>(g.tiles.size(), 64);
    std::array<std::pair<float, std::uint8_t>, 64> order;
    std::array<std::uint8_t, 64> alive{};
    for (size_t c = 0; c < m; ++c) {
        const auto s = camp_spec(g.kinds[c]);
        order[c] = {s.str / s.hp, static_cast<std::uint8_t>(c)};
        alive[c] = 1;
    }
    std::sort(order.begin(), order.begin() + m, [](auto a, auto b) { return a.first > b.first; });
    float turns = 0, used = 0;
    int exit = 1;
    for (size_t c = 0; c < m; ++c) {
        const auto s = camp_spec(g.kinds[c]);
        if (s.str > 0) exit = std::max(exit, s.range + 1 - std::min(range, s.range));
    }
    for (size_t j = 0; j < m; ++j) {
        const size_t c = order[j].second;
        float rate = 0;
        for (size_t o = 0; o < m; ++o) {
            if (!alive[o]) continue;
            const auto so = camp_spec(g.kinds[o]);
            if (so.str > 0 && (o == c || cheb(g.tiles[o], g.tiles[c]) + range <= so.range)) rate += so.str;
        }
        const float t = camp_spec(g.kinds[c]).hp / dps;
        turns += t;
        used += t * rate;
        if (rate > 0) {
            const float reserve = std::max(0.2f * full, rate * (exit + 2) + 40);
            const float handoff = std::max(0.2f * full, 2 * rate + 40);
            if (used > pool - (n - 1) * handoff - reserve) return -1;
        }
        alive[c] = 0;
    }
    return turns;
}
}  // namespace

void plan_opening(const std::vector<GroupInfo>& groups, const std::vector<cambc::champion_draft>& picks,
                  std::array<std::uint8_t, 5>& first, std::array<std::uint8_t, 5>& team) {
    first.fill(255);
    team.fill(1);
    const int slots = static_cast<int>(std::min<size_t>(picks.size(), 5));
    if (slots <= 0 || groups.empty() || groups.size() > 250) return;
    // Walk from each group to the nearest altar.
    std::vector<int> bank(groups.size(), kFar);
    int home_altar = kFar;
    {
        cambc::bitboard spots;
        const int ar = cambc::constants::submit_gems_range;
        for (auto a : cambc::map::altars().positions())
            for (int dy = -ar; dy <= ar; ++dy)
                for (int dx = -ar; dx <= ar; ++dx) {
                    const vec2 t{a.x + dx, a.y + dy};
                    if (inside(t)) spots.set(t);
                }
        spots &= ~walk_walls();
        cambc::distance_field f;
        cambc::bitboard seen;
        f.compute(walk_walls(), spots, seen);
        for (size_t i = 0; i < groups.size(); ++i)
            for (auto t : groups[i].tiles)
                if (seen.is_set(t)) bank[i] = std::min(bank[i], static_cast<int>(f.get(t)));
        for (auto t : cambc::map::spawn_region(team::ally).positions())
            if (seen.is_set(t)) home_altar = std::min(home_altar, static_cast<int>(f.get(t)));
    }
    std::vector<char> taken(groups.size(), 0);
    std::vector<int> free_slots;
    for (int s = 0; s < slots; ++s) free_slots.push_back(s);
    float top = 0;  // the first (best) plan's rate
    while (!free_slots.empty()) {
        // Free slots, strongest first: a team of n is the n strongest still free.
        std::stable_sort(free_slots.begin(), free_slots.end(),
                         [&](int a, int b) { return base_of(picks[a]).dps > base_of(picks[b]).dps; });
        float best = 0;
        int best_g = -1, best_n = 0;
        for (size_t i = 0; i < groups.size(); ++i) {
            const auto& g = groups[i];
            // Contested groups need a won fight first: the champions' own choice handles them.
            if (taken[i] || g.d_ally >= kFar || g.d_ally - g.d_enemy > -3) continue;
            const float value = g.gems + 0.35f * g.gold;
            if (value <= 0) continue;
            float dps = 0, pool = 0;
            int range = 3;
            for (int n = 1; n <= std::min<int>(kOpenTeam, static_cast<int>(free_slots.size())); ++n) {
                const Base b = base_of(picks[free_slots[static_cast<size_t>(n - 1)]]);
                dps += b.dps;
                pool += b.hp;
                range = std::min(range, b.range);
                const float t = clear_turns(g, dps, pool, pool / n, n, range);
                if (t < 0) continue;
                const float bk = g.gems / 45.0f * std::min(bank[i], 6 + home_altar);
                const float rate = value / n / (g.d_ally + t + 6.0f + bk / n);
                if (rate > best) { best = rate; best_g = static_cast<int>(i); best_n = n; }
                break;  // the smallest team that can do it
            }
        }
        // Far worse than the first plan (a long walk for leftovers): the champions' own choice does better.
        if (best_g < 0 || best < kOpenCut * top) break;
        top = std::max(top, best);
        taken[static_cast<size_t>(best_g)] = 1;
        for (int k = 0; k < best_n; ++k) {
            const int s = free_slots[static_cast<size_t>(k)];
            first[static_cast<size_t>(s)] = static_cast<std::uint8_t>(best_g);
            team[static_cast<size_t>(s)] = static_cast<std::uint8_t>(best_n);
        }
        free_slots.erase(free_slots.begin(), free_slots.begin() + best_n);
    }
}
