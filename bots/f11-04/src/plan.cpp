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
// What one pick brings to a solo clear: health, damage per turn on camps, basic range.
struct SlotStats { float hp, dps; int range; bool splash; };
SlotStats slot_stats(cambc::champion_draft d) {
    using cd = cambc::champion_draft;
    switch (d) {
    case cd::vesper_vale: return {1000, 62, 3, false};
    case cd::vendetta: return {1000, 95, 1, false};
    case cd::marcella: return {1200, 66, 1, false};
    case cd::tavi: return {900, 70, 3, false};
    case cd::varg: return {1000, 50, 1, true};
    default: return {900, 55, 1, false};
    }
}
// Σ hp · incoming over the kill order (highest threat per health first), for a fighter of basic range r:
// divided by our damage per turn it is the camp damage one champion takes clearing the group alone.
float solo_cost(const GroupInfo& g, int r) {
    const size_t n = g.tiles.size();
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto sa = camp_spec(g.kinds[a]), sb = camp_spec(g.kinds[b]);
        return sa.str / sa.hp > sb.str / sb.hp;
    });
    std::vector<char> alive(n, 1);
    float cost = 0;
    for (size_t c : order) {
        float rate = 0;
        for (size_t m = 0; m < n; ++m) {
            if (!alive[m]) continue;
            const auto s = camp_spec(g.kinds[m]);
            if (s.str > 0 && (m == c || cheb(g.tiles[m], g.tiles[c]) + r <= s.range)) rate += s.str;
        }
        cost += camp_spec(g.kinds[c]).hp * rate;
        alive[c] = 0;
    }
    return cost;
}
}  // namespace

std::vector<std::vector<std::uint8_t>> plan_routes(const std::vector<GroupInfo>& groups, int slots) {
    constexpr float kHorizon = 400.0f;   // plan the opening this many rounds deep
    constexpr float kCapture = 6.0f;     // five capture phases and the step onto the region
    constexpr float kTrip = 8.0f;        // recall and the heal on spawn
    constexpr float kGoldW = 0.35f;
    constexpr std::uint64_t kFieldReserve = 300000;  // fuel kept for the routing below
    constexpr std::uint64_t kLoopReserve = 40000;    // ... and for writing the plan
    constexpr size_t kMaxCand = 48;
    constexpr float kMaxTrips = 0.0f;    // heal trips one solo clear may take
    std::vector<std::vector<std::uint8_t>> routes(static_cast<size_t>(std::max(0, slots)));
    if (slots <= 0) return routes;
    const auto picks = pick_order(slots);
    // Candidates: groups one champion clears alone, on our side of the map.
    std::vector<int> cand;
    for (int i = 0; i < static_cast<int>(groups.size()) && i < 250; ++i) {
        const auto& g = groups[i];
        if (g.boss || g.d_ally >= kFar || g.tiles.empty()) continue;
        if (g.d_ally > g.d_enemy + 2) continue;
        if (g.gems + kGoldW * g.gold <= 0) continue;
        cand.push_back(i);
    }
    std::sort(cand.begin(), cand.end(), [&](int a, int b) { return groups[a].d_ally < groups[b].d_ally; });
    if (cand.size() > kMaxCand) cand.resize(kMaxCand);
    const size_t nc = cand.size();
    if (nc == 0) return routes;
    // Walking distance between candidate groups (tile to tile), one host BFS per row while fuel lasts.
    std::vector<int> dist(nc * nc, -1);
    {
        cambc::distance_field f;
        cambc::bitboard seen, roots;
        for (size_t a = 0; a < nc; ++a) {
            if (cambc::fuel_remaining() < kFieldReserve) break;
            roots.clear();
            for (auto t : groups[cand[a]].tiles) roots.set(t);
            f.compute(walk_walls(), roots, seen);
            const auto* q = static_cast<const cambc::distance_field&>(f).ptr();
            for (size_t b = 0; b < nc; ++b) {
                int best = kFar;
                for (auto t : groups[cand[b]].tiles)
                    if (inside(t)) best = std::min(best, raw_dist(q, t));
                dist[a * nc + b] = best;
            }
        }
    }
    auto d = [&](size_t a, size_t b) {
        if (dist[a * nc + b] >= 0) return dist[a * nc + b];
        if (dist[b * nc + a] >= 0) return dist[b * nc + a];
        return cheb(groups[cand[a]].center, groups[cand[b]].center) * 5 / 4 + 2;
    };
    std::vector<float> cost1(nc), cost3(nc);
    for (size_t a = 0; a < nc; ++a) {
        cost1[a] = solo_cost(groups[cand[a]], 1);
        cost3[a] = solo_cost(groups[cand[a]], 3);
    }
    struct Walker { float t = 0, dmg = 0; int cur = -1; bool done = false; };
    std::vector<Walker> w(static_cast<size_t>(slots));
    std::vector<char> used(nc, 0);
    for (int guard = 0; guard < 400; ++guard) {
        if (cambc::fuel_remaining() < kLoopReserve) break;
        int s = -1;
        for (int k = 0; k < slots; ++k)
            if (!w[k].done && (s < 0 || w[k].t < w[s].t)) s = k;
        if (s < 0) break;
        auto& me = w[static_cast<size_t>(s)];
        if (me.t > kHorizon || routes[s].size() >= static_cast<size_t>(dm::kRouteMax)) { me.done = true; continue; }
        const auto st = slot_stats(picks[static_cast<size_t>(s)]);
        float best_rate = 0, best_total = 0, best_dmg = 0;
        int best = -1;
        for (size_t j = 0; j < nc; ++j) {
            if (used[j]) continue;
            const auto& g = groups[cand[j]];
            const float dps = st.dps * (st.splash && g.tiles.size() > 1 ? 1.4f : 1.0f);
            const float dmg = (st.range > 1 ? cost3[j] : cost1[j]) / dps;
            const float bar = st.hp * 0.75f;  // camp damage one walk out absorbs
            float extra = 0, travel;
            float left = bar - me.dmg;
            if (left < std::min(dmg, bar) * 0.5f) {
                // Heal on the way: recall (or walk when it is close), then walk out again.
                extra = (me.cur < 0 ? 0.0f : std::min<float>(static_cast<float>(groups[cand[me.cur]].d_ally), kTrip)) + 2.0f;
                travel = static_cast<float>(g.d_ally);
                left = bar;
            } else {
                travel = static_cast<float>(me.cur < 0 ? g.d_ally : d(static_cast<size_t>(me.cur), j));
            }
            if (travel >= kFar) continue;
            // Clears longer than one health bar: trips home in the middle (camps do not heal).
            const float trips = dmg <= left ? 0.0f : std::ceil((dmg - left) / bar);
            if (trips > kMaxTrips) continue;  // a raid, not a solo clear
            const float after = trips > 0 ? dmg - left - (trips - 1) * bar : me.dmg + dmg;
            extra += trips * (std::min<float>(static_cast<float>(g.d_ally), kTrip) + 2.0f + static_cast<float>(g.d_ally));
            travel = std::max(0.0f, travel - static_cast<float>(st.range));
            const float clear = g.hp / dps;
            const float total = extra + travel + clear + kCapture;
            float f = 1.0f;
            // The enemy reaches it first: it may be gone (or contested) when we arrive.
            if (me.t + travel > static_cast<float>(g.d_enemy) + 6.0f) f = 0.6f;
            const float rate = (g.gems + kGoldW * g.gold) * f / total;
            if (rate > best_rate) { best_rate = rate; best = static_cast<int>(j); best_total = total; best_dmg = after; }
        }
        if (best < 0) { me.done = true; continue; }
        used[static_cast<size_t>(best)] = 1;
        routes[s].push_back(static_cast<std::uint8_t>(cand[static_cast<size_t>(best)]));
        me.t += best_total;
        me.dmg = best_dmg;
        me.cur = best;
    }
    return routes;
}
