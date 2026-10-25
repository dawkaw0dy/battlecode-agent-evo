// Draft: read the whole map once, choose a plan, write it to draft memory, pick champions for it.
#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using cambc::vec2;
using cambc::team;
using cambc::champion_draft;

namespace {

struct DraftPlan {
    bool ready = false;
    std::array<std::uint8_t, 1024> mem{};
    plan::kind kind = plan::farm;
};

std::uint8_t half_dist(std::uint16_t d) {
    if (d == cambc::distance_field::unreachable) return 255;
    return static_cast<std::uint8_t>(std::min<int>(254, d / 2));
}

std::uint16_t group_distance(const cambc::distance_field& field, const cambc::bitboard& spawns) {
    std::uint16_t best = cambc::distance_field::unreachable;
    for (auto p : spawns.positions()) best = std::min(best, field.get(p));
    return best;
}

void build_plan(DraftPlan& dp) {
    auto& m = dp.mem;
    m.fill(0);
    const int w = cambc::map::width(), h = cambc::map::height();
    const int area = w * h;
    const int team_size = static_cast<int>(cambc::draft::team_size());

    const auto& ours = cambc::map::spawn_region(team::ally);
    const auto& theirs = cambc::map::spawn_region(team::enemy);
    const auto groups = cambc::map::camp_groups();

    // Walking distances from both spawns; camp tiles are passable here (they die).
    const auto terrain = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    cambc::distance_field from_us, from_them;
    cambc::bitboard visited;
    from_us.compute(terrain | theirs, ours, visited);
    from_them.compute(terrain | ours, theirs, visited);
    const std::uint16_t spawn_gap = group_distance(from_us, theirs);

    double gem_rate = 0, boss_rate = 0, near_boss_rate = 0, cluster_rate = 0;
    const int ng = std::min<int>(plan::kMaxGroups, static_cast<int>(groups.size()));
    for (int g = 0; g < ng; ++g) {
        const auto& grp = groups[g];
        const auto spawns = grp.spawn_tiles();
        const std::uint16_t du = group_distance(from_us, spawns);
        const std::uint16_t dt = group_distance(from_them, spawns);
        bool boss = false, monster = false;
        for (const auto& c : grp.camps()) {
            if (plan::is_boss(c.kind())) boss = true;
            if (plan::camp_dps(c.kind()) > 0) monster = true;
        }
        std::uint8_t flags = 0;
        if (boss) flags |= plan::f_boss;
        if (monster) flags |= plan::f_monster;
        if (du != cambc::distance_field::unreachable && dt != cambc::distance_field::unreachable
            && dt + 6 < du) flags |= plan::f_contested;
        if (du == cambc::distance_field::unreachable) flags |= plan::f_skip;
        const int base = plan::kGroupBase + 4 * g;
        m[base + plan::gDistUs] = half_dist(du);
        m[base + plan::gDistThem] = half_dist(dt);
        m[base + plan::gFlags] = flags;

        const double rate = grp.gem_value() / static_cast<double>(std::max(10, grp.max_respawn_timer()));
        gem_rate += rate;
        if (boss) {
            boss_rate += rate;
            if (du != cambc::distance_field::unreachable && du <= dt + 4) near_boss_rate += rate;
        }
        if (!monster) cluster_rate += rate;
    }

    // Split our side's groups into one angular sector per champion, equal value each.
    {
        int sx = 0, sy = 0, n = 0;
        for (auto p : ours.positions()) { sx += p.x; sy += p.y; ++n; }
        const double cx = n ? double(sx) / n : 0, cy = n ? double(sy) / n : 0;
        struct Item { double angle, value; int g; };
        std::vector<Item> items;
        double total = 0;
        for (int g = 0; g < ng; ++g) {
            const int base = plan::kGroupBase + 4 * g;
            m[base + plan::gOwner] = 255;
            const int du = m[base + plan::gDistUs], dt = m[base + plan::gDistThem];
            if (du == 255 || dt + 2 < du) continue;
            if (m[base + plan::gFlags] & plan::f_boss) continue;
            int gx = 0, gy = 0, k = 0;
            for (auto p : groups[g].spawn_tiles().positions()) { gx += p.x; gy += p.y; ++k; }
            if (!k) continue;
            const double v = groups[g].gem_value() + 0.35 * groups[g].gold_value();
            items.push_back({std::atan2(gy / double(k) - cy, gx / double(k) - cx), v, g});
            total += v;
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.angle < b.angle; });
        double run = 0;
        for (const auto& it : items) {
            const int slot = std::min(team_size - 1, static_cast<int>((run + it.value / 2) / std::max(1.0, total) * team_size));
            m[plan::kGroupBase + 4 * it.g + plan::gOwner] = static_cast<std::uint8_t>(slot);
            run += it.value;
        }
    }

    // Choose the plan from map size, spawn gap and where the gems are.
    plan::kind kind = plan::farm;
    const bool small = area <= 1600 || (spawn_gap != cambc::distance_field::unreachable && spawn_gap <= 24);
    if (gem_rate > 0 && near_boss_rate >= 0.35 * gem_rate) kind = plan::boss;
    else if (small && cluster_rate < 0.5 * gem_rate) kind = plan::aggro;
    dp.kind = kind;

    m[plan::kMagic] = 'M';
    m[plan::kKind] = kind;
    m[plan::kBank] = kind == plan::aggro ? 30 : 40;
    m[plan::kTeam] = static_cast<std::uint8_t>(team_size);
    m[plan::kGroups] = static_cast<std::uint8_t>(ng);
    for (int i = 0; i < 12; ++i) m[plan::kSlots + i] = 255;
    dp.ready = true;
}

} // namespace

void play_draft() {
    static DraftPlan dp;
    if (!dp.ready) build_plan(dp);

    const auto chosen = cambc::draft::current_draft(team::ally);
    // Preference lists per plan; all strength champions so the shop plan fits everyone.
    static const std::array<champion_draft, 7> farm_list{champion_draft::vesper_vale, champion_draft::varg,
        champion_draft::vendetta, champion_draft::marcella, champion_draft::tavi, champion_draft::ryth,
        champion_draft::ren};
    static const std::array<champion_draft, 7> boss_list{champion_draft::marcella, champion_draft::varg,
        champion_draft::vendetta, champion_draft::vesper_vale, champion_draft::tavi, champion_draft::ryth,
        champion_draft::ren};
    static const std::array<champion_draft, 7> aggro_list{champion_draft::vendetta, champion_draft::tavi,
        champion_draft::marcella, champion_draft::vesper_vale, champion_draft::varg, champion_draft::ryth,
        champion_draft::ren};
    const auto& list = dp.kind == plan::boss ? boss_list : dp.kind == plan::aggro ? aggro_list : farm_list;

    champion_draft pick = list[0];
    for (auto c : list)
        if (std::find(chosen.begin(), chosen.end(), c) == chosen.end()) { pick = c; break; }

    // Slot table: draft order of our picks, keyed by champion_type (no Cassi & Issac in our lists,
    // so champion_draft and champion_type agree up to vesper_vale/vendetta; map explicitly).
    auto type_of = [](champion_draft d) -> int {
        switch (d) {
        case champion_draft::ryth: return static_cast<int>(cambc::champion_type::ryth);
        case champion_draft::vesper_vale: return static_cast<int>(cambc::champion_type::vesper_vale);
        case champion_draft::vendetta: return static_cast<int>(cambc::champion_type::vendetta);
        case champion_draft::marcella: return static_cast<int>(cambc::champion_type::marcella);
        case champion_draft::ren: return static_cast<int>(cambc::champion_type::ren);
        case champion_draft::varg: return static_cast<int>(cambc::champion_type::varg);
        case champion_draft::tavi: return static_cast<int>(cambc::champion_type::tavi);
        default: return -1;
        }
    };
    int slot = 0;
    for (auto c : chosen) {
        const int t = type_of(c);
        if (t >= 0 && t < 12) dp.mem[plan::kSlots + t] = static_cast<std::uint8_t>(slot);
        ++slot;
    }
    const int t = type_of(pick);
    if (t >= 0 && t < 12) dp.mem[plan::kSlots + t] = static_cast<std::uint8_t>(slot);

    cambc::draft_memory = dp.mem;
    cambc::draft::pick(pick);
}
