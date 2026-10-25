#include "plan.h"
#include <algorithm>
#include <cmath>

// Switches of the map plan (measured: see NOTES.md).
constexpr int kRunnerAltars = 2;     // most altar tiles for the lock runner and lock detours
constexpr int kLockDetour = 8;       // detour for a lock on those maps
constexpr bool kRoleHomes = false;    // forward home regions to the sturdiest picks
constexpr int kSmallArea = 1300;     // map classes by area
constexpr int kLargeArea = 3000;

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

using cd = cambc::champion_draft;

// champion_type splits cassi_and_issac into two entries.
int as_draft(cambc::champion_type t) {
    const int v = static_cast<int>(t);
    return v <= 2 ? v : (v <= 4 ? 3 : v - 1);
}

// How well a pick holds a forward region (health, stun immunity, melee trading): higher goes forward.
int sturdiness(cd c) {
    switch (c) {
    case cd::marcella: return 6;
    case cd::vendetta: return 5;
    case cd::varg: return 4;
    case cd::ryth: return 4;
    case cd::heart_of_ruin: return 4;
    case cd::ren: return 3;
    case cd::tavi: return 2;
    case cd::vesper_vale: return 1;  // the gold maker stays back
    default: return 0;
    }
}

std::vector<cd> default_picks(int size) {
    // Gold makers first: Vesper Vale (+33% gold), Marcella (gold per gem tributed), Tavi (shops anywhere).
    std::vector<cd> all{cd::vesper_vale, cd::vendetta, cd::marcella, cd::tavi, cd::varg};
    all.resize(static_cast<size_t>(std::clamp(size, 1, 5)));
    return all;
}

MapPlan g_plan;
bool g_plan_ready = false;
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

// ---------------------------------------------------------------- the plan

MapPlan make_plan(const std::vector<GroupInfo>& groups, int slots) {
    (void)groups;
    init_dims();
    MapPlan p;
    const int area = map_w() * map_h();
    p.cls = area < kSmallArea ? MapPlan::small : (area >= kLargeArea ? MapPlan::large : MapPlan::normal);
    // Locks only pay where the enemy has few altars to choose from: with more it walks on to an open one.
    const int altars = static_cast<int>(cambc::map::altars().count());
    if (altars <= kRunnerAltars) {
        p.flags |= MapPlan::runner;
        p.lock_detour = kLockDetour;
    } else {
        p.lock_detour = 0;
    }
    p.picks = default_picks(slots);
    return p;
}

void set_plan(const MapPlan& p) {
    g_plan = p;
    g_plan_ready = true;
}

void write_plan(std::span<std::byte> mem, const MapPlan& p) {
    mem[dm::pmagic] = std::byte{'Q'};
    mem[dm::pflags] = static_cast<std::byte>(p.flags);
    mem[dm::pdetour] = static_cast<std::byte>(std::clamp(p.lock_detour, 0, 255));
    mem[dm::pclass] = static_cast<std::byte>(p.cls);
    for (int i = 0; i < 5; ++i)
        mem[static_cast<size_t>(dm::ppicks + i)] = i < static_cast<int>(p.picks.size())
            ? static_cast<std::byte>(static_cast<int>(p.picks[static_cast<size_t>(i)])) : std::byte{255};
}

const MapPlan& map_plan() {
    if (g_plan_ready) return g_plan;
    g_plan_ready = true;
    if (!cambc::is_draft()) {
        const auto* mem = cambc::draft_memory.data();
        if (mem && static_cast<char>(mem[dm::pmagic]) == 'Q') {
            g_plan.flags = static_cast<std::uint8_t>(mem[dm::pflags]);
            g_plan.lock_detour = static_cast<int>(mem[dm::pdetour]);
            g_plan.cls = static_cast<std::uint8_t>(mem[dm::pclass]);
            g_plan.picks.clear();
            for (int i = 0; i < 5; ++i) {
                const int v = static_cast<int>(mem[static_cast<size_t>(dm::ppicks + i)]);
                if (v <= static_cast<int>(cd::heart_of_ruin)) g_plan.picks.push_back(static_cast<cd>(v));
            }
            return g_plan;
        }
    }
    // No plan in draft memory: make the same one from the map.
    const int size = cambc::is_draft() ? static_cast<int>(cambc::draft::team_size())
                                       : static_cast<int>(cambc::champions(team::ally).size());
    g_plan = make_plan({}, std::clamp(size, 1, 5));
    return g_plan;
}

std::vector<cambc::champion_draft> pick_order(int size) {
    const auto& p = map_plan();
    if (!p.picks.empty()) return p.picks;
    return default_picks(size);
}

int slot_of(cambc::champion_type t, int size) {
    const auto order = pick_order(size);
    const int d = as_draft(t);
    for (size_t i = 0; i < order.size(); ++i)
        if (static_cast<int>(order[i]) == d) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------- home regions

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
    // Regions in order of how far forward they lie (towards the enemy's side); the sturdiest picks take the
    // forward ones, the gold maker the safest.
    std::vector<int> slot_of_cluster(static_cast<size_t>(slots));
    for (int k = 0; k < slots; ++k) slot_of_cluster[static_cast<size_t>(k)] = k;
    const auto& picks = map_plan().picks;
    if (kRoleHomes && static_cast<int>(picks.size()) >= slots) {
        std::vector<double> front(static_cast<size_t>(slots), -1e9);
        for (int k = 0; k < slots; ++k) {
            double s = 0, w = 0;
            for (int i : el)
                if (owner[i] == k) {
                    const auto& g = groups[i];
                    const int de = g.d_enemy >= kFar ? g.d_ally + 30 : g.d_enemy;
                    s += value(i) * std::clamp(g.d_ally - de, -40, 10);
                    w += value(i);
                }
            if (w > 0) front[static_cast<size_t>(k)] = s / w;
        }
        std::vector<int> ck(static_cast<size_t>(slots)), sl(static_cast<size_t>(slots));
        for (int k = 0; k < slots; ++k) ck[static_cast<size_t>(k)] = sl[static_cast<size_t>(k)] = k;
        std::stable_sort(ck.begin(), ck.end(), [&](int a, int b) { return front[static_cast<size_t>(a)] > front[static_cast<size_t>(b)]; });
        std::stable_sort(sl.begin(), sl.end(), [&](int a, int b) {
            return sturdiness(picks[static_cast<size_t>(a)]) > sturdiness(picks[static_cast<size_t>(b)]);
        });
        for (int j = 0; j < slots; ++j) slot_of_cluster[static_cast<size_t>(ck[static_cast<size_t>(j)])] = sl[static_cast<size_t>(j)];
    }
    for (int i : el) home[i] = static_cast<std::uint8_t>(slot_of_cluster[static_cast<size_t>(owner[i])]);
    return home;
}
