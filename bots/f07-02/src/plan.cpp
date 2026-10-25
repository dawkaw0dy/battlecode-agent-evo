#include "plan.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {
constexpr int kContestGap = 4;          // walking-distance difference that still counts as contested
constexpr double kContestShare = 0.6;   // share of gem income in contested groups that makes a contest map
constexpr double kBossShare = 0.4;      // share of gem income held by bosses that makes a boss map
constexpr bool kBSpells = false;        // play large maps with parent B's spells
constexpr int kRunnerAltars = 2;        // most altars on a map where the last pick runs locks (the enemy
                                        // walks past a locked altar to an open one: with many, a lock costs it little)
constexpr int kBigMap = 3000;           // tiles from which a map is played with parent B's spells
}  // namespace

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

// Contested groups lie about as far from both spawns; a map whose gem income is mostly contested is won
// in the fights over it, not in the farming race.
MapPlan make_plan(const std::vector<GroupInfo>& groups, int slots) {
    MapPlan p;
    p.group.assign(groups.size(), 0);
    double total = 0, contested = 0, bosses = 0;
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        if (g.d_ally >= kFar && g.d_enemy >= kFar) continue;
        const double rate = g.gems / static_cast<double>(std::max(1, g.respawn));
        total += rate;
        if (g.boss) bosses += rate;
        if (g.d_ally < kFar && g.d_enemy < kFar && std::abs(g.d_ally - g.d_enemy) <= kContestGap) {
            p.group[i] |= MapPlan::g_contested;
            contested += rate;
        }
    }
    if (slots >= 2 && total > 0 && contested >= kContestShare * total) p.flags |= MapPlan::contest;
    if (slots >= 2 && total > 0 && bosses >= kBossShare * total) p.flags |= MapPlan::boss;
    if (kBSpells && map_w() * map_h() >= kBigMap) p.flags |= MapPlan::b_spells;
    if (static_cast<int>(cambc::map::altars().count()) <= kRunnerAltars) p.flags |= MapPlan::runner;
    return p;
}
