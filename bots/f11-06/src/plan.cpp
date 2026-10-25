#include "plan.h"
#include <cambc.h>

using cd = cambc::champion_draft;

#ifndef EVO_SQUAD
#define EVO_SQUAD 0
#endif
constexpr int kSquadMode = EVO_SQUAD;   // 0 solo, 1 pairs, 2 one ball
#ifndef EVO_GOLEM_POW
#define EVO_GOLEM_POW 75
#endif
#ifndef EVO_MONO_POW
#define EVO_MONO_POW 110
#endif
#ifndef EVO_RAID_ROUND
#define EVO_RAID_ROUND 600
#endif
constexpr int kGolemPow = EVO_GOLEM_POW, kMonoPow = EVO_MONO_POW, kRaidRound = EVO_RAID_ROUND;
#ifndef EVO_RAID_ALWAYS
#define EVO_RAID_ALWAYS 0
#endif
constexpr bool kRaidAlways = EVO_RAID_ALWAYS;   // raid bosses on every map (measured worse: deaths at the boss)

Plan make_plan(const MapInfo& m, int team_size) {
    Plan p;
    const int n = static_cast<int>(m.groups.size());
    p.cls.assign(static_cast<size_t>(n), kGHome);

    int home_gems = 0, cluster_gems = 0, boss_gems = 0, all_gems = 0;
    int best_golem = -1, best_golem_d = kFar, best_mono = -1, best_mono_d = kFar;
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
        if (g.dA < kFar) {
            all_gems += g.gems;
            if (g.boss) boss_gems += g.gems;
        }
        if (g.boss && g.dA < kFar && g.dA <= g.dE + 2) {
            if (g.mono && g.dA < best_mono_d) {
                best_mono = i;
                best_mono_d = g.dA;
            } else if (!g.mono && g.dA < best_golem_d) {
                best_golem = i;
                best_golem_d = g.dA;
            }
        }
    }

    p.mode = kModeFarm;
    if (m.spawn_gap <= 30) p.mode = kModeSmall;
    else if (home_gems > 0 && cluster_gems * 2 >= home_gems) p.mode = kModeCluster;
    // Bosses: a golem first (easier), the monolith second. Where bosses hold most of the gems the raid
    // starts early; otherwise once a champion is strong enough, or late in the game.
    const bool boss_map = all_gems > 0 && boss_gems * 5 >= all_gems * 4;
    if (!boss_map && !kRaidAlways) best_golem = best_mono = -1;
    if (best_golem >= 0) {
        p.raid = best_golem;
        p.raid_pow = kGolemPow;
        if (best_mono >= 0) {
            p.raid2 = best_mono;
            p.raid2_pow = kMonoPow;
        }
    } else if (best_mono >= 0) {
        p.raid = best_mono;
        p.raid_pow = boss_map ? kGolemPow : kMonoPow;
    }
    if (p.raid >= 0) {
        p.raid_round = boss_map ? 200 : kRaidRound;
        if (p.mode == kModeFarm) p.mode = kModeBoss;
    }
    p.bank = m.spawn_gap <= 30 ? 25 : (m.spawn_gap >= 90 ? 60 : 40);
    if (p.mode == kModeSmall) p.flags |= planflag::kAggro;
    // Squads: one ball on small maps, solo farmers elsewhere.
    for (int i = 0; i < 5; ++i) {
        int sq = i;
        if (kSquadMode == 1) sq = i / 2;
        else if (kSquadMode == 2) sq = 0;
        p.squad[static_cast<size_t>(i)] = static_cast<std::uint8_t>(sq);
    }

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
    put(dm::raid2, p.raid2 + 1);
    put(dm::raid2_pow, p.raid2_pow / 4);
    put(dm::raid_round, p.raid_round / 8);
    put(dm::flags, p.flags);
    for (int i = 0; i < 5; ++i) put(dm::squads + i, p.squad[static_cast<size_t>(i)]);
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
    p.raid2 = get(dm::raid2) - 1;
    p.raid2_pow = get(dm::raid2_pow) * 4;
    p.raid_round = get(dm::raid_round) * 8;
    p.flags = static_cast<std::uint8_t>(get(dm::flags));
    for (int i = 0; i < 5; ++i) p.squad[static_cast<size_t>(i)] = static_cast<std::uint8_t>(get(dm::squads + i));
    const int n = static_cast<int>(m.groups.size());
    const int stored = get(dm::ngroups);
    p.cls.assign(static_cast<size_t>(n), kGHome);
    for (int i = 0; i < n && i < stored; ++i) p.cls[static_cast<size_t>(i)] = static_cast<std::uint8_t>(get(dm::group + i));
    return p;
}
