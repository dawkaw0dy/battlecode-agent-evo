// The opening subsystem (see plan.h).
//
// Rules it is built on: the draft sees the whole map with 10x fuel on its first turn and writes 1024
// bytes the champions read at init; picks are one by one and revealed; each champion's first turn has 2x
// fuel and a turn that runs out resumes next round on stale data (a lost turn). The parent's champions
// each read camp_groups() and ran analyse_map() at init (~150k fuel on a big camp layout): on 26 of the
// 40 maps champions lost round 1 (on most all five), on some rounds 2-4 as well. Here the draft does
// that analysis once and packs it into draft memory (header, route, 13 bytes per group, 3 per camp; home
// slots only when it does not fit), champions decode it, and a first turn that ran over ends at once
// instead of acting stale.
// Picks: Vesper Vale, Vendetta, Marcella, Tavi, then Ryth (area stun, 1200 health) instead of Varg on
// five-pick maps. Home regions per slot are the parent's balanced k-means. A team opening route (the
// whole team takes the same first groups, kRouteSteps) and home regions by exposure (kExposureHomes)
// are wired in but off: both measured worse.
#include "plan.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace {
constexpr int kRouteSteps = 0;        // groups in the opening route
constexpr int kOpenEnd = 150;         // no route step starts after this round
constexpr float kTeamDps = 65.0f;     // per champion, early (basic hits and spells)
constexpr float kCapture = 6.0f;      // capture phases and the step onto the region
constexpr int kDueSlack = 25;         // a step not done this long after its planned end is skipped
constexpr float kRouteBoost = 1000.0f;  // the route's group beats every other
constexpr int kEnemySide = 4;         // groups this much nearer the enemy spawn stay out of the route
constexpr bool kExposureHomes = false;  // home regions by exposure and sturdiness (else in seeding order)

// Draft memory layout: a header, the route, then either every group in full (with its camps) or, when
// that does not fit, the home slots alone (champions then analyse the map themselves).
constexpr int kMagic = 0;      // 'G'
constexpr int kMode = 1;       // 1: full groups, 2: home slots only
constexpr int kSize = 2;       // team size the plan was made for
constexpr int kSteps = 3;      // route length (<= 8)
constexpr int kGroups = 4;     // u16 number of groups
constexpr int kCamps = 6;      // u16 number of camps
constexpr int kRoute = 8;      // route group indices
constexpr int kDue = 16;       // route due rounds / 4
constexpr int kBody = 24;
constexpr int kGroupBytes = 13;  // handle u16, camps u8, gems u16, gold u16, respawn u8, d_ally u16, d_enemy u16, home u8
constexpr int kCampBytes = 3;    // x, y, kind
constexpr size_t kMem = 1024;

void put16(std::byte* m, int at, int v) {
    m[at] = static_cast<std::byte>(v & 0xff);
    m[at + 1] = static_cast<std::byte>((v >> 8) & 0xff);
}
int get16(const std::byte* m, int at) { return static_cast<int>(m[at]) | (static_cast<int>(m[at + 1]) << 8); }

// The per-group fields analyse_map() derives from the camps.
void finish_group(GroupInfo& d) {
    long sx = 0, sy = 0;
    for (size_t c = 0; c < d.tiles.size(); ++c) {
        const vec2 p = d.tiles[c];
        sx += p.x;
        sy += p.y;
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

// Fields and visit masks read in place: a host get() per tile costs ~35 fuel.
int nearest(const cambc::distance_field& f, const cambc::bitboard& seen, const std::vector<vec2>& tiles) {
    int best = kFar;
    for (auto t : tiles)
        if (inside(t) && raw_bit(seen.ptr(), t)) best = std::min<int>(best, raw_dist(f.ptr(), t));
    return best;
}
float group_value(const GroupInfo& g) { return g.gems + 0.35f * g.gold; }

// How much early fighting a pick takes (base health, stun immunity): who gets the most exposed home region.
int sturdiness(cambc::champion_draft c) {
    using cd = cambc::champion_draft;
    switch (c) {
    case cd::marcella: return 6;
    case cd::ryth: return 5;
    case cd::vendetta: return 4;
    case cd::varg: return 4;
    case cd::vesper_vale: return 3;
    case cd::tavi: return 2;
    default: return 1;
    }
}
}  // namespace

const cambc::bitboard& walk_walls() {
    static const cambc::bitboard w = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    return w;
}

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
        d.handle = static_cast<std::uint32_t>(static_cast<cambc::handle_camp_group_t>(g));
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
    return out;
}

std::vector<cambc::champion_draft> pick_order(int size) {
    using cd = cambc::champion_draft;
    // Gold makers first: Vesper Vale (+33% gold), Marcella (gold per gem tributed), Tavi (shops anywhere).
    std::vector<cd> all{cd::vesper_vale, cd::vendetta, cd::marcella, cd::tavi, cd::ryth};
    all.resize(static_cast<size_t>(std::clamp(size, 1, 5)));
    return all;
}

int slot_of(cambc::champion_type t, int size) {
    const auto order = pick_order(size);
    // champion_type splits cassi_and_issac into two entries.
    const int v = static_cast<int>(t);
    const int as_draft = v <= 2 ? v : (v <= 4 ? 3 : v - 1);
    for (size_t i = 0; i < order.size(); ++i)
        if (static_cast<int>(order[i]) == as_draft) return static_cast<int>(i);
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
    // Regions by exposure: the one nearest the enemy (value-weighted d_ally - d_enemy) to the sturdiest pick.
    std::vector<int> slot_of_region(static_cast<size_t>(slots));
    for (int k = 0; k < slots; ++k) slot_of_region[static_cast<size_t>(k)] = k;
    if (kExposureHomes) {
        std::vector<double> fwd(static_cast<size_t>(slots), 0), w(static_cast<size_t>(slots), 0);
        for (int i : el) {
            fwd[static_cast<size_t>(owner[i])] += value(i) * (groups[i].d_ally - groups[i].d_enemy);
            w[static_cast<size_t>(owner[i])] += value(i);
        }
        std::vector<int> regions(static_cast<size_t>(slots)), picks(static_cast<size_t>(slots));
        for (int k = 0; k < slots; ++k) {
            regions[static_cast<size_t>(k)] = k;
            picks[static_cast<size_t>(k)] = k;
            if (w[static_cast<size_t>(k)] > 0) fwd[static_cast<size_t>(k)] /= w[static_cast<size_t>(k)];
        }
        const auto order = pick_order(slots);
        std::stable_sort(regions.begin(), regions.end(), [&](int a, int b) { return fwd[static_cast<size_t>(a)] > fwd[static_cast<size_t>(b)]; });
        std::stable_sort(picks.begin(), picks.end(), [&](int a, int b) {
            return sturdiness(order[static_cast<size_t>(a)]) > sturdiness(order[static_cast<size_t>(b)]);
        });
        for (int k = 0; k < slots; ++k) slot_of_region[static_cast<size_t>(regions[static_cast<size_t>(k)])] = picks[static_cast<size_t>(k)];
    }
    for (int i : el) home[i] = static_cast<std::uint8_t>(slot_of_region[static_cast<size_t>(owner[i])]);
    return home;
}

Opening plan_opening(const std::vector<GroupInfo>& groups, int size) {
    Opening o;
    if (kRouteSteps <= 0 || size <= 1 || groups.empty()) return o;
    const float dps = kTeamDps * static_cast<float>(size);
    cambc::distance_field f;
    cambc::bitboard seen;
    f.compute(walk_walls(), cambc::map::spawn_region(team::ally), seen);
    float t = 0;
    for (int s = 0; s < kRouteSteps && t < kOpenEnd; ++s) {
        int best = -1;
        float best_sc = 0, best_t = 0;
        for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
            const auto& g = groups[i];
            if (g.boss || g.d_ally >= kFar || g.d_enemy + kEnemySide < g.d_ally) continue;
            if (std::find(o.route.begin(), o.route.end(), i) != o.route.end()) continue;
            const float v = group_value(g);
            if (v <= 0) continue;
            const int d = nearest(f, seen, g.tiles);
            if (d >= kFar) continue;
            const float turns = static_cast<float>(d) + g.hp / dps + kCapture;
            const float sc = v / turns;
            if (sc > best_sc) { best_sc = sc; best = i; best_t = turns; }
        }
        if (best < 0) break;
        t += best_t;
        o.route.push_back(best);
        o.due.push_back(static_cast<int>(t) + kDueSlack);
        cambc::bitboard roots;
        for (auto p : groups[best].tiles) roots.set(p);
        f.compute(walk_walls(), roots, seen);
    }
    o.end = kOpenEnd;
    return o;
}

void write_plan(const std::vector<GroupInfo>& groups, int size) {
    const auto home = assign_homes(groups, size);
    const auto open = plan_opening(groups, size);
    std::array<std::byte, kMem> mem{};
    std::byte* m = mem.data();
    const int ng = static_cast<int>(groups.size());
    int nc = 0;
    bool full = ng <= 250;
    for (const auto& g : groups) {
        nc += static_cast<int>(g.tiles.size());
        if (g.handle > 0xffff || g.tiles.size() > 255) full = false;
        for (auto t : g.tiles) if (t.x < 0 || t.y < 0 || t.x > 255 || t.y > 255) full = false;
    }
    if (static_cast<size_t>(kBody + kGroupBytes * ng + kCampBytes * nc) > kMem) full = false;
    if (!full && static_cast<size_t>(kBody + ng) > kMem) return;  // no plan: champions make their own
    m[kMagic] = std::byte{'G'};
    m[kMode] = static_cast<std::byte>(full ? 1 : 2);
    m[kSize] = static_cast<std::byte>(size);
    put16(m, kGroups, ng);
    put16(m, kCamps, nc);
    int steps = 0;
    for (size_t s = 0; s < open.route.size() && steps < 8 && open.route[s] < 256; ++s, ++steps) {
        m[kRoute + steps] = static_cast<std::byte>(open.route[s]);
        m[kDue + steps] = static_cast<std::byte>(std::min(255, open.due[s] / 4));
    }
    m[kSteps] = static_cast<std::byte>(steps);
    int at = kBody;
    if (!full) {
        for (int i = 0; i < ng; ++i) m[at + i] = static_cast<std::byte>(home[i]);
    } else {
        for (int i = 0; i < ng; ++i) {
            const auto& g = groups[i];
            put16(m, at, static_cast<int>(g.handle));
            m[at + 2] = static_cast<std::byte>(g.tiles.size());
            put16(m, at + 3, std::clamp(g.gems, 0, 0xffff));
            put16(m, at + 5, std::clamp(g.gold, 0, 0xffff));
            m[at + 7] = static_cast<std::byte>(std::clamp(g.respawn, 0, 255));
            put16(m, at + 8, std::clamp(g.d_ally, 0, 0xffff));
            put16(m, at + 10, std::clamp(g.d_enemy, 0, 0xffff));
            m[at + 12] = static_cast<std::byte>(home[i]);
            at += kGroupBytes;
        }
        for (const auto& g : groups)
            for (size_t c = 0; c < g.tiles.size(); ++c) {
                m[at] = static_cast<std::byte>(g.tiles[c].x);
                m[at + 1] = static_cast<std::byte>(g.tiles[c].y);
                m[at + 2] = static_cast<std::byte>(static_cast<int>(g.kinds[c]));
                at += kCampBytes;
            }
    }
    cambc::draft_memory = std::span<const std::byte>{mem.data(), mem.size()};
}

bool read_plan(std::vector<GroupInfo>& groups, int& size, std::vector<std::uint8_t>& home, Opening& open) {
    const auto* m = cambc::draft_memory.data();
    const int mode = static_cast<int>(m[kMode]);
    if (cambc::draft_memory.size() < kMem || static_cast<char>(m[kMagic]) != 'G' || (mode != 1 && mode != 2)) {
        groups = analyse_map();
        home = assign_homes(groups, std::max(1, size));
        open = plan_opening(groups, size);
        return false;
    }
    const int ng = get16(m, kGroups);
    int at = kBody;
    if (mode == 2) {
        groups = analyse_map();
        if (static_cast<int>(groups.size()) != ng) {
            home = assign_homes(groups, std::max(1, size));
            open = plan_opening(groups, size);
            return false;
        }
        home.resize(static_cast<size_t>(ng));
        for (int i = 0; i < ng; ++i) home[static_cast<size_t>(i)] = static_cast<std::uint8_t>(m[at + i]);
    } else {
        init_dims();
        groups.assign(static_cast<size_t>(ng), GroupInfo{});
        home.resize(static_cast<size_t>(ng));
        int camp_at = kBody + kGroupBytes * ng;
        for (int i = 0; i < ng; ++i) {
            auto& g = groups[static_cast<size_t>(i)];
            g.handle = static_cast<std::uint32_t>(get16(m, at));
            const int n = static_cast<int>(m[at + 2]);
            g.gems = get16(m, at + 3);
            g.gold = get16(m, at + 5);
            g.respawn = static_cast<int>(m[at + 7]);
            g.d_ally = get16(m, at + 8);
            g.d_enemy = get16(m, at + 10);
            home[static_cast<size_t>(i)] = static_cast<std::uint8_t>(m[at + 12]);
            at += kGroupBytes;
            g.tiles.resize(static_cast<size_t>(n));
            g.kinds.resize(static_cast<size_t>(n));
            for (int c = 0; c < n; ++c, camp_at += kCampBytes) {
                g.tiles[static_cast<size_t>(c)] = {static_cast<int>(m[camp_at]), static_cast<int>(m[camp_at + 1])};
                g.kinds[static_cast<size_t>(c)] = static_cast<cambc::camp_type>(static_cast<int>(m[camp_at + 2]));
            }
            finish_group(g);
        }
    }
    size = static_cast<int>(m[kSize]);
    open = Opening{};
    const int steps = std::min(8, static_cast<int>(m[kSteps]));
    for (int s = 0; s < steps; ++s) {
        const int g = static_cast<int>(m[kRoute + s]);
        if (g >= ng) break;
        open.route.push_back(g);
        open.due.push_back(4 * static_cast<int>(m[kDue + s]));
    }
    open.end = kOpenEnd;
    return true;
}

float home_factor(const std::vector<std::uint8_t>& home, int i, int slot) {
    if (home.size() <= static_cast<size_t>(i)) return 1.0f;
    if (home[i] == 255) return 0.8f;
    return home[i] == slot ? 1.0f : 0.6f;
}

float opening_boost(int i, int current) { return i == current ? kRouteBoost : 1.0f; }
