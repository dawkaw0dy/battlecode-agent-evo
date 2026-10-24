#include "plan.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using cambc::vec2;
using cambc::team;

namespace {
struct MonsterSpec { float hp, str; int range; };

MonsterSpec spec(cambc::camp_type type) {
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

vec2 region_center(const cambc::bitboard& region) {
    const auto tiles = region.positions();
    if (tiles.empty()) return {0, 0};
    long sx = 0, sy = 0;
    for (auto t : tiles) { sx += t.x; sy += t.y; }
    return {static_cast<int>(sx / static_cast<long>(tiles.size())), static_cast<int>(sy / static_cast<long>(tiles.size()))};
}
} // namespace

GroupFacts group_facts(const cambc::map::camp_group& group) {
    GroupFacts facts;
    facts.gems = static_cast<int>(group.gem_value());
    facts.gold = static_cast<int>(group.gold_value());
    long sx = 0, sy = 0;
    for (const auto& member : group.camps()) {
        const auto s = spec(member.kind());
        facts.hp += s.hp;
        if (s.str > 0) {
            facts.monster_hp += s.hp;
            facts.monsters.emplace_back(s.hp, s.str);
        }
        facts.str += s.str;
        facts.range = std::max(facts.range, s.range);
        if (member.kind() == cambc::camp_type::golem) facts.golem = true;
        if (member.kind() == cambc::camp_type::monolith) facts.monolith = true;
        const auto p = member.spawn_position();
        facts.tiles.push_back(p);
        sx += p.x;
        sy += p.y;
    }
    // Kill the most damage per health first: that order minimises damage taken.
    std::sort(facts.monsters.begin(), facts.monsters.end(),
        [](const auto& a, const auto& b) { return a.second / a.first > b.second / b.first; });
    if (!facts.tiles.empty())
        facts.center = {static_cast<int>(sx / static_cast<long>(facts.tiles.size())),
            static_cast<int>(sy / static_cast<long>(facts.tiles.size()))};
    return facts;
}

float clear_damage(const GroupFacts& facts, float dps) {
    float elapsed = 0, damage = 0;
    for (const auto& [hp, str] : facts.monsters) {
        elapsed += hp / dps;
        damage += str * elapsed;
    }
    return damage;
}

cambc::bitboard static_walls() {
    return cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(team::enemy);
}

void build_plan(Plan& plan) {
    plan = Plan{};
    plan.magic = plan_magic;
    const auto groups = cambc::map::camp_groups();
    const int ngroups = std::min<int>(static_cast<int>(groups.size()), max_groups);
    plan.ngroups = static_cast<std::uint8_t>(ngroups);

    const int width = cambc::map::width(), height = cambc::map::height();
    const int area = width * height;

    // Walking distances from our spawn.
    const auto walls = static_walls();
    cambc::distance_field from_spawn;
    cambc::bitboard reached;
    from_spawn.compute(walls, cambc::map::spawn_region(team::ally), reached);

    const auto spawn_center = region_center(cambc::map::spawn_region(team::ally));

    // Distance between the two spawns, ignoring the enemy-spawn wall.
    const auto open = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    cambc::distance_field from_enemy;
    cambc::bitboard enemy_reached;
    from_enemy.compute(open, cambc::map::spawn_region(team::enemy), enemy_reached);
    {
        int best = 9999;
        for (auto t : cambc::map::spawn_region(team::ally).positions())
            if (enemy_reached.is_set(t)) best = std::min<int>(best, from_enemy.get(t));
        plan.spawn_gap = static_cast<std::uint16_t>(std::min(best, 9999));
    }

    // Altar distance from spawn.
    int altar_dist = 9999;
    for (auto altar : cambc::map::altars().positions())
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const vec2 t{altar.x + dx, altar.y + dy};
                if (!cambc::map::is_in_bounds(t) || !reached.is_set(t)) continue;
                altar_dist = std::min<int>(altar_dist, from_spawn.get(t));
            }

    struct Entry { int index; double angle; };
    std::vector<Entry> reachable;
    int easy = 0, reachable_count = 0, total_gems = 0;
    long dist_sum = 0;
    std::uint16_t checksum = 0;
    for (int i = 0; i < ngroups; ++i) {
        const auto facts = group_facts(groups[i]);
        checksum = static_cast<std::uint16_t>(checksum + facts.gems * 7 + facts.gold);
        int best = 9999;
        for (auto t : facts.tiles)
            if (reached.is_set(t)) best = std::min<int>(best, from_spawn.get(t));
        int enemy_best = 9999;
        for (auto t : facts.tiles)
            if (enemy_reached.is_set(t)) enemy_best = std::min<int>(enemy_best, from_enemy.get(t));
        std::uint8_t flags = 0;
        if (best < 9999) flags |= group_reachable;
        if (enemy_best * 10 < best * 8) flags |= group_enemy_side;
        if (facts.golem || facts.monolith) flags |= group_big;
        plan.flags[i] = flags;
        if ((flags & group_reachable) && !facts.monolith && facts.gems > 0) {
            ++reachable_count;
            total_gems += facts.gems;
            dist_sum += best;
            if (facts.monster_hp <= 1500) ++easy;
            reachable.push_back({i, std::atan2(static_cast<double>(facts.center.y - spawn_center.y),
                static_cast<double>(facts.center.x - spawn_center.x))});
        }
    }
    plan.checksum = checksum;

    // Angular order around our spawn; the widest gap becomes the cut point.
    std::sort(reachable.begin(), reachable.end(), [](const Entry& a, const Entry& b) { return a.angle < b.angle; });
    if (!reachable.empty()) {
        std::size_t cut = 0;
        double widest = -1;
        for (std::size_t k = 0; k < reachable.size(); ++k) {
            const double next = k + 1 < reachable.size() ? reachable[k + 1].angle : reachable[0].angle + 2 * M_PI;
            if (next - reachable[k].angle > widest) { widest = next - reachable[k].angle; cut = (k + 1) % reachable.size(); }
        }
        const std::size_t n = reachable.size();
        for (std::size_t k = 0; k < n; ++k) {
            const auto& entry = reachable[(cut + k) % n];
            plan.zone[entry.index] = static_cast<std::uint8_t>(n > 1 ? (k * 255) / (n - 1) : 0);
        }
    }

    unsigned team_size = 3;
    try { team_size = cambc::draft::team_size(); } catch (...) {}
    plan.team_size = static_cast<std::uint8_t>(team_size);

    const int avg_dist = reachable_count ? static_cast<int>(dist_sum / reachable_count) : 0;
    plan.altar_far = static_cast<std::uint8_t>(std::min(altar_dist, 255));

    // Strategy selection from the map's numbers.
    if (plan.spawn_gap <= 32 && area <= 1600)
        plan.mode = mode_skirmish;
    else if (reachable_count >= 3 * static_cast<int>(team_size) && area >= 1800 && easy >= static_cast<int>(team_size))
        plan.mode = mode_spread;
    else
        plan.mode = mode_pack;
    plan.chase = (plan.mode == mode_skirmish || plan.spawn_gap <= 45) ? 1 : 0;

    // Bank more per trip when altars are far from the farming grounds.
    plan.bank_gems = static_cast<std::uint8_t>(std::clamp(20 + avg_dist / 2, 25, 60));
    // Golems need a strong team (x10 dps); starved maps lower the bar.
    plan.golem_dps = total_gems < 150 ? 22 : 30;
    plan.gemless = total_gems == 0 ? 1 : 0;
}

const Plan& the_plan() {
    static const Plan plan = [] {
        Plan result;
        bool ok = cambc::draft_memory.size() >= sizeof(Plan);
        if (ok) {
            std::memcpy(&result, cambc::draft_memory.data(), sizeof(Plan));
            ok = result.magic == plan_magic;
        }
        if (!ok) build_plan(result);
        return result;
    }();
    return plan;
}
