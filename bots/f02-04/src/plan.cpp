#include "plan.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstddef>
#include <span>

using cambc::vec2;
using cambc::team;

namespace {

struct CampDesign { float hp, str; int range; };

CampDesign design(cambc::camp_type type) {
    switch (type) {
    case cambc::camp_type::wolf: return {1350, 20, 3};
    case cambc::camp_type::elk: return {2400, 60, 4};
    case cambc::camp_type::gem_cluster: return {120, 0, 0};
    case cambc::camp_type::gold_cluster: return {120, 0, 0};
    case cambc::camp_type::monolith: return {20000, 200, 5};
    case cambc::camp_type::golem: return {5000, 120, 4};
    }
    return {1000, 50, 3};
}

vec2 region_center(const cambc::bitboard& board) {
    long sx = 0, sy = 0, n = 0;
    for (auto p : board.positions()) { sx += p.x; sy += p.y; ++n; }
    if (!n) return {0, 0};
    return {static_cast<int>(sx / n), static_cast<int>(sy / n)};
}

int min_over(const cambc::distance_field& field, const cambc::bitboard& visited, const std::vector<vec2>& tiles) {
    int best = 9999;
    for (auto t : tiles)
        if (visited.is_set(t)) best = std::min<int>(best, field.get(t));
    return best;
}

} // namespace

const cambc::bitboard& static_walls_ally() {
    static const cambc::bitboard walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    return walls;
}

const cambc::bitboard& dash_walls_ally() {
    static const cambc::bitboard walls = [] {
        const auto walkable = ~static_walls_ally();
        const auto& holes = cambc::map::holes();
        cambc::bitboard thin;
        thin.clear();
        const int d[2][2] = {{1, 0}, {0, 1}};  // straight jumps only: diagonal ones can be corner-blocked
        for (const auto& v : d) {
            const auto a = walkable.shift_right(v[0]).shift_up(v[1]);
            const auto b = walkable.shift_right(-v[0]).shift_up(-v[1]);
            thin |= holes & a & b;
        }
        return static_walls_ally() & ~thin;
    }();
    return walls;
}

std::vector<GroupData> analyse_groups() {
    std::vector<GroupData> out;
    const auto groups = cambc::map::camp_groups();
    auto walls_a = static_walls_ally();
    auto walls_e = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::ally);
    cambc::distance_field fa, fe, fd;
    cambc::bitboard va, ve, vd;
    fa.compute(walls_a, cambc::map::spawn_region(team::ally), va);
    fd.compute(dash_walls_ally(), cambc::map::spawn_region(team::ally), vd);
    fe.compute(walls_e, cambc::map::spawn_region(team::enemy), ve);
    for (const auto& g : groups) {
        GroupData d;
        long sx = 0, sy = 0;
        for (const auto& c : g.camps()) {
            auto p = c.spawn_position();
            d.tiles.push_back(p);
            sx += p.x; sy += p.y;
            auto des = design(c.kind());
            d.hp += des.hp;
            d.dps += des.str;
            d.range = std::max(d.range, des.range);
            d.max_str = std::max(d.max_str, des.str);
            if (c.kind() == cambc::camp_type::monolith) d.has_monolith = true;
            if (c.kind() == cambc::camp_type::golem) d.has_golem = true;
        }
        if (!d.tiles.empty())
            d.center = {static_cast<int>(sx / static_cast<long>(d.tiles.size())), static_cast<int>(sy / static_cast<long>(d.tiles.size()))};
        d.gems = static_cast<int>(g.gem_value());
        d.gold = static_cast<int>(g.gold_value());
        d.respawn = g.max_respawn_timer();
        d.dist_ally = min_over(fa, va, d.tiles);
        d.dist_enemy = min_over(fe, ve, d.tiles);
        d.dist_ally_dash = min_over(fd, vd, d.tiles);
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<std::uint8_t> split_regions(const std::vector<GroupData>& groups, int slots, bool dash_map) {
    std::vector<std::uint8_t> owner(groups.size(), 255);
    if (slots <= 0 || groups.empty()) return owner;
    const vec2 home = region_center(cambc::map::spawn_region(team::ally));
    const vec2 away = region_center(cambc::map::spawn_region(team::enemy));
    const double fx = away.x - home.x, fy = away.y - home.y;

    // Our half of the map plus contested ground; solo-impossible groups stay unassigned.
    std::vector<int> eligible;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        const auto& g = groups[i];
        if (g.has_monolith || g.dps >= 120) continue;
        const int da = dash_map ? g.dist_ally_dash : g.dist_ally;
        if (da >= 9999) continue;
        if (da <= g.dist_enemy + 2 || (dash_map && g.dist_enemy >= 9999)) eligible.push_back(i);
    }
    if (eligible.empty()) return owner;

    // Sort by bearing from our spawn relative to the enemy direction, then cut into equal-value sectors.
    std::vector<std::pair<double, int>> order;
    for (int i : eligible) {
        const double dx = groups[i].center.x - home.x, dy = groups[i].center.y - home.y;
        const double angle = std::atan2(fx * dy - fy * dx, fx * dx + fy * dy);
        order.push_back({angle, i});
    }
    std::sort(order.begin(), order.end());
    std::vector<double> weight;
    double total = 0;
    for (auto& [a, i] : order) {
        const auto& g = groups[i];
        const double w = g.gems + 0.3 * g.gold + 4.0 + g.hp / 400.0;
        weight.push_back(w);
        total += w;
    }
    const int regions = std::min<int>(slots, static_cast<int>(order.size()));
    std::vector<int> chunk(order.size());
    double acc = 0;
    for (size_t k = 0; k < order.size(); ++k) {
        const double mid = acc + weight[k] / 2;
        chunk[k] = std::min(regions - 1, static_cast<int>(mid / total * regions));
        acc += weight[k];
    }
    // Hardest sector to the strongest slot.
    std::vector<double> difficulty(regions, 0);
    for (size_t k = 0; k < order.size(); ++k) difficulty[chunk[k]] += groups[order[k].second].hp * (1 + groups[order[k].second].dps / 20.0);
    std::vector<int> rank(regions);
    std::iota(rank.begin(), rank.end(), 0);
    std::stable_sort(rank.begin(), rank.end(), [&](int a, int b) { return difficulty[a] > difficulty[b]; });
    std::vector<int> slot_of(regions);
    for (int s = 0; s < regions; ++s) slot_of[rank[s]] = s;
    for (size_t k = 0; k < order.size(); ++k) owner[order[k].second] = static_cast<std::uint8_t>(slot_of[chunk[k]]);
    return owner;
}

bool is_dash_map(const std::vector<GroupData>& groups) {
    float walk = 0, dash = 0;
    for (const auto& g : groups) {
        const float v = g.gems + 0.3f * g.gold;
        if (g.dist_ally < 9999) walk += v;
        if (g.dist_ally_dash < 9999) dash += v;
    }
    return dash > 0 && walk < 0.6f * dash;
}

std::vector<cambc::champion_draft> preferred_picks(int team_size, bool dash_map, bool raid) {
    using cd = cambc::champion_draft;
    std::vector<cd> all{cd::vendetta, cd::marcella, cd::vesper_vale, cd::tavi, cd::varg, cd::ryth};
    (void)raid;
    // Most camps behind one-tile holes: take the champions that can dash over them.
    if (dash_map) all = {cd::vendetta, cd::vesper_vale, cd::heart_of_ruin, cd::ren, cd::marcella, cd::tavi};
    all.resize(std::min<size_t>(all.size(), std::max(team_size, 1)));
    return all;
}

namespace {

// Raiders are taken in this order: tanky melee first (only the nearest champion is hit), then damage.
int raid_rank(cambc::champion_draft c) {
    using cd = cambc::champion_draft;
    switch (c) {
    case cd::marcella: return 0;
    case cd::vendetta: return 1;
    case cd::varg: return 2;
    case cd::ryth: return 3;
    case cd::heart_of_ruin: return 4;
    case cd::vesper_vale: return 5;
    case cd::tavi: return 6;
    case cd::ren: return 7;
    default: return 8;
    }
}

// The group whose camps stand on every altar tile we could reach (a boss beside the only altar), or -1.
int altar_blocker(const std::vector<GroupData>& groups, bool dash_map) {
    const auto& walls = dash_map ? dash_walls_ally() : static_walls_ally();
    cambc::bitboard camp_tiles;
    camp_tiles.clear();
    for (const auto& g : groups)
        for (auto t : g.tiles) camp_tiles.set(t);
    cambc::bitboard beside;
    beside.clear();
    const int w = cambc::map::width(), h = cambc::map::height();
    for (auto a : cambc::map::altars().positions())
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const cambc::vec2 p{a.x + dx, a.y + dy};
                if (p.x >= 0 && p.y >= 0 && p.x < w && p.y < h) beside.set(p);
            }
    beside &= ~static_walls_ally();
    cambc::distance_field f;
    cambc::bitboard reached;
    f.compute(walls | camp_tiles, cambc::map::spawn_region(team::ally), reached);
    if (!(reached & beside & ~camp_tiles).is_empty()) return -1;
    // Every free altar tile is out of reach: the nearest group standing on one must be cleared.
    int best = -1, best_d = 9999;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        bool on = false;
        for (auto t : groups[i].tiles) on |= beside.is_set(t);
        const int d = dash_map ? groups[i].dist_ally_dash : groups[i].dist_ally;
        if (on && d < best_d) { best_d = d; best = i; }
    }
    return best;
}

} // namespace

Plan make_plan(const std::vector<GroupData>& groups, int team_size) {
    Plan plan;
    plan.dash_map = is_dash_map(groups);
    plan.blocker = altar_blocker(groups, plan.dash_map);
    // Boss groups we can reach first (or about as soon as the enemy), and the value of everything else.
    std::vector<std::pair<float, int>> bosses;
    float boss_rate = 0, small_rate = 0;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        const auto& g = groups[i];
        const int da = plan.dash_map ? g.dist_ally_dash : g.dist_ally;
        if (da >= 9999) continue;
        const float rate = g.gems / static_cast<float>(std::max(40, g.respawn));
        if (g.boss() || i == plan.blocker) {
            if (da > g.dist_enemy + 6 && i != plan.blocker) continue;
            bosses.push_back({-(g.gems + 0.3f * g.gold) / (da + 30.0f) - (i == plan.blocker ? 1000.0f : 0.0f), i});
            boss_rate += rate;
        } else if (da <= g.dist_enemy + 2) {
            small_rate += rate;
        }
    }
    std::sort(bosses.begin(), bosses.end());
    for (size_t k = 0; k < bosses.size() && k < 8; ++k) plan.raid_list.push_back(bosses[k].second);
    const float share = boss_rate / std::max(0.01f, boss_rate + small_rate);
    // Squad raids only pay where the altars are blocked: a general boss raid lost 0/6 against f00-03
    // (champions died to the monolith one by one), so other boss maps stay with the split farm.
    int raiders = 0;
    if (!plan.raid_list.empty() && plan.blocker >= 0) raiders = team_size;
    (void)share;
    plan.mode = raiders == 0 ? Plan::split : (raiders >= team_size ? Plan::raid : Plan::mixed);
    plan.picks = preferred_picks(team_size, plan.dash_map, raiders > 0);
    std::vector<int> order(plan.picks.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return raid_rank(plan.picks[a]) < raid_rank(plan.picks[b]); });
    for (int k = 0; k < raiders && k < static_cast<int>(order.size()); ++k) plan.raiders |= static_cast<std::uint8_t>(1u << order[k]);
    plan.need = std::clamp(raiders - 1, 2, 4);
    if (raiders < 2) plan.need = 1;
    plan.owner = split_regions(groups, static_cast<int>(plan.picks.size()), plan.dash_map);
    return plan;
}

void write_plan(const Plan& plan, int group_count) {
    std::array<std::byte, 1024> mem{};
    auto put = [&](int at, int v) { mem[static_cast<size_t>(at)] = static_cast<std::byte>(v & 0xff); };
    put(layout::magic, 'R');
    put(layout::team_size, static_cast<int>(plan.picks.size()));
    for (size_t i = 0; i < plan.picks.size() && i < 5; ++i) put(layout::picks + static_cast<int>(i), static_cast<int>(plan.picks[i]));
    const int count = std::min(group_count, layout::max_groups);
    put(layout::group_count, count);
    for (int i = 0; i < count; ++i) put(layout::regions + i, plan.owner[static_cast<size_t>(i)]);
    put(layout::dash_map, plan.dash_map ? 1 : 0);
    put(layout::mode, plan.mode);
    put(layout::raiders, plan.raiders);
    put(layout::raid_count, static_cast<int>(plan.raid_list.size()));
    for (size_t k = 0; k < plan.raid_list.size() && k < 8; ++k) put(layout::raid_list + static_cast<int>(k), plan.raid_list[k]);
    put(layout::blocker, plan.blocker < 0 ? 255 : plan.blocker);
    put(layout::need, plan.need);
    cambc::draft_memory = std::span<const std::byte>{mem.data(), mem.size()};
}

bool read_plan(Plan& plan, int group_count) {
    const auto* mem = cambc::draft_memory.data();
    const size_t size = cambc::draft_memory.size();
    if (size <= static_cast<size_t>(layout::need) || static_cast<char>(mem[layout::magic]) != 'R') return false;
    auto get = [&](int at) { return static_cast<int>(mem[static_cast<size_t>(at)]); };
    if (get(layout::group_count) != std::min(group_count, layout::max_groups) || group_count > layout::max_groups) return false;
    plan.picks.clear();
    for (int i = 0; i < get(layout::team_size) && i < 5; ++i) plan.picks.push_back(static_cast<cambc::champion_draft>(get(layout::picks + i)));
    plan.owner.clear();
    for (int i = 0; i < group_count; ++i) plan.owner.push_back(static_cast<std::uint8_t>(get(layout::regions + i)));
    plan.dash_map = get(layout::dash_map) != 0;
    plan.mode = static_cast<Plan::Mode>(get(layout::mode));
    plan.raiders = static_cast<std::uint8_t>(get(layout::raiders));
    plan.raid_list.clear();
    for (int k = 0; k < get(layout::raid_count) && k < 8; ++k) {
        const int g = get(layout::raid_list + k);
        if (g < group_count) plan.raid_list.push_back(g);
    }
    const int b = get(layout::blocker);
    plan.blocker = b < group_count ? b : -1;
    plan.need = std::max(1, get(layout::need));
    return true;
}
