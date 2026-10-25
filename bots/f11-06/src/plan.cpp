#include "plan.h"
#include <cambc.h>

using cd = cambc::champion_draft;

Plan make_plan(const MapInfo& m, int team_size) {
    Plan p;
    const int n = static_cast<int>(m.groups.size());
    p.cls.assign(static_cast<size_t>(n), kGHome);

    int home_gems = 0, cluster_gems = 0;
    int best_boss = -1, best_boss_d = kFar;
    for (int i = 0; i < n; ++i) {
        const auto& g = m.groups[static_cast<size_t>(i)];
        std::uint8_t c;
        if (g.dA >= kFar) c = kGUnreachable;
        else if (g.boss) c = kGBoss;
        else if (g.dA <= g.dE + 1) c = kGHome;
        else if (g.dA <= g.dE + 8) c = kGContested;
        else c = kGEnemy;
        p.cls[static_cast<size_t>(i)] = c;
        if (c == kGHome) {
            home_gems += g.gems;
            for (size_t k = 0; k < g.kinds.size(); ++k)
                if (g.kinds[k] == cambc::camp_type::gem_cluster) cluster_gems += 7;
        }
        if (g.boss && g.dA < kFar && g.dA <= g.dE + 2 && g.dA < best_boss_d) {
            best_boss = i;
            best_boss_d = g.dA;
        }
    }

    p.mode = kModeFarm;
    if (m.spawn_gap <= 30) p.mode = kModeSmall;
    else if (home_gems > 0 && cluster_gems * 2 >= home_gems) p.mode = kModeCluster;
    if (best_boss >= 0) {
        p.raid = best_boss;
        p.raid_pow = m.groups[static_cast<size_t>(best_boss)].mono ? 160 : 90;
        if (p.mode == kModeFarm) p.mode = kModeBoss;
    }
    p.bank = m.spawn_gap <= 30 ? 25 : (m.spawn_gap >= 90 ? 60 : 40);
    if (p.mode == kModeSmall) p.flags |= planflag::kAggro;

    std::vector<cd> order{cd::vendetta, cd::vesper_vale, cd::marcella, cd::tavi, cd::varg};
    order.resize(static_cast<size_t>(std::clamp(team_size, 1, 5)));
    p.picks = order;
    return p;
}

void write_plan(const Plan& p, const MapInfo& m, std::span<std::byte> mem) {
    auto put = [&](int at, int v) { mem[static_cast<size_t>(at)] = static_cast<std::byte>(v & 255); };
    put(dm::magic, 'M');
    put(dm::npicks, static_cast<int>(p.picks.size()));
    for (size_t i = 0; i < p.picks.size() && i < 5; ++i) put(dm::picks + static_cast<int>(i), static_cast<int>(p.picks[i]));
    put(dm::mode, p.mode);
    put(dm::bank, p.bank);
    put(dm::raid, p.raid + 1);
    put(dm::raid_pow, p.raid_pow / 4);
    put(dm::flags, p.flags);
    const int n = std::min<int>(static_cast<int>(m.groups.size()), dm::kMaxGroups);
    put(dm::ngroups, n);
    for (int i = 0; i < n; ++i) put(dm::group + i, p.cls[static_cast<size_t>(i)]);
}

Plan read_plan(const MapInfo& m) {
    const auto& mem = cambc::draft_memory;
    auto get = [&](int at) {
        if (static_cast<size_t>(at) >= mem.size()) return 0;
        return static_cast<int>(static_cast<std::uint8_t>(mem[static_cast<size_t>(at)]));
    };
    if (get(dm::magic) != 'M') return make_plan(m, 5);
    Plan p;
    const int np = std::min(get(dm::npicks), 5);
    for (int i = 0; i < np; ++i) p.picks.push_back(static_cast<cd>(get(dm::picks + i)));
    p.mode = static_cast<std::uint8_t>(get(dm::mode));
    p.bank = get(dm::bank);
    p.raid = get(dm::raid) - 1;
    p.raid_pow = get(dm::raid_pow) * 4;
    p.flags = static_cast<std::uint8_t>(get(dm::flags));
    const int n = static_cast<int>(m.groups.size());
    const int stored = get(dm::ngroups);
    p.cls.assign(static_cast<size_t>(n), kGHome);
    for (int i = 0; i < n && i < stored; ++i) p.cls[static_cast<size_t>(i)] = static_cast<std::uint8_t>(get(dm::group + i));
    return p;
}
